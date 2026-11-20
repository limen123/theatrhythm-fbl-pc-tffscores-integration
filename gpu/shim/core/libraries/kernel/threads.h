// theatrhythm: host threads that may call guest code (AvPlayer allocator callbacks).
// Each thread gets a guest TCB (GS base, TLS) from the C runtime before running.
#pragma once
#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <system_error>
#include <thread>
#include "common/types.h"

extern "C" void runtime_thread_attach_host(const char* name);

namespace Libraries::Kernel {
class Thread {
public:
    Thread() = default;
    ~Thread() { Stop(); }
    void Run(std::function<void(std::stop_token)>&& func) {
        Join();
        std::scoped_lock lk{mutex};
        finished = std::make_shared<std::atomic<bool>>(false);
        thread = std::jthread([func = std::move(func), done = finished](std::stop_token stop) {
            runtime_thread_attach_host("theatrhythm:hle");
            func(stop);
            done->store(true, std::memory_order_release);
        });
    }
    // A thread may stop its own Thread object (AvPlayer does); serialize ownership transfer so
    // EOF self-cleanup never races Stop() calling join().
    void Join() {
        std::jthread local;
        std::shared_ptr<std::atomic<bool>> done;
        {
            std::scoped_lock lk{mutex};
            if (!thread.joinable()) return;
            local = std::move(thread);
            done = finished;
        }
        if (local.get_id() == std::this_thread::get_id()) {
            local.detach();
            return;
        }
        try {
            local.join();
        } catch (const std::system_error& error) {
            std::fprintf(stderr, "HLE thread: join failed (%s); waiting for it to finish\n", error.what());
            if (done) {
                while (!done->load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }
            if (local.joinable()) local.detach();
        }
    }
    bool Joinable() const {
        std::scoped_lock lk{mutex};
        return thread.joinable();
    }
    void Stop() {
        {
            std::scoped_lock lk{mutex};
            if (thread.joinable()) {
                thread.request_stop();
            }
        }
        Join();
    }

private:
    mutable std::mutex mutex;
    std::jthread thread;
    std::shared_ptr<std::atomic<bool>> finished;
};
} // namespace Libraries::Kernel
