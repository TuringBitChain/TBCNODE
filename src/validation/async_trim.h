// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P3.4 AsyncTrim 专用线程
//
// 当 mempool 满载（高水位）时由专用线程 trim，单次 evict 拆批 1000 个，
// 每批主动 unlock + yield 给 commit 路径机会
// （F8 修补：trim 持 unique_lock(smtx) 跟 commit 互斥，单批 ≤ 15ms）

#ifndef BITCOIN_VALIDATION_ASYNC_TRIM_H
#define BITCOIN_VALIDATION_ASYNC_TRIM_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace tbc {
namespace validation {

class AsyncTrim {
public:
    using TrimBatchFunc = std::function<size_t(size_t /*max_evict*/)>;

    static constexpr size_t TRIM_BATCH_SIZE = 1000;
    static constexpr int MAX_BATCHES_PER_WAKE = 10;
    static constexpr int IDLE_WAIT_MS = 10;

    explicit AsyncTrim(TrimBatchFunc trim_fn)
        : trim_func(std::move(trim_fn)) {
        thread = std::thread([this] { Run(); });
    }

    ~AsyncTrim() { Stop(); }

    void Notify() noexcept { cv.notify_one(); }
    void NotifyUrgent() noexcept {
        urgent.store(true, std::memory_order_release);
        cv.notify_one();
    }

    void Stop() {
        running.store(false, std::memory_order_release);
        cv.notify_all();
        if (thread.joinable()) thread.join();
    }

    AsyncTrim(const AsyncTrim&) = delete;
    AsyncTrim& operator=(const AsyncTrim&) = delete;

private:
    void Run() {
        while (running.load(std::memory_order_acquire)) {
            {
                std::unique_lock lock(mtx);
                cv.wait_for(lock, std::chrono::milliseconds(IDLE_WAIT_MS), [this] {
                    return urgent.exchange(false, std::memory_order_acq_rel) ||
                           !running.load(std::memory_order_acquire);
                });
            }
            if (!running.load(std::memory_order_acquire)) break;

            // 拆批 evict，每批 yield 给 commit 路径机会
            for (int i = 0; i < MAX_BATCHES_PER_WAKE; i++) {
                size_t evicted = 0;
                try { evicted = trim_func(TRIM_BATCH_SIZE); }
                catch (...) { evicted = 0; }
                if (evicted < TRIM_BATCH_SIZE) break;
                std::this_thread::yield();
            }
        }
    }

    TrimBatchFunc trim_func;
    std::thread thread;
    std::atomic<bool> running{true};
    std::atomic<bool> urgent{false};
    std::mutex mtx;
    std::condition_variable cv;
};

// 全局 AsyncTrim 句柄（init.cpp 启动时构造，shutdown 时析构）
class AsyncTrimManager {
public:
    void Start(AsyncTrim::TrimBatchFunc trim_fn);
    void Stop();
    void Notify() noexcept;
    void NotifyUrgent() noexcept;
    bool IsRunning() const noexcept;
private:
    std::unique_ptr<AsyncTrim> trim;
    // review v7 F2-4：mutable 让 const 方法 (IsRunning) 取锁不需要 const_cast
    mutable std::mutex mtx;
};

extern AsyncTrimManager g_async_trim;

} // namespace validation
} // namespace tbc

#endif // BITCOIN_VALIDATION_ASYNC_TRIM_H
