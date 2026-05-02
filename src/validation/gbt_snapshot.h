// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P3.6: GbtSnapshotProvider — 单 refresh worker + merge queue
//
// 旧路径：每次 getblocktemplate RPC 调用都 thread().detach() 起 worker 算 snapshot；
//         多个矿工同时打 GBT 会爆炸（10+ 矿工 × 5 秒一次 GBT = 100+ 临时线程）。
//
// 新路径：单 refresh worker 持续维护"最新 snapshot"；
//         RPC 路径来一个 GBT 请求 → push 到 merge queue → refresh worker 计算后回写 promise；
//         同时刻多请求合并（同一 chain tip 的 snapshot 复用，省重复算）。
//
// 当前 P3.6 阶段：基础架构 + 全局实例。getblocktemplate 真接入留 P3.6.b（需改 journaling_block_assembler）。

#ifndef BITCOIN_VALIDATION_GBT_SNAPSHOT_H
#define BITCOIN_VALIDATION_GBT_SNAPSHOT_H

#include "uint256.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

namespace tbc {
namespace validation {

// snapshot 结构（P3.6.b 真接入时填具体字段；当前是 stub）
struct GbtSnapshot {
    uint256 prev_hash;        // 基于哪个 tip 算的
    int64_t computed_at_us = 0;
    // 真 snapshot 字段（P3.6.b 接入时从 journaling_block_assembler 拷过来）：
    // - txns + sig_op_count + size + fees ...
    // 当前阶段只占位
};

class GbtSnapshotProvider {
public:
    using SnapshotPtr = std::shared_ptr<GbtSnapshot>;

    // RefreshFunc：基于当前 tip 算一个 snapshot（P3.6.b 接 journaling assembler）
    using RefreshFunc = std::function<SnapshotPtr()>;

    void Start(RefreshFunc refresh_fn);
    void Stop();
    bool IsRunning() const noexcept;

    // RPC 入口：请求一个新鲜 snapshot（block until refresh worker 算完）。
    // 同 tip 的 multi-request 会合并：refresh 一次，多 promise 都 set_value。
    SnapshotPtr GetSnapshot(int64_t timeout_ms = 5000);

    // 通知 refresh worker"tip 变了，重算"（ConnectBlock 完成后调）
    void NotifyTipChanged() noexcept;

    GbtSnapshotProvider() = default;
    ~GbtSnapshotProvider() { Stop(); }
    GbtSnapshotProvider(const GbtSnapshotProvider&) = delete;
    GbtSnapshotProvider& operator=(const GbtSnapshotProvider&) = delete;

private:
    void Run();

    RefreshFunc refresh_fn;
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> tip_changed{false};

    // 当前 cached snapshot（atomic shared_ptr 替换；读无锁）
    std::mutex cache_mtx;
    SnapshotPtr cached;

    // 等待 snapshot 的 promise 队列（同 tip 合并）
    std::mutex pending_mtx;
    std::vector<std::shared_ptr<std::promise<SnapshotPtr>>> pending;
    std::condition_variable refresh_cv;
};

extern GbtSnapshotProvider g_gbt_snapshot;

} // namespace validation
} // namespace tbc

#endif // BITCOIN_VALIDATION_GBT_SNAPSHOT_H
