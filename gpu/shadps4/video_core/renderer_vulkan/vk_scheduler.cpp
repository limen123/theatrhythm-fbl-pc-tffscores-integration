// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <thread>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <functional>

#include "theatrhythm_copy.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "theatrhythm_toggles.h"
#include "video_core/renderer_vulkan/vk_wait_diagnostics.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/thread.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "theatrhythm_threads.h"

namespace Vulkan {

std::mutex Scheduler::submit_mutex;

Scheduler::Scheduler(const Instance& instance, bool threaded_recording)
    : instance{instance}, work_semaphore{instance}, command_pool{instance, &work_semaphore},
      uses_async_copies{threaded_recording} {
    // theatrhythm: THEATRHYTHM_VK_RECORD_THREAD=0 records on the calling thread.
    const char* env = std::getenv("THEATRHYTHM_VK_RECORD_THREAD");
    if (threaded_recording && !(env && env[0] == '0')) {
        record_chunk = AcquireChunk();
        recorder_thread = std::jthread(std::bind_front(&Scheduler::RecorderThread, this));
        recording_threaded = true;
    }
#if TRACY_GPU_ENABLED
    profiler_scope = reinterpret_cast<tracy::VkCtxScope*>(std::malloc(sizeof(tracy::VkCtxScope)));
#endif
    AllocateWorkerCommandBuffers();
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
}

Scheduler::~Scheduler() {
    if (recorder_thread.joinable()) {
        SyncRecording();
        recorder_thread.request_stop();
        recorder_cv.notify_all();
        recorder_thread.join();
    }
#if TRACY_GPU_ENABLED
    std::free(profiler_scope);
#endif
}

bool Scheduler::FullBarriers() {
    static const bool enabled = [] {
        const char* env = std::getenv("THEATRHYTHM_FULL_BARRIERS");
        const bool on = env && env[0] == '1';
        if (on) {
            // In last_run.log whatever the GPU log level, so a tester's log shows the mode.
            std::printf("GPU: full barriers on (THEATRHYTHM_FULL_BARRIERS=1)\n");
        }
        return on;
    }();
    return enabled;
}

void Scheduler::RecordFullBarrier(vk::CommandBuffer cmdbuf) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });
}

void Scheduler::BeginRendering(const RenderState& new_state) {
    if (is_rendering && render_state == new_state) {
        return;
    }
    EndRendering();
    is_rendering = true;
    render_state = new_state;

    std::array<vk::RenderingAttachmentInfo, 8> color_attachments;
    for (u32 i = 0; i < render_state.num_color_attachments; ++i) {
        const auto& cb = render_state.color_attachments[i];
        color_attachments[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }

    const auto& db = render_state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                                          db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };

    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = render_state.num_layers,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = db.has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil_attachment : nullptr,
    };

    if (!recording_threaded) {
        if (FullBarriers()) {
            RecordFullBarrier(current_cmdbuf);
        }
        current_cmdbuf.beginRendering(rendering_info);
        return;
    }
    // The attachment infos live on this stack frame: the recorded closure keeps copies.
    Record([info = rendering_info, color_attachments, depth_attachment,
            stencil_attachment](vk::CommandBuffer cmdbuf) mutable {
        info.pColorAttachments = color_attachments.data();
        if (info.pDepthAttachment) {
            info.pDepthAttachment = &depth_attachment;
        }
        if (info.pStencilAttachment) {
            info.pStencilAttachment = &stencil_attachment;
        }
        if (FullBarriers()) {
            RecordFullBarrier(cmdbuf);
        }
        cmdbuf.beginRendering(info);
    });
}

void Scheduler::EndRendering() {
    if (!is_rendering) {
        return;
    }
    is_rendering = false;
    Record([](vk::CommandBuffer cmdbuf) {
        cmdbuf.endRendering();
        if (FullBarriers()) {
            RecordFullBarrier(cmdbuf);
        }
    });
}

