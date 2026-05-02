// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P3.7 / H5 SignalDispatcher — 单线程异步 + per-tx FIFO 序号
//
// 解耦 worker commit 路径跟 wallet/ZMQ/REST/GBT subscriber 的反向锁顺序：
//   - worker commit 后调 Enqueue（不阻塞）
//   - 单 dispatch thread 顺序消费 queue，按 global_seq FIFO 转发给 subscriber
//   - subscriber 即使内部持锁（如 cs_wallet）也跟 worker 不同帧栈

#ifndef BITCOIN_VALIDATION_SIGNAL_DISPATCHER_H
#define BITCOIN_VALIDATION_SIGNAL_DISPATCHER_H

#include "logging.h"      // task #158: LogPrintf
#include "primitives/transaction.h"
#include "uint256.h"
#include "utiltime.h"     // task #158: GetTimeMicros
#include "validation/metrics.h"  // Phase pre-J (task #230): g_metrics

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace tbc {
namespace validation {

enum class SignalType : uint8_t {
    TransactionAddedToMempool,
    TransactionRemovedFromMempool,
    BlockConnected,
    BlockDisconnected,
};

struct SignalEvent {
    SignalType type;
    CTransactionRef tx;       // tx-related signals 用
    uint256 block_hash;       // block-related signals 用
    uint64_t global_seq = 0;  // FIFO 顺序号
};

class SignalDispatcher {
public:
    using SubscriberFn = std::function<void(const SignalEvent&)>;

    SignalDispatcher() = default;
    ~SignalDispatcher() { Stop(); }

    void Start() {
        running.store(true, std::memory_order_release);
        worker = std::thread([this] { Run(); });
    }

    void Stop() {
        // review v7 F-02：notify_all 必须跟 running=false 在同一 lock 帧内，
        //   防 worker 在 lock 释放跟 notify 之间从 Enqueue 醒来重判 predicate
        //   后再回 wait → notify 击中空 cv → join 永挂。
        {
            std::lock_guard l(mtx);
            running.store(false, std::memory_order_release);
            cv.notify_all();
        }
        if (worker.joinable()) worker.join();
        // review v7 F2-10：join 之后 worker 已退出，但 Stop 之后 Enqueue 可能仍
        //   入队（shutdown race）让 CTransactionRef 留在 deque → ref-count 泄漏到
        //   程序退出。清空 queue 让 shared_ptr 立即释放。
        std::lock_guard l(mtx);
        queue.clear();
    }

    // Phase E (task #158)：cap + drop-oldest 防无界增长
    static constexpr size_t MAX_QUEUE_DEPTH = 16384;

    // worker / ConnectBlock 调（不阻塞）
    void Enqueue(SignalType type, CTransactionRef tx) {
        SignalEvent e;
        e.type = type;
        e.tx = std::move(tx);
        e.global_seq = global_seq.fetch_add(1, std::memory_order_relaxed);
        bool dropped = false;
        {
            std::lock_guard l(mtx);
            // review v7 F2-10：Stop 之后 push 会留 CTransactionRef 不被消费 → 泄漏
            if (!running.load(std::memory_order_acquire)) return;
            if (queue.size() >= MAX_QUEUE_DEPTH) {
                queue.pop_front();
                dropped = true;
            }
            queue.push_back(std::move(e));
            cv.notify_one();   // F-02 同款：notify 锁内
        }
        if (dropped) MaybeWarnDropped();
    }

    void EnqueueBlock(SignalType type, const uint256& block_hash) {
        SignalEvent e;
        e.type = type;
        e.block_hash = block_hash;
        e.global_seq = global_seq.fetch_add(1, std::memory_order_relaxed);
        bool dropped = false;
        {
            std::lock_guard l(mtx);
            if (!running.load(std::memory_order_acquire)) return;
            if (queue.size() >= MAX_QUEUE_DEPTH) {
                queue.pop_front();
                dropped = true;
            }
            queue.push_back(std::move(e));
            cv.notify_one();   // F-02 同款：notify 锁内
        }
        if (dropped) MaybeWarnDropped();
    }

private:
    // Phase E (task #158)：drop 日志聚合，每秒最多 1 条 WARNING
    std::atomic<int64_t> last_warn_us_{0};
    std::atomic<uint64_t> dropped_total_{0};
    void MaybeWarnDropped() {
        const uint64_t total = dropped_total_.fetch_add(1, std::memory_order_relaxed) + 1;
        // Phase pre-J (task #230)：metric 钩子
        g_metrics.signal_dispatcher_dropped_total.fetch_add(1, std::memory_order_relaxed);
        const int64_t now_us = GetTimeMicros();
        const int64_t prev = last_warn_us_.load(std::memory_order_acquire);
        if (now_us - prev >= 1'000'000) {
            int64_t expected = prev;
            if (last_warn_us_.compare_exchange_strong(expected, now_us)) {
                LogPrintf("WARNING SignalDispatcher: queue full, dropped oldest (total=%u)\n",
                          static_cast<unsigned>(total));
            }
        }
    }
public:

    void Subscribe(SubscriberFn fn) {
        std::lock_guard l(sub_mtx);
        subscribers.push_back(std::move(fn));
    }

    size_t QueueSize() const {
        std::lock_guard l(mtx);
        return queue.size();
    }

    SignalDispatcher(const SignalDispatcher&) = delete;
    SignalDispatcher& operator=(const SignalDispatcher&) = delete;

private:
    void Run() {
        while (true) {
            SignalEvent e;
            {
                std::unique_lock l(mtx);
                cv.wait(l, [this] {
                    return !queue.empty() || !running.load(std::memory_order_acquire);
                });
                if (!running.load(std::memory_order_acquire) && queue.empty()) return;
                if (queue.empty()) continue;
                e = std::move(queue.front());
                queue.pop_front();
            }

            std::vector<SubscriberFn> subs_copy;
            {
                std::lock_guard l(sub_mtx);
                subs_copy = subscribers;
            }
            for (auto& s : subs_copy) {
                try { s(e); } catch (...) {}   // subscriber 异常不传播
            }
        }
    }

    mutable std::mutex mtx;
    std::condition_variable cv;
    std::deque<SignalEvent> queue;
    std::atomic<uint64_t> global_seq{0};
    std::atomic<bool> running{false};

    std::mutex sub_mtx;
    std::vector<SubscriberFn> subscribers;

    std::thread worker;
};

// v2.6.1 M2: 全局实例（定义在 signal_dispatcher.cpp）
extern SignalDispatcher g_signal_dispatcher;

} // namespace validation
} // namespace tbc

#endif // BITCOIN_VALIDATION_SIGNAL_DISPATCHER_H
