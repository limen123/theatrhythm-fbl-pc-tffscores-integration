// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "common/thread.h"
#include "core/emulator_settings.h"
#include "core/libraries/videoout/frame_pacing.h"
#include "theatrhythm_gpu.h"
#include "video_core/renderer_vulkan/vk_async_skip_budget.h"

extern "C" void runtime_tffscores_trigger_auth_hotkey(void) {}
extern "C" int runtime_memory_is_mapped(std::uintptr_t, std::uint64_t) { return 0; }

using namespace std::chrono;
using Libraries::VideoOut::AdvanceFlipDeadline;
using Libraries::VideoOut::AdvanceVblankDeadline;
using Libraries::VideoOut::ComputePacingDecision;
using Vulkan::AsyncSkipWindow;
using Vulkan::IsAsyncSkipBudgetSpent;
using Vulkan::IsTargetStillRecent;
using Vulkan::ShouldForgetRecentTarget;

static void TestPacingDecisions() {
    // Unlimited (-1): never throttled in software, regardless of VSync or display rate.
    {
        const auto p = ComputePacingDecision(-1, false, 143.99, 60.0, false, 0, true);
        assert(p.immediate_flips);
        assert(!p.follow_display);
        assert(p.frame_period.count() == 0);
    }
    {
        const auto p = ComputePacingDecision(-1, true, 60.0, 60.0, false, 0, true);
        assert(p.immediate_flips);
        assert(!p.follow_display);
        assert(p.frame_period.count() == 0);
    }

    // Follow display (0) + VSync on:
    // - 59.95 Hz panel locks 1:1 to DXGI vblank (no drifting software timer).
    {
        const auto p = ComputePacingDecision(0, true, 59.95, 60.0, false, 0, true);
        assert(!p.immediate_flips);
        assert(p.follow_display);
        assert(p.frame_period.count() == 0);
    }
    // - 143.99 Hz panel paces flips evenly to the exact display rate so Fifo does not
    //   burst-queue and oscillate against async present.
    {
        const auto p = ComputePacingDecision(0, true, 143.99, 60.0, false, 0, true);
        assert(p.immediate_flips);
        assert(!p.follow_display);
        assert(std::llabs(p.frame_period.count() - 6'944'927LL) <= 2);
    }

    // Follow display (0) + VSync off: software paces cleanly to exact fractional display refresh.
    {
        const auto p = ComputePacingDecision(0, false, 143.99, 60.0, false, 0, true);
        assert(p.immediate_flips);
        assert(!p.follow_display);
        assert(std::llabs(p.frame_period.count() - 6'944'927LL) <= 2);
    }

    // All explicit FPS options with VSync off on 144 Hz (and 60 Hz):
    for (const int fps : {30, 100, 120, 144, 180, 240}) {
        const auto p = ComputePacingDecision(fps, false, 143.99, 60.0, false, 0, true);
        assert(p.immediate_flips);
        assert(!p.follow_display);
        const double expected_hz = fps == 144 ? 143.99 : double(fps);
        const s64 expected_ns = s64(1e9 / expected_hz + 0.5);
        assert(std::llabs(p.frame_period.count() - expected_ns) <= 1);
    }

    // 60 FPS on a 144/280 Hz panel with 60 Hz vblank keeps the fixed 1-flip-per-vblank timer.
    for (const bool vs : {false, true}) {
        const auto p60 = ComputePacingDecision(60, vs, 144.0, 60.0, false, 0, true);
        assert(!p60.immediate_flips);
        assert(!p60.follow_display);
        assert(p60.frame_period.count() == 0);
    }

    // Explicit FPS cap with VSync on:
    // - Slower than panel (30 on 60 Hz, 120 on 280 Hz) uses software cap.
    {
        const auto p120 = ComputePacingDecision(120, true, 280.0, 60.0, false, 0, true);
        assert(p120.immediate_flips);
        assert(std::llabs(p120.frame_period.count() - 8'333'333LL) <= 1);
        const auto p30 = ComputePacingDecision(30, true, 60.0, 60.0, false, 0, true);
        assert(p30.immediate_flips);
        assert(std::llabs(p30.frame_period.count() - 33'333'333LL) <= 1);
    }
    // - Meeting/exceeding 60 Hz display with VSync on locks to DXGI vblank instead of phase drift.
    {
        const auto p60 = ComputePacingDecision(60, true, 60.0, 60.0, false, 0, true);
        assert(!p60.immediate_flips);
        assert(p60.follow_display);
        assert(p60.frame_period.count() == 0);
        const auto p240 = ComputePacingDecision(240, true, 60.0, 60.0, false, 0, true);
        assert(!p240.immediate_flips);
        assert(p240.follow_display);
        assert(p240.frame_period.count() == 0);
    }
    // - Meeting/exceeding 144 Hz display with VSync on clamps flip cadence to display rate.
    {
        const auto p144 = ComputePacingDecision(144, true, 144.0, 60.0, false, 0, true);
        assert(p144.immediate_flips);
        assert(!p144.follow_display);
        assert(std::llabs(p144.frame_period.count() - 6'944'444LL) <= 1);
    }

    // THEATRHYTHM_FPS_LIMIT measurement overrides bypass snapping.
    {
        const auto p_zero = ComputePacingDecision(-1, true, 60.0, 60.0, true, 0, true);
        assert(!p_zero.immediate_flips);
        assert(p_zero.follow_display);
        const auto p_exact = ComputePacingDecision(0, false, 143.99, 60.0, true, 144, true);
        assert(p_exact.immediate_flips);
        assert(p_exact.frame_period.count() == 6'944'444LL);
    }

    // Dynamic DisplayPeriodFor must track monitor rate changes cleanly.
    const auto p_60 = EmulatorSettingsImpl::DisplayPeriodFor(60.0, 59.94, true);
    const auto p_144 = EmulatorSettingsImpl::DisplayPeriodFor(144.0, 143.86, true);
    assert(p_60.count() > 16'680'000LL);
    assert(p_144.count() > 6'950'000LL);
}