void Scheduler::TraceDirectRecording(void* caller) {
    static const bool enabled = [] {
        const char* env = std::getenv("THEATRHYTHM_RECORDER_TRACE");
        return env && env[0] == '1';
    }();
    if (!enabled) {
        return;
    }
    static std::mutex mutex;
    static std::unordered_map<void*, u64> callers;
    static u64 calls;
    std::scoped_lock lk{mutex};
    ++callers[caller];
    if (++calls % 2000) {
        return;
    }
    std::vector<std::pair<u64, void*>> top;
    for (const auto& [address, count] : callers) {
        top.emplace_back(count, address);
    }
    std::ranges::sort(top, std::greater{});
    for (size_t i = 0; i < std::min<size_t>(top.size(), 8); ++i) {
#ifdef _WIN32
        struct { const char* dli_fname; void* dli_fbase; } info{nullptr, nullptr};
#else
        Dl_info info{};
        dladdr(top[i].second, &info);
#endif
        std::printf("Recorder sync caller: %llu x %s+0x%lx\n",
                    static_cast<unsigned long long>(top[i].first),
                    info.dli_fname ? info.dli_fname : "?",
                    static_cast<unsigned long>(reinterpret_cast<uintptr_t>(top[i].second) -
                                               reinterpret_cast<uintptr_t>(info.dli_fbase)));
    }
    callers.clear();
}

std::unique_ptr<RecordChunk> Scheduler::AcquireChunk() {
    std::scoped_lock lk{recorder_mutex};
    if (free_chunks.empty()) {
        return std::make_unique<RecordChunk>();
    }
    auto chunk = std::move(free_chunks.back());
    free_chunks.pop_back();
    return chunk;
}

void Scheduler::SignalAfterHostCopies(std::function<void()> signal) {
    if (!IsRecordingDeferred()) {
        WaitHostCopies();
        signal();
        return;
    }
    TheatrhythmCopy::FlushBatch();
    deferred_signals_issued.fetch_add(1, std::memory_order_relaxed);
    Record([signal = std::move(signal), done = deferred_signals_done](vk::CommandBuffer) mutable {
        TheatrhythmCopy::AfterCopies([signal = std::move(signal), done = std::move(done)] {
            signal();
            done->fetch_add(1, std::memory_order_release);
        });
    });
    KickRecording(true);
}

void Scheduler::WaitDeferredSignals() {
    const u64 issued = deferred_signals_issued.load(std::memory_order_relaxed);
    if (deferred_signals_done->load(std::memory_order_acquire) >= issued ||
        TheatrhythmToggle::Disabled(TheatrhythmToggle::OrderedGuestWrites)) {
        return;
    }
    TheatrhythmStats::WaitTimer timer{TheatrhythmStats::host_copies_wait_ns};
    TheatrhythmStats::Timer breakdown_timer{TheatrhythmStats::wait_deferred_signals_ns};
    const auto t0 = TheatrhythmStats::enabled ? std::chrono::steady_clock::now()
                                              : std::chrono::steady_clock::time_point{};
    KickRecording(true);
    while (deferred_signals_done->load(std::memory_order_acquire) < issued) {
        if (recording_threaded) {
            WaitRecordingIdle();
        }
        // Helps and waits (without spinning) on the copy threads the signals wait for.
        TheatrhythmCopy::WaitAsync();
        if (deferred_signals_done->load(std::memory_order_acquire) >= issued) {
            break;
        }
        std::this_thread::yield();
    }
    if (TheatrhythmStats::enabled) {
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();
        if (ms >= 10.0) {
            std::printf("Long WaitDeferredSignals: %.1f ms (issued=%llu done=%llu)\n", ms,
                        static_cast<unsigned long long>(issued),
                        static_cast<unsigned long long>(
                            deferred_signals_done->load(std::memory_order_relaxed)));
        }
    }
}

