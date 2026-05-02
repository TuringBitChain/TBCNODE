// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P2.1: ChainDispatcher 骨架 + 16-shard inflight 状态机
//
// ChainDispatcher 负责把客户端提交的 tx 路由到 PerChainWorker：
//   - 同一子孙链路由到同一 worker（保证父先 commit、子后 commit）
//   - 独立链路由到最空闲 worker（power-of-two-choices）
//   - 16-shard inflight 表跟踪 tx 状态（QUEUED/RUNNING/COMMITTED/ABORTED）
//   - GC 线程 5ms 后清理 COMMITTED 项（给同链子 tx 路由窗口）
//
// 当前 P2.1：仅骨架（数据结构 + 状态机转换 API）；
//            路由 / worker pool / TopoSort / 异常处理在 P2.2-P2.8 渐进实现
//
// 锁层级（详见 src/validation/lock_hierarchy.h）：
//   inflight_shard_mtx (level 4) > worker.queue_mtx (level 5)

#ifndef BITCOIN_VALIDATION_CHAIN_DISPATCHER_H
#define BITCOIN_VALIDATION_CHAIN_DISPATCHER_H

#include "consensus/validation.h"     // CValidationState (sub-A12)
#include "primitives/transaction.h"   // TxId, CTransactionRef
#include "txn_validation_data.h"      // TxSource

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

class CNode;

namespace tbc {
namespace validation {

class PerChainWorker;

using WorkerId = int32_t;
constexpr WorkerId WORKER_NONE = -1;

enum class InflightStatus : uint8_t {
    QUEUED = 0,      // 已派发到 worker，未开始
    RUNNING = 1,     // worker 正在验证
    COMMITTED = 2,   // 已 commit（5ms 后 GC）
    ABORTED = 3,     // worker 异常 / Resubmit 超限（GC 立即清）
};

struct InflightEntry {
    WorkerId worker = WORKER_NONE;
    InflightStatus status = InflightStatus::QUEUED;
    int64_t commit_time_us = 0;   // COMMITTED 后保留 5ms 给同链子 tx 路由
};

constexpr size_t INFLIGHT_SHARDS = 16;
// COMMITTED 项 GC 延迟（us）。父 commit 后保留这段时间给同链子 tx 的
//   FindWorkerForChain 命中窗口。慢 RPC 客户端 / 跨网络链路 RTT > 5ms 时
//   可上调（init.cpp 通过 -dispatchercommitgcms 设置；单位 ms）。
//   合法范围 [1ms, 100ms]；超出 clamp。设过大让 inflight 表占用过多内存。
constexpr int64_t COMMITTED_GC_DELAY_US_DEFAULT = 5'000;
constexpr int64_t COMMITTED_GC_DELAY_US_MIN = 1'000;
constexpr int64_t COMMITTED_GC_DELAY_US_MAX = 100'000;
extern std::atomic<int64_t> g_committed_gc_delay_us;

class ChainDispatcher {
public:
    ChainDispatcher();    // .cpp 定义（vector<unique_ptr<PerChainWorker>> 需 PerChainWorker 完整类型）
    ~ChainDispatcher();   // .cpp 定义（同上）

    // 状态机 API（P2.1 骨架）
    void MarkQueued(const TxId& txid, WorkerId worker);
    void MarkRunning(const TxId& txid);
    void MarkCommitted(const TxId& txid);
    void MarkAborted(const TxId& txid);

    // 查询：tx 是否在 inflight + 当前 worker / 状态
    bool TryGetInflight(const TxId& txid, InflightEntry& out) const;
    size_t InflightSize() const;

    // ============================================================================
    // v2.6.1 P2.2: 路由策略
    // ============================================================================
    // FindWorkerForChain：查 tx 的 input 父 txid 在 inflight 中的 worker 投票
    //   - 同子孙链路由到同 worker（保证父先 commit、子后 commit）
    //   - 找不到任何父 → 返回 WORKER_NONE（调用方走 PickLeastLoadedWorker）
    //   - MAX_INPUT_SCAN = 100：防 DoS（恶意 tx 上千 input）
    //   - ABORTED 状态不计投票
    static constexpr int MAX_INPUT_SCAN = 100;
    WorkerId FindWorkerForChain(const class CTransaction& tx) const;

    // PickLeastLoadedWorker：power-of-two-choices 选最空闲 worker
    //   P2.2 阶段：stub（基于 inflight 表 worker 计数选最少的 2 个之一），需 P2.3 worker pool 真实现
    //   total_workers <= 0 → WORKER_NONE
    WorkerId PickLeastLoadedWorker(int total_workers) const;

