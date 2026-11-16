// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <limits>
#ifdef _WIN32
#include <windows.h>
#undef WAIT_TIMEOUT
#endif
#include "common/thread.h"
#include "theatrhythm_toggles.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

#include "common/assert.h"

namespace Vulkan {

constexpr u64 WAIT_TIMEOUT = std::numeric_limits<u64>::max();

Semaphore::Semaphore(const Instance& instance_) : instance{instance_} {
    const vk::StructureChain semaphore_chain = {
        vk::SemaphoreCreateInfo{},
        vk::SemaphoreTypeCreateInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        },
    };
    auto [semaphore_result, sem] =
        instance.GetDevice().createSemaphoreUnique(semaphore_chain.get());
    ASSERT_MSG(semaphore_result == vk::Result::eSuccess, "Failed to create master semaphore: {}",
               vk::to_string(semaphore_result));
    semaphore = std::move(sem);
}

Semaphore::~Semaphore() = default;

void Semaphore::Refresh() {
    u64 this_tick{};
    u64 counter{};
    do {
        this_tick = gpu_tick.load(std::memory_order_acquire);
        auto [counter_result, cntr] = instance.GetDevice().getSemaphoreCounterValue(*semaphore);
        ASSERT_MSG(counter_result == vk::Result::eSuccess,
                   "Failed to get master semaphore value: {}", vk::to_string(counter_result));
        counter = cntr;
        if (counter < this_tick) {
            return;
        }
    } while (!gpu_tick.compare_exchange_weak(this_tick, counter, std::memory_order_release,
                                             std::memory_order_relaxed));
}

void Semaphore::Wait(u64 tick) {
    // No need to wait if the GPU is ahead of the tick
    if (IsFree(tick)) {
        return;
    }
    // Update the GPU tick and try again
    Refresh();
    if (IsFree(tick)) {
        return;
    }

    // If none of the above is hit, fallback to a regular wait
#ifdef _WIN32
    ULONG64 cyc0 = 0;
    if (TheatrhythmStats::enabled) {
        QueryThreadCycleTime(GetCurrentThread(), &cyc0);
    }
    const auto finish_cycles = [&] {
        if (TheatrhythmStats::enabled) {
            ULONG64 cyc1 = 0;
            QueryThreadCycleTime(GetCurrentThread(), &cyc1);
            TheatrhythmStats::sem_wait_cycles.fetch_add(cyc1 - cyc0, std::memory_order_relaxed);
        }
    };
    // On Windows NVIDIA drivers, vkWaitSemaphores busy-spins a CPU core; briefly spin for <30 us
    // waits then sleep cleanly with the high-resolution waitable timer while polling the counter.
    const auto spin_until = std::chrono::steady_clock::now() + std::chrono::microseconds(30);
    while (std::chrono::steady_clock::now() < spin_until) {
        __builtin_ia32_pause();
        Refresh();
        if (IsFree(tick)) {
            finish_cycles();
            return;
        }
    }
    while (!IsFree(tick)) {
        Common::AccurateSleep(std::chrono::microseconds(250), nullptr, false);
        Refresh();
    }
    finish_cycles();
#else
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &semaphore.get(),
        .pValues = &tick,
    };

    while (instance.GetDevice().waitSemaphores(&wait_info, WAIT_TIMEOUT) != vk::Result::eSuccess) {
    }
    Refresh();
#endif
}

} // namespace Vulkan
