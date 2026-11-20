// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>

namespace Libraries::VideoOut {

struct PacingDecision {
    bool immediate_flips = false;
    bool follow_display = false;
    std::chrono::nanoseconds frame_period{0};

    bool operator==(const PacingDecision&) const = default;
};

inline std::chrono::nanoseconds NanosecondsFromHz(double hz) {
    if (hz <= 0.0) {
        return std::chrono::nanoseconds(0);
    }
    return std::chrono::nanoseconds(static_cast<std::int64_t>(1e9 / hz + 0.5));
}

inline PacingDecision ComputePacingDecision(int setting_fps, bool vsync, double display_hz,
                                            double vblank_hz, bool env_limit_set,
                                            std::uint32_t env_limit, bool allow_vblank_lock) {
    const bool near_vblank = display_hz > 0.5 && std::abs(display_hz - vblank_hz) < 0.5;

    if (env_limit_set) {
        const bool immediate = env_limit != 0;
        return {
            .immediate_flips = immediate,
            .follow_display = !immediate && allow_vblank_lock && vsync && near_vblank,
            .frame_period = NanosecondsFromHz(static_cast<double>(env_limit)),
        };
    }

    // Unlimited / uncapped (-1): never apply a software sleep cap. Hardware VSync (eFifo)
    // still paces to the display when `vsync` is true; with VSync off + tearing (eImmediate)
    // flips present as fast as the GPU renders them.
    if (setting_fps < 0) {
        return {
            .immediate_flips = true,
            .follow_display = false,
            .frame_period = std::chrono::nanoseconds(0),
        };
    }

    // Follow display (0): when VSync is on, hardware eFifo already paces at the panel's true
    // refresh rate; never add a competing software timer at the same rate. When VSync is off,
    // pace in software to the exact display rate.
    if (setting_fps == 0) {
        if (vsync) {
            if (display_hz > vblank_hz + 0.5) {
                return {
                    .immediate_flips = true,
                    .follow_display = false,
                    .frame_period = std::chrono::nanoseconds(0),
                };
            }
            return {
                .immediate_flips = false,
                .follow_display = allow_vblank_lock && near_vblank,
                .frame_period = std::chrono::nanoseconds(0),
            };
        }
        const double target_hz = display_hz > 0.5 ? display_hz : vblank_hz;
        return {
            .immediate_flips = true,
            .follow_display = false,
            .frame_period = NanosecondsFromHz(target_hz),
        };
    }

    // Explicit numeric cap (30, 60, 100, 120, 144, 180, 240, ...):
    // When VSync is on and the target meets or exceeds the known display rate, let hardware
    // eFifo pace natively (or lock 1:1 to DXGI vblank on 60 Hz panels) instead of phase-beating.
    // When the target matches the guest vblank rate (e.g. 60 FPS at 60 Hz vblank), use the
    // fixed 1-flip-per-vblank path rather than colliding with tick_deadline inside run_immediate.
    const double requested_hz = static_cast<double>(setting_fps);
    if (display_hz > 0.5 && vsync && requested_hz >= display_hz - 0.5) {
        if (near_vblank) {
            return {
                .immediate_flips = false,
                .follow_display = allow_vblank_lock,
                .frame_period = std::chrono::nanoseconds(0),
            };
        }
        return {
            .immediate_flips = true,
            .follow_display = false,
            .frame_period = std::chrono::nanoseconds(0),
        };
    }
    if (std::abs(requested_hz - vblank_hz) < 0.5) {
        return {
            .immediate_flips = false,
            .follow_display = vsync && allow_vblank_lock && near_vblank,
            .frame_period = std::chrono::nanoseconds(0),
        };
    }

    const double snapped_hz =
        display_hz > 0.5 && std::abs(display_hz - requested_hz) < 0.5 ? display_hz : requested_hz;
    return {
        .immediate_flips = true,
        .follow_display = false,
        .frame_period = NanosecondsFromHz(snapped_hz),
    };
}

template <typename TimePoint>
inline TimePoint AdvanceFlipDeadline(TimePoint prev_deadline, std::chrono::nanoseconds period,
                                     TimePoint now) {
    if (period.count() <= 0) {
        return now;
    }
    const auto step = std::chrono::duration_cast<typename TimePoint::duration>(period);
    const auto next = prev_deadline + step;
    return next > now ? next : (now + step);
}

template <typename TimePoint>
inline TimePoint AdvanceVblankDeadline(TimePoint prev_deadline, std::chrono::nanoseconds period,
                                       TimePoint now) {
    const auto step = std::chrono::duration_cast<typename TimePoint::duration>(period);
    const auto next = prev_deadline + step;
    if (next + step < now) {
        return now + step;
    }
    return next;
}

} // namespace Libraries::VideoOut