void Scheduler::WaitHostCopies() {
    if (host_copies_done.load(std::memory_order_acquire) < host_copies_issued) {
        TheatrhythmStats::WaitTimer timer{TheatrhythmStats::host_copies_wait_ns};
        TheatrhythmStats::Timer breakdown_timer{TheatrhythmStats::wait_host_copies_only_ns};
        TheatrhythmStats::host_copy_waits.fetch_add(1, std::memory_order_relaxed);
        const auto t0 = TheatrhythmStats::enabled ? std::chrono::steady_clock::now()
                                                  : std::chrono::steady_clock::time_point{};
        KickRecording(true);
        const auto t1 = TheatrhythmStats::enabled ? std::chrono::steady_clock::now()
                                                  : std::chrono::steady_clock::time_point{};
        if (host_copies_done.load(std::memory_order_acquire) < host_copies_issued) {
            const auto spin_until =
                std::chrono::steady_clock::now() + std::chrono::microseconds(20);
            while (host_copies_done.load(std::memory_order_acquire) < host_copies_issued &&
                   std::chrono::steady_clock::now() < spin_until) {
                __builtin_ia32_pause();
            }
            if (host_copies_done.load(std::memory_order_acquire) < host_copies_issued &&
                recording_threaded) {
                WaitRecordingIdle();
            }
            while (host_copies_done.load(std::memory_order_acquire) < host_copies_issued) {
                std::this_thread::yield();
            }
        }
        if (TheatrhythmStats::enabled) {
            const auto t2 = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(t2 - t0).count();
            if (ms >= 10.0) {
                const double kick_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                std::printf("Long WaitHostCopies: %.1f ms (kick=%.1f ms, issued=%llu done=%llu)\n",
                            ms, kick_ms, static_cast<unsigned long long>(host_copies_issued),
                            static_cast<unsigned long long>(
                                host_copies_done.load(std::memory_order_relaxed)));
            }
        }
    }
    if (uses_async_copies) {
        TheatrhythmStats::WaitTimer timer{TheatrhythmStats::copy_threads_wait_ns};
        TheatrhythmCopy::WaitAsync();
    }
}

void Scheduler::KickRecording(bool force) {
    if (!recording_threaded) {
        return;
    }
    // Callers kick where nobody holds the raw command buffer: deferral resumes.
    direct_mode = false;
    // Batches of tens of KiB keep the queue handoff cheap relative to the work it carries.
    if (!force && full_chunks.empty() && record_chunk->Size() < 32 * 1024) {
        return;
    }
    if (full_chunks.empty() && record_chunk->Empty()) {
        return;
    }
    bool wake;
    {
        std::scoped_lock lk{recorder_mutex};
        for (auto& chunk : full_chunks) {
            recorder_queue.push_back(std::move(chunk));
        }
        if (!record_chunk->Empty()) {
            recorder_queue.push_back(std::move(record_chunk));
        }
        wake = recorder_sleeping;
        queued_chunks.store(recorder_queue.size(), std::memory_order_release);
    }
    full_chunks.clear();
    // A busy recorder picks the new chunks up by itself: waking it is a syscall per draw.
    if (wake) {
        recorder_cv.notify_one();
    }
    if (!record_chunk) {
        record_chunk = AcquireChunk();
    }
}

void Scheduler::WaitRecordingIdle() {
    if (!recording_threaded) {
        return;
    }
    std::unique_lock lk{recorder_mutex};
    recorder_idle_cv.wait(lk, [this] { return recorder_queue.empty() && !recorder_busy; });
}

void Scheduler::SyncRecording() {
    if (!recording_threaded) {
        return;
    }
    KickRecording(true);
    TheatrhythmStats::WaitTimer timer{TheatrhythmStats::sync_recording_ns};
    WaitRecordingIdle();
}