static void TestDeadlines() {
    const auto base = steady_clock::time_point{};
    const auto period = nanoseconds(10'000'000); // 100 FPS

    // Slightly late (2 ms behind) keeps exact phase (`prev + period`).
    const auto kept = AdvanceFlipDeadline(base, period, base + milliseconds(2));
    assert(kept == base + period);

    // Multi-frame stall (> 1 period behind) spaces next flip a full period from `now`
    // rather than setting a past deadline that causes zero-gap burst frames.
    const auto stalled_now = base + milliseconds(35);
    const auto recovered = AdvanceFlipDeadline(base, period, stalled_now);
    assert(recovered == stalled_now + period);

    // Vblank deadline maintains fixed cadence across small jitter and resyncs on multi-tick pauses.
    const auto vb = nanoseconds(16'666'667);
    const auto vb1 = AdvanceVblankDeadline(base, vb, base + milliseconds(1));
    assert(vb1 == base + vb);
    const auto vb_resync = AdvanceVblankDeadline(base, vb, base + milliseconds(60));
    assert(vb_resync == base + milliseconds(60) + vb);
}

static void TestAsyncSkipAndFreshTargets() {
    AsyncSkipWindow win{};
    const u64 t0 = 1'000'000'000ULL;
    // Budget 1 at 250 FPS (4 ms/frame) must NOT expire after 1-2 GPU frames (4-8 ms).
    assert(!IsAsyncSkipBudgetSpent(win, 1, 100, t0));
    assert(!IsAsyncSkipBudgetSpent(win, 1, 101, t0 + 4'000'000ULL));
    assert(!IsAsyncSkipBudgetSpent(win, 1, 105, t0 + 20'000'000ULL));
    assert(IsAsyncSkipBudgetSpent(win, 1, 109, t0 + 36'000'000ULL));

    // Budget 0 is synchronous immediately; negative skips until ready.
    AsyncSkipWindow sync_win{};
    assert(IsAsyncSkipBudgetSpent(sync_win, 0, 100, t0));
    AsyncSkipWindow unlim_win{};
    assert(!IsAsyncSkipBudgetSpent(unlim_win, -1, 100, t0));
    assert(!IsAsyncSkipBudgetSpent(unlim_win, -1, 5000, t0 + 60'000'000'000ULL));

    // Fresh-target window: 25 GPU frames across 80 ms (high FPS) keeps periodic targets warm.
    assert(IsTargetStillRecent(125, 100, t0 + 80'000'000ULL, t0));
    assert(!IsTargetStillRecent(125, 100, t0 + 600'000'000ULL, t0));
    assert(!ShouldForgetRecentTarget(2000, 100, t0 + 2'000'000'000ULL, t0));
    assert(ShouldForgetRecentTarget(2000, 100, t0 + 12'000'000'000ULL, t0));
}

static void TestDisplaySettingsApiAndTimers() {
    theatrhythm_display_set(THEATRHYTHM_DISPLAY_TEARING, 1);
    assert(theatrhythm_display_get(THEATRHYTHM_DISPLAY_TEARING) == 1);
    theatrhythm_display_set(THEATRHYTHM_DISPLAY_TEARING, 0);
    assert(theatrhythm_display_get(THEATRHYTHM_DISPLAY_TEARING) == 0);
    theatrhythm_display_set(THEATRHYTHM_DISPLAY_FPS_LIMIT, -1);
    assert(theatrhythm_display_get(THEATRHYTHM_DISPLAY_FPS_LIMIT) == -1);

    // Non-positive sleep returns immediately.
    nanoseconds rem{-1};
    assert(Common::AccurateSleep(nanoseconds(0), &rem, false));
    assert(rem.count() == 0);
    assert(Common::AccurateSleep(nanoseconds(-1000), &rem, false));
    assert(rem.count() == 0);

    // Sub-tick deadline wait must beat legacy 15.6 ms OS timer quantization.
    const auto t0 = steady_clock::now();
    Common::AccurateWaitUntil(t0 + milliseconds(4));
    const auto elapsed = duration_cast<microseconds>(steady_clock::now() - t0);
    assert(elapsed >= microseconds(3500));
    assert(elapsed < microseconds(10000));
}

int main() {
    TestPacingDecisions();
    TestDeadlines();
    TestAsyncSkipAndFreshTargets();
    TestDisplaySettingsApiAndTimers();
    std::printf("PASS: test_frame_pacing completed successfully!\n");
    return 0;
}