    // ============================================================================
    // v2.6.1 P5.1+P5.2: Worker pool lifecycle + Submit 路径
    // ============================================================================
    // ValidationHandler：dispatcher 派给 worker 执行的 callback。
    //   入参 item 携带完整上下文：tx / source / pfrom / accept_time / fLimitFree / nAbsurdFee。
    //   返回 true 表示验证通过（已进 mempool / chain），false 表示拒绝（err 含原因）。
    //   handler 内部包旧 PTV processValidation，保证语义零回归。
    using ValidationHandler =
        std::function<bool(const struct WorkItem&, std::string&)>;

    // Start：创建 num_workers 个 PerChainWorker，注册 handler。
    //   幂等：已 Start 直接返回。
    //   num_workers <= 0 → 退化为 1。
    void Start(int num_workers, ValidationHandler handler);

    // H2 修补：handler 可以在 Start 之前提前 set，让启动早期 / 关停期 fallback 路径
    //         走 PTV 直调（而不是返回 "dispatcher not started" 错误）。
    void SetFallbackHandler(ValidationHandler handler);

    // Stop：通知所有 worker 退出，join 后清空 pool。
    //   幂等：未 Start 直接返回。
    //   注意：Stop 之后 Start 可重新启动（dev 调试）。
    void Stop();

    bool IsStarted() const noexcept {
        return started.load(std::memory_order_acquire);
    }
    int NumWorkers() const noexcept {
        return num_workers_count.load(std::memory_order_acquire);
    }

    // SubmitSync：阻塞直到 worker 处理完，返回验证结果。
    //   未 Start → 直接调 handler 同步执行（兼容 dev fallback）。
    //   全参数 overload：source/absurdFee/fLimitFree/accept_time 由调用方按入口语义传入。
    bool SubmitSync(const CTransactionRef& tx, std::string& err,
                    TxSource source,
                    int64_t absurdFee,
                    bool fLimitFree,
                    int64_t accept_time = 0);
    // 简单 overload（默认 RPC + maxTxFee + 不限 free）
    bool SubmitSync(const CTransactionRef& tx, std::string& err);

    // sub-A12 (task #156)：返回 CValidationState 的 overload — wallet / RPC 客户端
    //   可拿到完整 reject_code / IsMissingInputs / IsResubmittedTx，不再被压平为
    //   REJECT_INVALID 字符串。
    CValidationState SubmitSyncState(const CTransactionRef& tx,
                                     TxSource source,
                                     int64_t absurdFee,
                                     bool fLimitFree,
                                     int64_t accept_time = 0);

    // H5 / sub-A12: 返回 future 的 Submit（不阻塞，调用方自行 wait_for budget）
    //     用于 submitrawtransactions 等批量场景：N 笔并行派发。
    //     sub-A12：promise 类型迁 CValidationState（保留全 reject 信息透传 RPC）。
    std::shared_future<CValidationState>
    SubmitForFuture(const CTransactionRef& tx,
                    TxSource source,
                    int64_t absurdFee,
                    bool fLimitFree,
                    int64_t accept_time = 0);

    // SubmitAsync：派发后立即返回（fire-and-forget）。
    //   未 Start → 直接调 handler 同步执行（兼容 dev fallback）。
    //   全参数 overload：non-P2P 异步入口（reorg / finalised / file）。
    void SubmitAsync(const CTransactionRef& tx,
                     TxSource source,
                     int64_t absurdFee = 0,
                     bool fLimitFree = true,
                     int64_t accept_time = 0);
    // 简单 overload（默认 P2P 语义）
    void SubmitAsync(const CTransactionRef& tx);

    // P5.2: P2P 专用 SubmitAsync — 携带 pfrom（peer 指针）+ tx context。
    //   handler 可拿到 pfrom 做 ban / orphan 缓存 / 关联 relay。
    //   未 Start → 同步 handler fallback（无 pfrom 上下文，退化）。
    void SubmitAsyncP2P(const CTransactionRef& tx,
                        std::shared_ptr<CNode> pfrom,
                        int64_t accept_time,
                        bool fLimitFree,
                        int64_t nAbsurdFee);

