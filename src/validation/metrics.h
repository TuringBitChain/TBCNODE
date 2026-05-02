// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// Phase pre-J (task #230) — dev metric 基础设施
//
// 用途：提供 v2.6.1 cs_main 重构期间运行时可观测性（仅供节点内部使用）。
//   - 全 atomic 计数器 + 几个 gauge（current depth）
//   - 不带 prom-client / 不引第三方依赖
//   - 不通过 RPC 对外暴露；仅供内部 LogPrintf / debug 路径读取
//
// 典型采集：
//   - worker queue depth (push 之前 / pop 之后)
//   - reorg 阻塞 worker 累计时长 (us)
//   - signal_dispatcher / async_subscriber 累计 dropped
//   - PerChainWorker push timeout / Stop 时 promise drain 数量
//
// 调用约定：
//   - 计数器：g_metrics.foo_total.fetch_add(1, std::memory_order_relaxed)
//   - gauge：g_metrics.bar_current.store(n, std::memory_order_release)
//   - 读：内部代码直接 load g_metrics.<field>（不对外暴露）

#ifndef BITCOIN_VALIDATION_METRICS_H
#define BITCOIN_VALIDATION_METRICS_H

#include <atomic>
#include <cstdint>

namespace tbc {
namespace validation {

struct DevMetrics {
    // ---- worker queue ----
    // 当前各 worker queue 深度之和（gauge，push/pop 时维护）
    std::atomic<uint64_t> worker_queue_depth_current{0};
    // 历史最大值（gauge max 推送）
    std::atomic<uint64_t> worker_queue_depth_max{0};
    // push 因 queue 满 timeout 而 drop / reject 的累计次数
    std::atomic<uint64_t> worker_queue_push_timeout_total{0};

    // ---- reorg 阻塞 ----
    // worker cv.wait_for(reorg_cv) 累计阻塞时长（us）
    std::atomic<uint64_t> reorg_blocked_us_total{0};
    // worker cv.wait_for(reorg_cv) timeout 次数（30s 默认）
    std::atomic<uint64_t> reorg_blocked_timeout_total{0};
    // 当前 reorg in-progress 是否阻塞 worker (0/1 gauge)
    std::atomic<uint64_t> reorg_in_progress_current{0};

    // ---- signal_dispatcher / async_subscriber ----
    // signal_dispatcher 队列满 drop 累计
    std::atomic<uint64_t> signal_dispatcher_dropped_total{0};
    // 各 AsyncSubscriber 实例 dropped overflow 总和
    std::atomic<uint64_t> async_subscriber_dropped_total{0};
    // shutdown 时未消费 task 总和
    std::atomic<uint64_t> async_subscriber_dropped_shutdown_total{0};

    // ---- PerChainWorker Stop 路径 ----
    // Stop 时 drain 出去（设 broken_promise）的 promise 数
    std::atomic<uint64_t> worker_stop_promise_drained_total{0};

    // ---- chain_dispatcher ----
    // 当前 in-flight tx 数（gauge）
    std::atomic<uint64_t> dispatcher_inflight_current{0};
    // 跨 worker 父子假 missing-inputs reject 累计
    std::atomic<uint64_t> dispatcher_cross_worker_missing_total{0};

    // ---- 三锁帧 ConnectTip ----
    // ConnectTip 三锁原子帧累计 us（用于 latency 监控）
    std::atomic<uint64_t> connecttip_three_lock_us_total{0};
    std::atomic<uint64_t> connecttip_three_lock_count{0};

    // 维护 worker_queue_depth_max（CAS-max，release on success 让 RPC reader
    // acquire-load 看到的 max 跟同一时刻的 current 同序）
    // review v5 MEDIUM：worker_queue_depth_current 改为 delta 增量而非全局 store。
    //   原 store(depth) 让多 worker 之间互相覆盖（last-writer-wins）让 RPC reader
    //   看到 last writer 的 depth 而非聚合。改用 fetch_add(delta) 让 current 是所有
    //   worker queue depth 之和。caller 必须传 (new_depth - old_depth) delta。
    void RecordQueueDepthDelta(int64_t delta) noexcept {
        if (delta == 0) return;
        if (delta > 0) {
            uint64_t after = worker_queue_depth_current.fetch_add(
                static_cast<uint64_t>(delta), std::memory_order_acq_rel) + delta;
            // CAS-max 仍然按"快照后总值"评估
            uint64_t prev = worker_queue_depth_max.load(std::memory_order_acquire);
            while (after > prev &&
                   !worker_queue_depth_max.compare_exchange_weak(
                       prev, after,
                       std::memory_order_release,
                       std::memory_order_acquire)) {}
        } else {
            // review v6 LOW：防 fetch_sub 在 Stop 双调或 caller 配对错误时 underflow
            //   wrap 到 UINT64_MAX 让 metric 永久污染。CAS-loop 限到底 0。
            const uint64_t to_sub = static_cast<uint64_t>(-delta);
            uint64_t prev = worker_queue_depth_current.load(std::memory_order_acquire);
            while (true) {
                const uint64_t next = (prev >= to_sub) ? (prev - to_sub) : 0;
                if (worker_queue_depth_current.compare_exchange_weak(
                        prev, next,
                        std::memory_order_release,
                        std::memory_order_acquire)) {
                    break;
                }
            }
        }
    }
    // 兼容老调用（保留 max 行为，但 current 不再写 — depth 由 caller 用 delta 维护）
    void RecordQueueDepth(uint64_t depth) noexcept {
        uint64_t prev = worker_queue_depth_max.load(std::memory_order_acquire);
        while (depth > prev &&
               !worker_queue_depth_max.compare_exchange_weak(
                   prev, depth,
                   std::memory_order_release,
                   std::memory_order_acquire)) {}
    }

    DevMetrics() = default;
    DevMetrics(const DevMetrics&) = delete;
    DevMetrics& operator=(const DevMetrics&) = delete;
};

// 全局实例（定义在 metrics.cpp）
extern DevMetrics g_metrics;

} // namespace validation
} // namespace tbc

#endif // BITCOIN_VALIDATION_METRICS_H