void Scheduler::RecorderThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("theatrhythm:VkRecorder");
    while (true) {
        // Spin briefly before sleeping: the next chunk usually follows within microseconds,
        // and a sleeping recorder costs the GPU thread a wake-up syscall per kick. With few
        // hardware threads (Steam Deck: 8) the spin would take time from guest threads.
        static const auto spin_time = std::chrono::microseconds(
            TheatrhythmThreads::Available() >= 12 ? 200 : 20);
        const auto spin_until = std::chrono::steady_clock::now() + spin_time;
        for (u32 spins = 1; queued_chunks.load(std::memory_order_acquire) == 0; ++spins) {
            __builtin_ia32_pause();
            // The clock is read every 256 pauses, not per iteration.
            if (!(spins & 255) &&
                (stoken.stop_requested() || std::chrono::steady_clock::now() >= spin_until)) {
                break;
            }
        }
        std::unique_ptr<RecordChunk> chunk;
        {
            std::unique_lock lk{recorder_mutex};
            recorder_sleeping = true;
            recorder_cv.wait(lk, stoken, [this] { return !recorder_queue.empty(); });
            recorder_sleeping = false;
            if (recorder_queue.empty()) {
                return; // stop requested
            }
            chunk = std::move(recorder_queue.front());
            recorder_queue.pop_front();
            queued_chunks.store(recorder_queue.size(), std::memory_order_release);
            recorder_busy = true;
        }
        // current_cmdbuf only changes after SyncRecording(), which waits for this thread.
        const auto exec_t0 = TheatrhythmStats::enabled ? std::chrono::steady_clock::now()
                                                       : std::chrono::steady_clock::time_point{};
        const u64 hc_before = host_copies_done.load(std::memory_order_relaxed);
        chunk->Execute(current_cmdbuf);
        if (TheatrhythmStats::enabled) {
            const double exec_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - exec_t0)
                    .count();
            if (exec_ms >= 5.0) {
                std::printf("Long ChunkExecute: %.1f ms (host_copies=%llu)\n", exec_ms,
                            static_cast<unsigned long long>(
                                host_copies_done.load(std::memory_order_relaxed) - hc_before));
            }
        }
        {
            std::scoped_lock lk{recorder_mutex};
            free_chunks.push_back(std::move(chunk));
            recorder_busy = false;
            if (recorder_queue.empty()) {
                recorder_idle_cv.notify_all();
            }
        }
    }
}

void Scheduler::Flush(SubmitInfo& info) {
    // When flushing, we only send data to the driver; no waiting is necessary.
    SubmitExecution(info);
}

void Scheduler::Flush() {
    SubmitInfo info{};
    Flush(info);
}

void Scheduler::Finish(std::source_location caller) {
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    const bool diagnose = WaitDiagnostics::Enabled();
    const auto start = diagnose ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    SubmitInfo info{};
    SubmitExecution(info);
    const u64 submit_ns = diagnose ? std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count() : 0;
    Wait(presubmit_tick, caller, submit_ns);
}

void Scheduler::WaitSubmitted(std::source_location caller) {
    const u64 tick = CurrentTick();
    if (tick > 1) {
        Wait(tick - 1, caller);
    }
}

void Scheduler::Wait(u64 tick, std::source_location caller, u64 submit_ns) {
    const bool diagnose = WaitDiagnostics::Enabled();
    auto start = diagnose ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (tick >= work_semaphore.CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        SubmitInfo info{};
        Flush(info);
    }
    TheatrhythmStats::WaitTimer timer{TheatrhythmStats::tick_wait_ns};
    const auto submitted = diagnose ? std::chrono::steady_clock::now() : start;
    work_semaphore.Wait(tick);
    if (diagnose) {
        const auto completed = std::chrono::steady_clock::now();
        WaitDiagnostics::Get().Add(caller.file_name(), caller.line(),
            submit_ns + std::chrono::duration_cast<std::chrono::nanoseconds>(submitted - start).count(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(completed - submitted).count());
    }
}

void Scheduler::PopPendingOperations() {
    if (num_pending_ops.load(std::memory_order_acquire) == 0) {
        return; // every draw comes here
    }
    std::unique_lock lk(pending_ops_mutex);
    // theatrhythm: this runs on every draw and dispatch. Querying the timeline semaphore is an
    // ioctl, so it is skipped when nothing waits and done once per 32 calls (~0.3 ms; reading
    // the clock per draw instead was itself a hot spot).
    if (pending_ops.empty()) {
        return;
    }
    if (!work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        if ((++pending_polls & 31) != 0 && !TheatrhythmToggle::Disabled(TheatrhythmToggle::PendingPollLimit)) {
            return;
        }
        work_semaphore.Refresh();
    }
    while (!pending_ops.empty() && work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
        num_pending_ops.fetch_sub(1, std::memory_order_release);
    }
}

void Scheduler::AllocateWorkerCommandBuffers() {
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };

    current_cmdbuf = command_pool.Commit();
    Check(current_cmdbuf.begin(begin_info));

    // Invalidate dynamic state so it gets applied to the new command buffer.
    dynamic_state.Invalidate();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        static const auto scope_loc =
            GPU_SCOPE_LOCATION("Guest Frame", MarkersPalette::GpuMarkerColor);
        new (profiler_scope) tracy::VkCtxScope{profiler_ctx, &scope_loc, current_cmdbuf, true};
    }