    // 不允许复制 / 移动
    ChainDispatcher(const ChainDispatcher&) = delete;
    ChainDispatcher& operator=(const ChainDispatcher&) = delete;

private:
    struct Shard {
        mutable std::shared_mutex mtx;   // LEVEL_INFLIGHT_SHARD = 4
        std::unordered_map<TxId, InflightEntry> map;
    };
    std::array<Shard, INFLIGHT_SHARDS> shards;

    static size_t ShardOf(const TxId& txid) noexcept {
        // 用 TxId 高 64-bit 作为 shard 选择（uint256 第一个 64-bit word）
        return std::hash<TxId>{}(txid) % INFLIGHT_SHARDS;
    }

    // C3 修补：worker pool 用 shared_ptr COW + atomic<bool> stopping，
    //          Submit* 路径取 snapshot 后 Push 不持任何锁，跟 Stop 无死锁路径。
    using WorkersVec = std::vector<std::unique_ptr<PerChainWorker>>;
    std::shared_ptr<WorkersVec> workers_snapshot;
    mutable std::mutex workers_mutate_mtx;   // 仅保护 Start/Stop 互斥（不参与 Submit hot path）
    std::atomic<bool> started{false};
    std::atomic<bool> stopping{false};       // C3: stop 信号，Submit 进入前快速检查
    std::atomic<int> num_workers_count{0};
    ValidationHandler handler_cb;

    // Phase B (post-Teranode-audit)：retry_threads 后台机制已删除。
    //   reorg-resubmit race 失败 → 直接 push 回 g_reorg_stash，drain 路径下次重派。
    //   消除 H1 retry_threads 无界增长 OOM 风险。

    // 选 worker：先按 input 父 chain 投票，没有就 power-of-two
    WorkerId RouteWorker(const CTransaction& tx) const;

    // 内部：取 workers snapshot（无锁，atomic shared_ptr load）
    std::shared_ptr<WorkersVec> GetWorkers() const noexcept {
        return std::atomic_load_explicit(&workers_snapshot,
                                         std::memory_order_acquire);
    }

    // P2.6 GC 线程
    std::thread gc_thread;
    std::atomic<bool> gc_running{false};
    void RunGc();
    // 配置常量
    static constexpr int64_t GC_INTERVAL_MS = 1000;     // 每秒扫一次
    static constexpr int64_t LONG_RUNNING_TIMEOUT_US = 30 * 1'000'000;  // 30s 视为卡死
    // 监控
    std::atomic<uint64_t> gc_committed_cleaned{0};
    std::atomic<uint64_t> gc_aborted_cleaned{0};
    std::atomic<uint64_t> gc_long_running_cleaned{0};

    // P2.7 Watchdog 线程
    std::thread watchdog_thread;
    std::atomic<bool> watchdog_running{false};
    void RunWatchdog();
    static constexpr int64_t WATCHDOG_INTERVAL_MS = 5000;       // 5s 扫一次
    static constexpr int64_t WORKER_STALL_THRESHOLD_US = 60 * 1'000'000;  // 60s 没进度告警
    std::atomic<uint64_t> watchdog_stall_alerts{0};

    // H3: stash drain 后台线程（替代 UpdateTip 内 cs_main 持锁 drain）
    std::thread drain_thread;
    std::atomic<bool> drain_running{false};
    std::condition_variable drain_cv;
    std::mutex drain_cv_mtx;
    std::atomic<uint32_t> drain_signal_seq{0};   // 防 notify 漏发
    void RunDrainStash();
public:
    // 给 UpdateTip 调：唤醒 drain thread（无锁，nofification 仅 signal_seq+1 + cv.notify）
    void NotifyDrainStash() noexcept {
        drain_signal_seq.fetch_add(1, std::memory_order_release);
        drain_cv.notify_one();
    }
private:
public:
    struct GcMetrics {
        uint64_t committed_cleaned;
        uint64_t aborted_cleaned;
        uint64_t long_running_cleaned;
    };
    GcMetrics GetGcMetrics() const noexcept {
        return {
            gc_committed_cleaned.load(std::memory_order_relaxed),
            gc_aborted_cleaned.load(std::memory_order_relaxed),
            gc_long_running_cleaned.load(std::memory_order_relaxed),
        };
    }
    uint64_t GetWatchdogStallAlerts() const noexcept {
        return watchdog_stall_alerts.load(std::memory_order_relaxed);
    }
};

// 全局 dispatcher 实例（P2.1 引入；P5 RPC 入口接入）
extern ChainDispatcher g_dispatcher;

} // namespace validation
} // namespace tbc

#endif // BITCOIN_VALIDATION_CHAIN_DISPATCHER_H
