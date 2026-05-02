// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P4 §5.3 AsyncSubscriber — 单线程 FIFO 异步消费框架
//
// 用途：把 CValidationInterface subscriber 的 callback 实体放到独立 worker 线程，
//      让 ConnectTip 三锁帧（cs_main + mempool.smtx unique + batchWriteMtx unique）
//      内调 GetMainSignals().XXX() 时只 push 一笔 task 即返回，不阻塞主验证链。
//
// 设计要点：
//   - 单线程 worker：保证 per-subscriber FIFO 消息顺序（不需要 global_seq）
//   - 容量上限（默认 16384）+ 丢消息 WARNING：避免 subscriber 阻塞拖死内存
//   - subscriber 抛异常不传播：worker 线程吞掉，记 LogPrintf
//   - Stop 幂等：可在 shutdown 早期调，drain 到空再退出
//   - 不依赖 boost / 不引新依赖（std::thread + std::mutex + std::condition_variable）
//
// 锁层级：
//   AsyncSubscriber 内部 queue_mtx 是叶子层（不调用任何 user-mutex 函数）
//   subscriber callback 在 worker 线程上下文执行，已脱离 ConnectTip 帧栈，
//   就算 callback 内取 cs_main / mempool.smtx 也不构成反向锁

#ifndef BITCOIN_VALIDATION_ASYNC_SUBSCRIBER_H
#define BITCOIN_VALIDATION_ASYNC_SUBSCRIBER_H

#include "logging.h"
#include "validation/metrics.h"  // Phase pre-J (task #230): g_metrics

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace tbc {
namespace validation {

// 单消费者 FIFO 异步派发器。
//   - Enqueue 不阻塞（满了丢一条 + WARNING）
//   - worker 线程串行消费，保证 push 顺序
//   - subscriber 用 std::function<void()> 闭包形式注入（在 enqueue 点把入参拷贝进闭包）
class AsyncSubscriber {
public:
    using Task = std::function<void()>;

    explicit AsyncSubscriber(std::string name,
                             std::size_t capacity = 16'384)
        : m_name(std::move(name)), m_capacity(capacity) {}

    ~AsyncSubscriber() { Stop(); }

    void Start() {
        bool expected = false;
        if (!m_running.compare_exchange_strong(expected, true,
                                               std::memory_order_acq_rel)) {
            return; // 已 start
        }
        m_worker = std::thread([this] { Run(); });
    }

    // 幂等：可重复调，已 stop 直接返回
    void Stop() {
        bool expected = true;
        if (!m_running.compare_exchange_strong(expected, false,
                                               std::memory_order_acq_rel)) {
            return; // 未 start 或已 stop
        }
        // Phase E (task #158)：notify_all 必须在 lock_guard 内一帧原子，
        //   防 worker 在 m_running CAS 之后、notify 之前进入 wait 错过唤醒导致 join 永挂。
        //   原代码空 lock_guard + 锁外 notify 之间存在 race window。
        {
            std::lock_guard<std::mutex> l(m_mtx);
            m_cv.notify_all();
        }
        if (m_worker.joinable()) m_worker.join();
        // L2 (post-Teranode-audit)：second lock — 清残留必须在 worker join 之后，
        //   防 worker 仍在持锁消费 task。注意：此处不能跟上面 lock 合并 — join
        //   必须在锁外（worker 退出条件 predicate 在 wait_for 内取同一锁）。
        // 清空残留（已 push 但未消费的 task 丢弃 — shutdown 路径，可接受）
        std::lock_guard<std::mutex> l(m_mtx);
        std::size_t leftover = m_queue.size();
        if (leftover > 0) {
            LogPrintf("AsyncSubscriber[%s]: %u tasks dropped on Stop\n",
                      m_name, static_cast<unsigned>(leftover));
            // Phase pre-J (task #230)：metric 钩子
            g_metrics.async_subscriber_dropped_shutdown_total.fetch_add(
                static_cast<uint64_t>(leftover), std::memory_order_relaxed);
        }
        m_queue.clear();
    }

    // 不阻塞 push；满了丢一条 + WARNING（保留 FIFO，新条目从队尾插）
    // review v7 F-25：m_running 检查跟 lock_guard 之间 Stop 可能完整跑完
    //   (CAS+notify+join+clear)，task push 进永远不会被消费的 queue → broken_promise。
    //   修法：lock 内重判 m_running，stop 后丢弃 + 计 metric。
    void Enqueue(Task t) {
        if (!m_running.load(std::memory_order_acquire)) {
            m_dropped_not_running.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        bool dropped = false;
        {
            std::lock_guard<std::mutex> l(m_mtx);
            // 锁内重判，关 TOCTOU 窗口
            if (!m_running.load(std::memory_order_acquire)) {
                m_dropped_not_running.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (m_queue.size() >= m_capacity) {
                m_queue.pop_front();
                dropped = true;
                m_dropped_overflow.fetch_add(1, std::memory_order_relaxed);
                // Phase pre-J (task #230)：metric 钩子（per-instance + 全局聚合）
                g_metrics.async_subscriber_dropped_total.fetch_add(
                    1, std::memory_order_relaxed);
            }
            m_queue.push_back(std::move(t));
            m_cv.notify_one();   // F-12 同款：notify 在 lock 内
        }
        if (dropped) {
            // 在锁外打 log，避免 logging 卡住 push
            LogPrintf("WARNING: AsyncSubscriber[%s] queue full (cap=%u), "
                      "dropped oldest task\n",
                      m_name, static_cast<unsigned>(m_capacity));
        }
    }

    std::size_t QueueSize() const {
        std::lock_guard<std::mutex> l(m_mtx);
        return m_queue.size();
    }

    uint64_t DroppedOverflow() const noexcept {
        return m_dropped_overflow.load(std::memory_order_relaxed);
    }
    uint64_t DroppedNotRunning() const noexcept {
        return m_dropped_not_running.load(std::memory_order_relaxed);
    }

    AsyncSubscriber(const AsyncSubscriber&) = delete;
    AsyncSubscriber& operator=(const AsyncSubscriber&) = delete;

private:
    void Run() {
        while (true) {
            Task t;
            {
                std::unique_lock<std::mutex> l(m_mtx);
                m_cv.wait(l, [this] {
                    return !m_queue.empty() ||
                           !m_running.load(std::memory_order_acquire);
                });
                if (!m_running.load(std::memory_order_acquire) &&
                    m_queue.empty()) {
                    return;
                }
                if (m_queue.empty()) continue;
                t = std::move(m_queue.front());
                m_queue.pop_front();
            }
            try {
                t();
            } catch (const std::exception& e) {
                LogPrintf("WARNING: AsyncSubscriber[%s] task threw: %s\n",
                          m_name, e.what());
            } catch (...) {
                LogPrintf("WARNING: AsyncSubscriber[%s] task threw "
                          "unknown exception\n",
                          m_name);
            }
        }
    }

    const std::string m_name;
    const std::size_t m_capacity;

    mutable std::mutex m_mtx;
    std::condition_variable m_cv;
    std::deque<Task> m_queue;
    std::atomic<bool> m_running{false};
    std::thread m_worker;

    std::atomic<uint64_t> m_dropped_overflow{0};
    std::atomic<uint64_t> m_dropped_not_running{0};
};

} // namespace validation
} // namespace tbc

#endif // BITCOIN_VALIDATION_ASYNC_SUBSCRIBER_H