#endif
}

void Scheduler::SubmitExecution(SubmitInfo& info) {
    const auto mutex_t0 = TheatrhythmStats::enabled ? std::chrono::steady_clock::now()
                                                    : std::chrono::steady_clock::time_point{};
    std::scoped_lock lk{submit_mutex};
    if (TheatrhythmStats::enabled) {
        TheatrhythmStats::sched_submit_mutex_ns.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - mutex_t0)
                .count(),
            std::memory_order_relaxed);
    }
    const auto prep_t0 = TheatrhythmStats::enabled ? std::chrono::steady_clock::now()
                                                   : std::chrono::steady_clock::time_point{};
    const u64 signal_value = work_semaphore.NextTick();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        profiler_scope->~VkCtxScope();
        TracyVkCollect(profiler_ctx, current_cmdbuf);
    }
#endif

    if (on_submit) {
        on_submit(info);
    }

    EndRendering();
    if (auto* profiler = GpuProfiler::Get(); profiler && profiler->Records(this)) {
        // Until the next submission's first timestamp: mostly the GPU waiting for it.
        profiler->Mark(0x5B317ull, [] { return std::string{"(between submissions: GPU idle)"}; });
    }
    SyncRecording();
    if (TheatrhythmStats::enabled) {
        TheatrhythmStats::sched_on_submit_ns.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - prep_t0)
                .count(),
            std::memory_order_relaxed);
    }
    // Guest memory copies into staging read by this submission (copy threads).
    {
        TheatrhythmStats::Timer wc_timer{TheatrhythmStats::sched_wait_host_copies_ns};
        WaitHostCopies();
    }
    const auto sub_t0 = TheatrhythmStats::enabled ? std::chrono::steady_clock::now()
                                                  : std::chrono::steady_clock::time_point{};
    Check(current_cmdbuf.end());

    const vk::Semaphore timeline = work_semaphore.Handle();
    info.AddSignal(timeline, signal_value);

    static constexpr std::array<vk::PipelineStageFlags, 2> wait_stage_masks = {
        vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
    };

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };

    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = 1U,
        .pCommandBuffers = &current_cmdbuf,
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };

    ImGui::Core::TextureManager::Submit();
    auto submit_result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");

    work_semaphore.Refresh();
    AllocateWorkerCommandBuffers();

    // Apply pending operations
    PopPendingOperations();
    if (TheatrhythmStats::enabled) {
        TheatrhythmStats::sched_vk_submit_ns.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - sub_t0)
                .count(),
            std::memory_order_relaxed);
    }
}

void Scheduler::PriorityPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedPriorityPendingOpsRunner");

    while (!stoken.stop_requested()) {
        PendingOp op;
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken,
                                         [this] { return !priority_pending_ops.empty(); });
            if (stoken.stop_requested()) {
                break;
            }

            op = std::move(priority_pending_ops.front());
            priority_pending_ops.pop();
        }

        work_semaphore.Wait(op.gpu_tick);
        if (stoken.stop_requested()) {
            break;
        }

        op.callback();
    }
}

void DynamicState::Commit(const Instance& instance, const vk::CommandBuffer& cmdbuf) {
    // A null command buffer only updates the dirty flags, exactly as recording would.
    CommitWith(instance.IsDepthBoundsSupported(), instance.IsDynamicColorWriteMaskSupported(),
               instance.IsAttachmentFeedbackLoopLayoutSupported(), [&](auto&& command) {
                   if (cmdbuf) {
                       command(cmdbuf);
                   }
               });
}

} // namespace Vulkan
