// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>

namespace Vulkan {

/// Tracks both GPU frame count and wall-clock time since an async shader/pipeline draw started
/// skipping, so high/uncapped FPS (120-500+ FPS) grants workers a real time window before stalling.
struct AsyncSkipWindow {
    std::uint64_t frame = ~std::uint64_t{};
    std::uint64_t start_ns = 0;

    bool IsUnset() const noexcept {
        return frame == ~std::uint64_t{};
    }
    void Reset() noexcept {
        frame = ~std::uint64_t{};
        start_ns = 0;
    }
    static AsyncSkipWindow Capture(std::uint64_t current_frame, std::uint64_t now_ns) noexcept {
        return {current_frame, now_ns};
    }
};

inline std::uint64_t SteadyClockNowNs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

inline bool IsAsyncSkipBudgetSpent(AsyncSkipWindow& window, int budget_frames,
                                   std::uint64_t current_frame, std::uint64_t now_ns) noexcept {
    if (budget_frames < 0) {
        return false;
    }
    if (budget_frames == 0) {
        return true;
    }
    if (window.IsUnset()) {
        window = AsyncSkipWindow::Capture(current_frame, now_ns);
    }
    // At 60 FPS a frame is 16.7 ms; at 240+ FPS a single frame is <4.2 ms, which is shorter than
    // typical SPIR-V/pipeline compilation. Require ~33.3 ms per budget step as a time floor.
    constexpr std::uint64_t StepFloorNs = 33'333'334ULL;
    const std::uint64_t elapsed_frames =
        current_frame >= window.frame ? current_frame - window.frame : 0;
    const std::uint64_t elapsed_ns = now_ns >= window.start_ns ? now_ns - window.start_ns : 0;
    return elapsed_frames >= static_cast<std::uint64_t>(budget_frames) &&
           elapsed_ns >= static_cast<std::uint64_t>(budget_frames) * StepFloorNs;
}

/// Determines whether a render target was used recently enough that re-drawing it should not force
/// synchronous fresh-target pipeline compilation.
inline bool IsTargetStillRecent(std::uint64_t current_frame, std::uint64_t last_frame,
                                std::uint64_t now_ns, std::uint64_t last_ns,
                                std::uint64_t recent_frames = 4,
                                std::uint64_t recent_window_ns = 500'000'000ULL) noexcept {
    if (current_frame >= last_frame && current_frame - last_frame <= recent_frames) {
        return true;
    }
    return now_ns >= last_ns && (now_ns - last_ns) <= recent_window_ns;
}

inline bool ShouldForgetRecentTarget(std::uint64_t current_frame, std::uint64_t last_frame,
                                     std::uint64_t now_ns, std::uint64_t last_ns,
                                     std::uint64_t forget_frames = 600,
                                     std::uint64_t forget_window_ns = 10'000'000'000ULL) noexcept {
    return (current_frame >= last_frame && current_frame - last_frame > forget_frames) &&
           (now_ns >= last_ns && now_ns - last_ns > forget_window_ns);
}

} // namespace Vulkan
