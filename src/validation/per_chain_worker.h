// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P2.3: PerChainWorker — 单 worker 线程 + queue + cv
//
// 每个 worker 独立线程，从 queue 取 WorkItem 跑验证。
//   - Push：dispatcher 派发，notify cv
//   - Pop：worker 主循环，cv.wait 阻塞直到 queue 非空或停止
//   - Stop：优雅停止，drain queue 中所有项后退出
//
// P2.3 阶段：worker 主循环只调 stub HandleItem（不真验证 tx）；
//            真验证逻辑（doubleCheck / commit / Resubmit）在 P3.1-P3.3 接入

#ifndef BITCOIN_VALIDATION_PER_CHAIN_WORKER_H
#define BITCOIN_VALIDATION_PER_CHAIN_WORKER_H

#include "consensus/validation.h"     // CValidationState (sub-A12)
#include "primitives/transaction.h"   // CTransactionRef
#include "txn_validation_data.h"      // TxSource

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

class CNode;
using CNodePtr = std::shared_ptr<CNode>;

namespace tbc {
namespace validation {

using WorkerId = int32_t;

// v2.6.1 P5.1+P5.2: WorkItem 携带 promise 让 SubmitSync 阻塞等待结果。
//                   携带 source / pNode 让 P2P 路径保留 ban/orphan/relay 语义。
struct WorkItem {
    CTransactionRef tx;
    bool is_reorg_resubmit = false;

    // P5.2: P2P 上下文（rpc 路径为 nullptr / unknown）
    TxSource source = TxSource::unknown;
    CNodePtr pfrom;       // P2P 来源 peer，可空
    int64_t accept_time = 0;
    bool fLimitFree = true;
    int64_t nAbsurdFee = 0;

    // sub-A12 (task #182)：promise 类型 pair<bool,string> → CValidationState
    //   使用真实 reject_code / reject_reason / nDoS / IsMissingInputs() / fResubmitTx，
    //   支持 RPC handler / wallet / P2P 三个调用方按 prod 等价的方式取错误信息。
    std::shared_ptr<std::promise<CValidationState>> result_promise;

    WorkItem() = default;
    explicit WorkItem(CTransactionRef tx_) : tx(std::move(tx_)) {}
    WorkItem(WorkItem&&) noexcept = default;
    WorkItem& operator=(WorkItem&&) noexcept = default;
    WorkItem(const WorkItem&) = delete;
    WorkItem& operator=(const WorkItem&) = delete;

    // 便利：完成结果回写（CValidationState）
    void SetResult(CValidationState state) noexcept {
        if (result_promise) {
            try {
                result_promise->set_value(std::move(state));
            } catch (const std::future_error&) {
                // promise 已被 set 过（resubmit 重试场景）— 忽略
            }
        }
    }

    // 兼容 overload（sub-A12 → sub-A5 渐进迁移期）：caller 传 (bool ok, string err)
    //   ok=true：MODE_VALID；ok=false：MODE_INVALID + reject_reason=err
    //   reject_code 默认 REJECT_INVALID（caller 升级到 CValidationState 后弃用）
    void SetResult(bool ok, std::string err) noexcept {
        CValidationState state;
        if (!ok) {
            state.Invalid(false, REJECT_INVALID, std::move(err));
        }
        SetResult(std::move(state));
    }
};

class PerChainWorker {
public:
    // handler: 真验证回调，P3 阶段接 ProcessItem(tx)
    // P2.3 测试期：可传 noop / 计数 lambda
    using ItemHandler = std::function<void(WorkItem&&)>;

    // task #157：queue 上限。按 signal_dispatcher 同档位（mempool 远大于此 → 不会瓶颈）。
    static constexpr size_t MAX_QUEUE_DEPTH = 16384;

    PerChainWorker(WorkerId id, ItemHandler handler,
                   size_t capacity = MAX_QUEUE_DEPTH);
    ~PerChainWorker();

    // task #157：满 queue 时直接拒绝，调 item.SetResult(false, "queue full")
    //            返回 false 让 caller 决定（dispatcher reorg-resubmit 路径
    //            push g_reorg_stash / 普通路径直接 reject）。
    //            空 promise（fire-and-forget）也满则 drop + metric +1。
    bool Push(WorkItem&& item);
    size_t QueueSize() const;
    size_t Capacity() const noexcept { return capacity_; }

    // watchdog 用：worker 上次 progress 的 microseconds
    int64_t LastProgressUs() const noexcept {
        return last_progress_us.load(std::memory_order_relaxed);
    }

    void Stop();   // 优雅停止：drain queue 后退出（注意：drain 走 handler）

    PerChainWorker(const PerChainWorker&) = delete;
    PerChainWorker& operator=(const PerChainWorker&) = delete;

private:
    void Run();

    WorkerId id;
    ItemHandler handler;

    std::thread thread;
    std::atomic<bool> running{true};
    std::atomic<int64_t> last_progress_us{0};

    mutable std::mutex queue_mtx;   // LEVEL_WORKER_QUEUE = 5
    std::condition_variable cv;
    std::deque<WorkItem> queue;
    size_t capacity_;
};

} // namespace validation
} // namespace tbc

#endif // BITCOIN_VALIDATION_PER_CHAIN_WORKER_H
