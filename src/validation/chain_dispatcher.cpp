// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.

#include "validation/chain_dispatcher.h"

#include "logging.h"
#include "net/net.h"                  // CNode (P2P pfrom 上下文 GetId)
#include "primitives/transaction.h"   // CTransaction / TxId
#include "utiltime.h"                 // GetTimeMicros
#include "validation/chainstate.h"    // g_reorg_epoch (sub-A4 review L1)
#include "validation/metrics.h"       // g_metrics (sub-A4 review M1)
#include "validation/per_chain_worker.h"
#include "validation/tx_stash.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <string>
#include <thread>
#include <utility>

namespace tbc {
namespace validation {

// P3.2: parse err 字符串里的 "race-retry: <reason>" 前缀（worker_validate.cpp 写）
static bool IsRaceRetryError(const std::string& err) noexcept {
    return err.rfind("race-retry:", 0) == 0;
}

// review v7 a5 HIGH：SubmitSync stall WARNING rate limit（每秒最多 1 条）
//   防 worker stall 时 N 笔 in-flight tx 各自 30s 后都打 WARNING 炸 log。
static std::atomic<int64_t> g_last_stall_warn_us{0};
static bool ShouldEmitStallWarning() noexcept {
    const int64_t now_us = GetTimeMicros();
    int64_t prev = g_last_stall_warn_us.load(std::memory_order_acquire);
    if (now_us - prev < 1'000'000) return false;
    return g_last_stall_warn_us.compare_exchange_strong(
        prev, now_us, std::memory_order_acq_rel);
}

// review v7 a5: 30s wait quantum 共享常量（防 SubmitSync/SubmitSyncState 各自定义漂移）
static constexpr int64_t kWaitQuantumUs = 30 * 1'000'000LL;

// sub-A4 review M1 (task #235)：跨 worker 父子假 missing-inputs 检测
//   race-retry 失败原因含 "missing-inputs" / "missingorspent" → 父在 sibling worker，
//   计 metric 让运维识别 worker 路由不均导致的 false reject 频率。
static bool IsCrossWorkerMissingInputs(const std::string& err) noexcept {
    return err.find("missing-inputs") != std::string::npos
        || err.find("missingorspent") != std::string::npos;
}

void ChainDispatcher::MarkQueued(const TxId& txid, WorkerId worker) {
    Shard& shard = shards[ShardOf(txid)];
    {
        std::unique_lock lock(shard.mtx);
        const bool was_new = (shard.map.find(txid) == shard.map.end());
        InflightEntry& e = shard.map[txid];
        e.worker = worker;
        e.status = InflightStatus::QUEUED;
        // P2.6: commit_time_us 用作 enqueue_time（QUEUED/RUNNING 时表示入队时刻），
        //       GC 检测 30s 没进展即视为卡死强清。
        e.commit_time_us = GetTimeMicros();
        // review v7 F-21：fetch_add 必须在 lock 内 — 锁外做的话 GC 可以在 lock
        //   释放跟 fetch_add 之间 erase entry 并 fetch_sub，序列变成 sub→add，
        //   gauge 短暂 underflow wrap UINT64_MAX。锁内做让 add/sub 严格有序。
        if (was_new) {
            g_metrics.dispatcher_inflight_current.fetch_add(
                1, std::memory_order_release);
        }
    }
}

void ChainDispatcher::MarkRunning(const TxId& txid) {
    Shard& shard = shards[ShardOf(txid)];
    std::unique_lock lock(shard.mtx);
    auto it = shard.map.find(txid);
    if (it != shard.map.end()) {
        it->second.status = InflightStatus::RUNNING;
        // 不重置 commit_time_us — 保留入队时刻，GC 算总在 inflight 时长
    }
}

void ChainDispatcher::MarkCommitted(const TxId& txid) {
    Shard& shard = shards[ShardOf(txid)];
    std::unique_lock lock(shard.mtx);
    auto it = shard.map.find(txid);
    if (it != shard.map.end()) {
        it->second.status = InflightStatus::COMMITTED;
        it->second.commit_time_us = GetTimeMicros();
    }
}

void ChainDispatcher::MarkAborted(const TxId& txid) {
    Shard& shard = shards[ShardOf(txid)];
    std::unique_lock lock(shard.mtx);
    auto it = shard.map.find(txid);
    if (it != shard.map.end()) {
        it->second.status = InflightStatus::ABORTED;
        // ABORTED 立即清理（GC 不再延迟）
        it->second.commit_time_us = GetTimeMicros();
    }
}

bool ChainDispatcher::TryGetInflight(const TxId& txid, InflightEntry& out) const {
    const Shard& shard = shards[ShardOf(txid)];
    std::shared_lock lock(shard.mtx);
    auto it = shard.map.find(txid);
    if (it == shard.map.end()) return false;
    out = it->second;
    return true;
}

size_t ChainDispatcher::InflightSize() const {
    size_t total = 0;
    for (const Shard& shard : shards) {
        std::shared_lock lock(shard.mtx);
        total += shard.map.size();
    }
    return total;
}

// ============================================================================
// v2.6.1 P2.2: 路由策略
// ============================================================================

WorkerId ChainDispatcher::FindWorkerForChain(const CTransaction& tx) const {
    // 防 DoS：扫 input 上限 100（恶意 tx 上千 input 时也只看前 100 个）
    int scan = std::min<int>(static_cast<int>(tx.vin.size()), MAX_INPUT_SCAN);

    // first-hit 路由：任一 input 父在 inflight → 路到那个 worker。
    //   语义：父分散在多 worker 时，子无论路到哪都至少有一个父没 commit → 子
    //   会 missing-input reject 让客户端重提交。所以"投票最多 worker" 没有
    //   收益（doubleCheck 阶段都串行在 mempool.smtx 上），只是徒增 alloc 和
    //   max_element 开销。命中第一个就 break，O(命中索引) 平均。
    for (int i = 0; i < scan; i++) {
        const TxId& parent_txid = tx.vin[i].prevout.GetTxId();
        const Shard& shard = shards[ShardOf(parent_txid)];
        std::shared_lock lock(shard.mtx);
        auto it = shard.map.find(parent_txid);
        if (it == shard.map.end()) continue;
        // ABORTED 跳过（worker crash 后清理路径，路过去等 GC）
        if (it->second.status == InflightStatus::ABORTED) continue;
        return it->second.worker;
    }
    return WORKER_NONE;
}

WorkerId ChainDispatcher::PickLeastLoadedWorker(int total_workers) const {
    if (total_workers <= 0) return WORKER_NONE;
    if (total_workers == 1) return 0;

    // v2.6.1 P2.3 真切（设计文档 §2.1）：power-of-two-choices 用 worker.QueueSize() 真 O(1)
    //   旧 stub 全 16 shard 扫 inflight 表 O(N)；新版 O(1) atomic snapshot + 2 次 mutex queue.size()
    //   双 agent 审核通过（v3）。
    thread_local std::mt19937 rng = []{
        std::seed_seq seq{
            std::random_device{}(),
            static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id())),
            static_cast<uint32_t>(GetTimeMicros())
        };
        return std::mt19937(seq);
    }();

    int a = static_cast<int>(rng() % static_cast<uint32_t>(total_workers));
    int b = static_cast<int>(rng() % static_cast<uint32_t>(total_workers));

    auto snap = GetWorkers();
    if (!snap || snap->empty()) return 0;
    if (a >= static_cast<int>(snap->size())) a = 0;
    if (b >= static_cast<int>(snap->size())) b = 0;
    if (a == b) return a;

    // 即使 a==b 命中前才 return，这里保证 a/b 都 < snap.size 且不等，正常 power-of-two
    const size_t qa = (*snap)[a]->QueueSize();
    const size_t qb = (*snap)[b]->QueueSize();
    return qa <= qb ? a : b;
}

// ============================================================================
// v2.6.1 M4: Worker pool lifecycle + Submit 路径
// ============================================================================

WorkerId ChainDispatcher::RouteWorker(const CTransaction& tx) const {
    // 1. 先按 input 父 chain 投票
    WorkerId w = FindWorkerForChain(tx);
    if (w != WORKER_NONE) return w;
    // 2. 没有同链父 → power-of-two-choices
    int n = num_workers_count.load(std::memory_order_acquire);
    return PickLeastLoadedWorker(n);
}

void ChainDispatcher::Start(int num_workers, ValidationHandler handler) {
    std::lock_guard<std::mutex> mutate_lock(workers_mutate_mtx);
    // 幂等：已 Start 直接返回
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true,
                                         std::memory_order_acq_rel)) {
        return;
    }
    stopping.store(false, std::memory_order_release);
    if (num_workers <= 0) num_workers = 1;
    handler_cb = std::move(handler);

    auto new_workers = std::make_shared<WorkersVec>();
    new_workers->reserve(static_cast<size_t>(num_workers));
    // review v7 F-26 + H2 (post-Teranode-audit)：worker 构造 *和* gc/watchdog/drain
    //   线程构造都可能 throw std::system_error (pthread_create EAGAIN)。三个 helper
    //   线程的 std::thread 构造也必须在 try 内，否则若 watchdog_thread 构造抛 →
    //   gc_running/gc_thread 已起 + watchdog_running 已设但 thread 未起 + flag 半态
    //   "started but no threads" 僵尸态，析构不 join 任何 thread → UAF。
    try {
    // P2.6: 启动 GC 线程
    gc_running.store(true, std::memory_order_release);
    gc_thread = std::thread([this] { RunGc(); });
    // P2.7: 启动 Watchdog 线程
    watchdog_running.store(true, std::memory_order_release);
    watchdog_thread = std::thread([this] { RunWatchdog(); });
    // H3: 启动 Drain Stash 后台线程（每 500ms 醒一次或被 NotifyDrainStash 唤醒）
    drain_running.store(true, std::memory_order_release);
    drain_thread = std::thread([this] { RunDrainStash(); });
    for (int i = 0; i < num_workers; i++) {
        WorkerId wid = static_cast<WorkerId>(i);
        // P5.1 真接入：worker 主循环跑这个 lambda。
        //   1. inflight QUEUED → RUNNING
        //   2. 调 handler_cb 真验证（旧 PTV processValidation）
        //   3. inflight RUNNING → COMMITTED / ABORTED
        //   4. 把结果回写到 WorkItem.result_promise（SubmitSync 调用方在 future 上等）
        auto worker_handler = [this, wid](WorkItem&& item) {
            if (!item.tx) {
                CValidationState st;
                st.Invalid(false, REJECT_INVALID, "null tx");
                item.SetResult(std::move(st));
                return;
            }
            const TxId txid = item.tx->GetId();
            MarkRunning(txid);
            // v3.4.0 finding 1 修：透传完整 CValidationState（保 IsMissingInputs /
            // IsResubmittedTx / 真实 reject_code）。worker_handler 现在填 outState 而不是
            // 返回 (bool, string)。
            CValidationState state;
            // v3.4.0 finding 2' 修：no-handler 必须显式标错，否则 state 默认 valid →
            // 后面 ok=true → MarkCommitted 误认为入池成功（实际啥都没做）。
            if (!handler_cb) {
                state.Error("dispatcher validation handler not set");
            } else {
                try {
                    handler_cb(item, state);
                } catch (const std::exception& e) {
                    // v3.4.0 finding 2'' 修：handler 异常是节点内部错误不是 tx 永久 invalid。
                    // 用 Error() 让 RPC 客户端能区分"内部错误可重试"vs"tx 真无效"。
                    state.Error(std::string("handler exception: ") + e.what());
                } catch (...) {
                    state.Error("handler exception (unknown)");
                }
            }
            const bool ok = state.IsValid();
            const std::string err = state.GetRejectReason();  // 给 metric/log 用

            // sub-A4 review M1 (task #235)：失败时检测跨 worker 父子假 missing-inputs
            if (!ok && IsCrossWorkerMissingInputs(err)) {
                g_metrics.dispatcher_cross_worker_missing_total.fetch_add(
                    1, std::memory_order_relaxed);
            }
            // sub-A3 (task #154/#189) + Phase B (post-Teranode-audit)：
            //   普通 RPC race 失败：直接 reject 客户端（sub-A3 已实施）。
            //   reorg-resubmit race 失败：push 回 g_reorg_stash，让 drain 路径下次取出来
            //     重派给某个 worker（worker 拿到新 chainstate snap 跑就行）。
            //   不再起后台 thread + 指数退避 + retry_count — drain 周期天然就是延迟重试。
            //   消除：H1 retry_threads 无界增长 OOM 风险。
            if (!ok && item.is_reorg_resubmit && IsRaceRetryError(err)) {
                g_reorg_stash.Push(item.tx);
                LogPrint(BCLog::TXNVAL,
                         "v2.6.1 reorg-resubmit race fail: tx=%s -> ReorgStash (drain will retry)\n",
                         txid.ToString());
                MarkAborted(txid);
                item.SetResult(std::move(state));
                return;
            }

            // sub-A3：非 reorg race 失败立即 reject（不进 stash 后台重试）。
            //   prod 行为对齐：失败的 tx 该 reject 就 reject，不再背着客户端偷偷 commit。
            if (ok) MarkCommitted(txid);
            else    MarkAborted(txid);
            item.SetResult(std::move(state));
        };
        new_workers->emplace_back(
            std::make_unique<PerChainWorker>(wid, std::move(worker_handler)));
    }
    num_workers_count.store(num_workers, std::memory_order_release);
    // C3：原子发布 workers snapshot —— 所有读者后续 GetWorkers 看到完整 vector
    std::atomic_store_explicit(&workers_snapshot, new_workers,
                               std::memory_order_release);
    } catch (...) {
        // review v7 F-26：构造 worker 抛异常 → 停掉已起 gc/watchdog/drain + 已建 worker
        //   再 rethrow。new_workers 析构会调每个 unique_ptr<PerChainWorker> dtor → Stop。
        gc_running.store(false, std::memory_order_release);
        watchdog_running.store(false, std::memory_order_release);
        // H4 (post-Teranode-audit)：drain notify 也包进 lock_guard
        {
            std::lock_guard<std::mutex> lk(drain_cv_mtx);
            drain_running.store(false, std::memory_order_release);
            drain_cv.notify_all();
        }
        if (gc_thread.joinable()) gc_thread.join();
        if (watchdog_thread.joinable()) watchdog_thread.join();
        if (drain_thread.joinable()) drain_thread.join();
        new_workers.reset();  // 触发已建 worker 析构 → Stop join
        started.store(false, std::memory_order_release);
        throw;
    }
}

void ChainDispatcher::Stop() {
    std::lock_guard<std::mutex> mutate_lock(workers_mutate_mtx);
    bool expected = true;
    if (!started.compare_exchange_strong(expected, false,
                                         std::memory_order_acq_rel)) {
        return;
    }
    // C3：先抛 stopping 信号，新进来的 Submit* 会走 fallback 不再 push
    stopping.store(true, std::memory_order_release);
    // P2.6: 停 GC 线程
    gc_running.store(false, std::memory_order_release);
    if (gc_thread.joinable()) {
        gc_thread.join();
    }
    // P2.7: 停 Watchdog 线程
    watchdog_running.store(false, std::memory_order_release);
    if (watchdog_thread.joinable()) {
        watchdog_thread.join();
    }
    // H3: 停 Drain Stash 线程
    //   H4 (post-Teranode-audit)：drain_running.store + notify_all 必须在 lock_guard
    //   (drain_cv_mtx) 内，跟 signal_dispatcher / async_subscriber / per_chain_worker
    //   一致 — 防 lost-wakeup 让 drain_thread 错过信号多睡 500ms 抖动。
    {
        std::lock_guard<std::mutex> lk(drain_cv_mtx);
        drain_running.store(false, std::memory_order_release);
        drain_cv.notify_all();
    }
    if (drain_thread.joinable()) {
        drain_thread.join();
    }
    // Phase B (post-Teranode-audit)：retry_threads 后台机制已删除（reorg-resubmit
    //   race 失败改成直接 push 回 g_reorg_stash），不再有 retry thread 要 join。
    // C3：取出当前 workers snapshot 显式 Stop（让 worker 主循环 drain 退出），
    //     然后替换 snapshot 为空，原 vector 析构 unique_ptr 时调 Worker.Stop join。
    auto current = GetWorkers();
    if (current) {
        for (auto& w : *current) {
            if (w) w->Stop();
        }
    }
    std::atomic_store_explicit(&workers_snapshot,
                               std::shared_ptr<WorkersVec>{},
                               std::memory_order_release);
    num_workers_count.store(0, std::memory_order_release);
    // review v7 F-04/05/06：不再 handler_cb=nullptr — std::function 跨线程
    //   并发读写不是 thread-safe。worker thread 可能正在 handler_cb(item, err)，
    //   nullptr 写会 corrupt 内部 state。改靠 stopping flag + worker.Stop join 保证
    //   handler_cb 生命周期跟 ChainDispatcher 一致。
    // handler_cb 保留到 ~ChainDispatcher() 析构。
}

// P2.6 GC 主循环：每秒扫所有 shard，清：
//   - COMMITTED 项 5ms 之后（让同链子 tx 的 FindWorkerForChain 路由窗口闭合）
//   - ABORTED 项立即（commit_time_us 在 MarkAborted 时已设）
//   - RUNNING 项 30s 没动 → 视为 worker 卡死遗留，强清（防止泄漏）
void ChainDispatcher::RunGc() {
    while (gc_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(GC_INTERVAL_MS));
        if (!gc_running.load(std::memory_order_acquire)) break;

        const int64_t now = GetTimeMicros();
        uint64_t cleaned_c = 0, cleaned_a = 0, cleaned_lr = 0;

        for (Shard& shard : shards) {
            std::unique_lock lock(shard.mtx);
            for (auto it = shard.map.begin(); it != shard.map.end(); ) {
                const InflightEntry& e = it->second;
                bool drop = false;
                switch (e.status) {
                    case InflightStatus::COMMITTED:
                        if (e.commit_time_us > 0 &&
                            now - e.commit_time_us >= g_committed_gc_delay_us.load(std::memory_order_acquire)) {
                            drop = true;
                            cleaned_c++;
                        }
                        break;
                    case InflightStatus::ABORTED:
                        // sub-A4 review M2 (task #236)：ABORTED 项立即清理（无 5ms 延迟）。
                        //   COMMITTED 等 5ms 是为给同链 child tx 的 FindWorkerForChain
                        //   留路由窗口（让 child 跟 parent 路由到同 worker）；
                        //   ABORTED parent 不会被 child 引用（child 也会失败），
                        //   立即清避免 child 路由到已死 parent 的 worker。
                        drop = true;
                        cleaned_a++;
                        break;
                    case InflightStatus::RUNNING:
                    case InflightStatus::QUEUED:
                        // 卡死兜底：30s 没进展强清（worker 死锁 / handler 异常吞掉）
                        if (e.commit_time_us == 0) {
                            // 没有 timestamp，无法判定 — 跳过
                            break;
                        }
                        if (now - e.commit_time_us >= LONG_RUNNING_TIMEOUT_US) {
                            drop = true;
                            cleaned_lr++;
                        }
                        break;
                }
                if (drop) {
                    it = shard.map.erase(it);
                    // sub-A4 review M1 (task #235)：drop 时 inflight gauge 减 1
                    g_metrics.dispatcher_inflight_current.fetch_sub(
                        1, std::memory_order_release);
                } else {
                    ++it;
                }
            }
        }

        if (cleaned_c) gc_committed_cleaned.fetch_add(cleaned_c, std::memory_order_relaxed);
        if (cleaned_a) gc_aborted_cleaned.fetch_add(cleaned_a, std::memory_order_relaxed);
        if (cleaned_lr) {
            gc_long_running_cleaned.fetch_add(cleaned_lr, std::memory_order_relaxed);
            LogPrint(BCLog::TXNVAL,
                     "v2.6.1 GC: cleaned %llu long-running inflight (>%ds no progress)\n",
                     (unsigned long long)cleaned_lr,
                     (int)(LONG_RUNNING_TIMEOUT_US / 1'000'000));
        }

        // P2.4: GC reorg-only stash（race-stash 已删，sub-A3 task #154）
        g_reorg_stash.GC(1000);
    }
}

bool ChainDispatcher::SubmitSync(const CTransactionRef& tx, std::string& err,
                                 TxSource source,
                                 int64_t absurdFee,
                                 bool fLimitFree,
                                 int64_t accept_time) {
    if (!tx) { err = "null tx"; return false; }

    // C3：取 workers snapshot —— 之后无锁访问
    auto snap = GetWorkers();
    const bool can_dispatch = !stopping.load(std::memory_order_acquire) &&
                              started.load(std::memory_order_acquire) &&
                              snap && !snap->empty();
    if (!can_dispatch) {
        // 未 Start / 关停期 → fallback 同步调 handler
        if (handler_cb) {
            WorkItem fallback_item(tx);
            fallback_item.source = source;
            fallback_item.nAbsurdFee = absurdFee;
            fallback_item.fLimitFree = fLimitFree;
            fallback_item.accept_time = accept_time;
            // v3.4.0 finding 1 修：handler_cb 现在返 void 填 CValidationState&
            // v3.4.0 finding 2'' 修：handler 异常用 Error 表内部错误（非 tx invalid）
            CValidationState st;
            try { handler_cb(fallback_item, st); }
            catch (const std::exception& e) {
                st.Error(std::string("handler exception: ") + e.what());
            } catch (...) {
                st.Error("handler exception");
            }
            if (!st.IsValid()) err = st.GetRejectReason();
            return st.IsValid();
        }
        err = "dispatcher not started, no fallback handler";
        return false;
    }

    // 路由 worker：按父 chain 投票 → power-of-2
    WorkerId wid = RouteWorker(*tx);
    if (wid == WORKER_NONE || wid < 0 ||
        wid >= static_cast<WorkerId>(snap->size())) {
        wid = 0;
    }

    const TxId txid = tx->GetId();
    MarkQueued(txid, wid);

    // P5.1 真接入：构造 WorkItem 携带 promise，push 到 worker queue
    // sub-A12 (task #182)：promise 类型 pair<bool,string> → CValidationState
    auto promise = std::make_shared<std::promise<CValidationState>>();
    auto future = promise->get_future();

    LogPrint(BCLog::TXNVAL,
             "v2.6.1 SubmitSync: tx=%s -> worker[%d] qsize=%zu source=%d\n",
             txid.ToString(), (int)wid, (*snap)[wid]->QueueSize(), (int)source);

    WorkItem item(tx);
    item.source = source;
    item.nAbsurdFee = absurdFee;
    item.fLimitFree = fLimitFree;
    item.accept_time = accept_time;
    item.result_promise = promise;
    // v3.4.0 finding 2 修：Push 失败时清 inflight QUEUED 状态（防 30s GC 延迟里假父
    // 误导子交易 first-hit routing）。Push 内部已经 SetResult(false, "queue full"/"stopped")
    // 完了 promise，这里仅补 inflight cleanup。
    if (!(*snap)[wid]->Push(std::move(item))) {
        MarkAborted(txid);
    }

    // 阻塞等 worker 完成（无 budget — 跟 prod 同步 PTV 行为一致）。
    // worker 卡死要从 deadlock / 过载本身查；RPC 层 30s timeout 只让客户端拿到
    // 假 reject，worker 仍跑完导致客户端 / 节点状态不一致。
    // sub-A4 review L1 (task #237)：保持无 budget 跟 task #209 决策（reorg 期阻塞
    //   不强制 reject）一致。但 reorg 不在进行时若 wait > 30s 写 metric，让运维
    //   通过节点内部日志看到非 reorg 导致的 worker stall。
    // review v2 HIGH (task #239)：metric 必须写本次 30s 区间的 delta 而不是累积值。
    // review v5 HIGH：std::future wait_for 允许 spurious wakeup 提前返回 timeout，
    //   原代码每次 spurious wake 都进 metric 路径触发假 WARNING + 假 timeout 计数。
    //   修法：wait_for 返回 timeout 后用 GetTimeMicros 真测耗时 ≥30s 才进 metric 路径，
    //   spurious wake（实际 elapsed < 30s）跳过 + continue 继续等。
    constexpr int64_t WAIT_QUANTUM_US = kWaitQuantumUs;
    int64_t prev_us = GetTimeMicros();
    while (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
        const int64_t now_us = GetTimeMicros();
        const int64_t delta_us = now_us - prev_us;
        // M4 (post-Teranode-audit)：跟 SubmitSyncState 一致 — 显式防 clock 倒退
        //   导致 delta_us<0，cast uint64 后 wrap 到 ~2^64 永久污染 metric。
        if (delta_us < 0) { prev_us = now_us; continue; }
        // review v5 HIGH：spurious wakeup 防御 — 真测 elapsed < quantum 跳 metric
        if (delta_us < WAIT_QUANTUM_US) {
            continue;  // 不更新 prev_us，下一轮 elapsed 累加
        }
        prev_us = now_us;
        const bool reorg_in_progress = tbc::validation::ReorgInProgress();
        if (reorg_in_progress) {
            // reorg 期间预期 wait > 30s，记 delta 但继续等
            tbc::validation::g_metrics.reorg_blocked_us_total.fetch_add(
                static_cast<uint64_t>(delta_us), std::memory_order_relaxed);
        } else {
            // 非 reorg wait > 30s = worker stall，记 timeout 计数 + WARNING
            tbc::validation::g_metrics.reorg_blocked_timeout_total.fetch_add(
                1, std::memory_order_relaxed);
            // review v7 a5：rate-limit WARNING 每秒最多 1 条防 log 炸
            if (ShouldEmitStallWarning()) {
                LogPrintf("WARNING SubmitSync: tx=%s wait > 30s (no reorg), worker may be stalled\n",
                          txid.ToString());
            }
        }
    }
    CValidationState state = future.get();
    if (!state.IsValid()) {
        err = state.GetRejectReason();
        return false;
    }
    err.clear();
    return true;
}

bool ChainDispatcher::SubmitSync(const CTransactionRef& tx, std::string& err) {
    return SubmitSync(tx, err, TxSource::rpc, /*absurdFee*/0, /*fLimitFree*/false);
}

// sub-A12 (task #156)：直接返回 CValidationState 的 SubmitSync overload
// review v5 MEDIUM：原 future.wait() 无 stall 检测，wallet/RPC 路径走这个 overload
//   时遇到 worker stall 不会写 metric。复用 SubmitSync 的 30s metric 循环。
CValidationState ChainDispatcher::SubmitSyncState(const CTransactionRef& tx,
                                                  TxSource source,
                                                  int64_t absurdFee,
                                                  bool fLimitFree,
                                                  int64_t accept_time) {
    auto future = SubmitForFuture(tx, source, absurdFee, fLimitFree, accept_time);
    constexpr int64_t WAIT_QUANTUM_US = kWaitQuantumUs;
    int64_t prev_us = GetTimeMicros();
    while (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
        const int64_t now_us = GetTimeMicros();
        const int64_t delta_us = now_us - prev_us;
        // review v7 a3 CRITICAL：delta_us 负值（clock 倒退）会 wrap 到 ~2^64
        //   永久污染 metric。clamp 到 0 跳过这次记录。
        if (delta_us < 0) { prev_us = now_us; continue; }
        if (delta_us < WAIT_QUANTUM_US) continue;  // spurious wakeup 防御
        prev_us = now_us;
        if (tbc::validation::ReorgInProgress()) {
            tbc::validation::g_metrics.reorg_blocked_us_total.fetch_add(
                static_cast<uint64_t>(delta_us), std::memory_order_relaxed);
        } else {
            tbc::validation::g_metrics.reorg_blocked_timeout_total.fetch_add(
                1, std::memory_order_relaxed);
            if (ShouldEmitStallWarning()) {
                LogPrintf("WARNING SubmitSyncState: tx=%s wait > 30s (no reorg), "
                          "worker may be stalled\n",
                          tx ? tx->GetId().ToString() : "?");
            }
        }
    }
    return future.get();
}

// H5 / sub-A12: 返回 future 的 Submit — 调用方异步派发 N 笔后用整批 budget 等
std::shared_future<CValidationState>
ChainDispatcher::SubmitForFuture(const CTransactionRef& tx,
                                 TxSource source,
                                 int64_t absurdFee,
                                 bool fLimitFree,
                                 int64_t accept_time) {
    auto promise = std::make_shared<std::promise<CValidationState>>();
    auto future = promise->get_future().share();

    auto setInvalid = [&promise](const std::string& reason,
                                 unsigned int code = REJECT_INVALID) {
        CValidationState st;
        st.Invalid(false, code, reason);
        promise->set_value(std::move(st));
    };
    // v3.4.0 finding 2'' 修：内部错误（handler exception / dispatcher not started 等）
    // 用 Error 而非 Invalid，让 RPC 客户端能区分"内部错误可重试"vs"tx 永久 invalid"。
    auto setError = [&promise](const std::string& reason) {
        CValidationState st;
        st.Error(reason);
        promise->set_value(std::move(st));
    };

    if (!tx) {
        setInvalid("null tx");
        return future;
    }

    auto snap = GetWorkers();
    const bool can_dispatch = !stopping.load(std::memory_order_acquire) &&
                              started.load(std::memory_order_acquire) &&
                              snap && !snap->empty();
    if (!can_dispatch) {
        // fallback：直接同步调 handler 把结果写 promise
        if (handler_cb) {
            WorkItem fallback_item(tx);
            fallback_item.source = source;
            fallback_item.nAbsurdFee = absurdFee;
            fallback_item.fLimitFree = fLimitFree;
            fallback_item.accept_time = accept_time;
            // v3.4.0 finding 1 修：完整 CValidationState 透传
            CValidationState st;
            try {
                handler_cb(fallback_item, st);
                promise->set_value(std::move(st));
            } catch (const std::exception& e) {
                // v3.4.0 finding 2'' 修：内部异常用 Error
                setError(std::string("handler exception: ") + e.what());
            } catch (...) {
                setError("handler exception");
            }
        } else {
            // v3.4.0 finding 2'' 修：no-handler 是节点内部状态不是 tx 问题
            setError("dispatcher not started, no fallback");
        }
        return future;
    }

    WorkerId wid = RouteWorker(*tx);
    if (wid == WORKER_NONE || wid < 0 ||
        wid >= static_cast<WorkerId>(snap->size())) {
        wid = 0;
    }
    const TxId txid = tx->GetId();
    MarkQueued(txid, wid);

    LogPrint(BCLog::TXNVAL,
             "v2.6.1 SubmitForFuture: tx=%s -> worker[%d] qsize=%zu\n",
             txid.ToString(), (int)wid, (*snap)[wid]->QueueSize());

    WorkItem item(tx);
    item.source = source;
    item.nAbsurdFee = absurdFee;
    item.fLimitFree = fLimitFree;
    item.accept_time = accept_time;
    item.result_promise = promise;
    // v3.4.0 finding 2 修：Push 失败清 inflight QUEUED
    if (!(*snap)[wid]->Push(std::move(item))) {
        MarkAborted(txid);
    }
    return future;
}

void ChainDispatcher::SubmitAsync(const CTransactionRef& tx,
                                  TxSource source,
                                  int64_t absurdFee,
                                  bool fLimitFree,
                                  int64_t accept_time) {
    if (!tx) return;

    auto snap = GetWorkers();
    const bool can_dispatch = !stopping.load(std::memory_order_acquire) &&
                              started.load(std::memory_order_acquire) &&
                              snap && !snap->empty();
    if (!can_dispatch) {
        if (handler_cb) {
            WorkItem fallback_item(tx);
            fallback_item.source = source;
            fallback_item.nAbsurdFee = absurdFee;
            fallback_item.fLimitFree = fLimitFree;
            fallback_item.accept_time = accept_time;
            // v3.4.0 finding 1 修：fire-and-forget 路径不需结果，但仍要正确 handler 签名
            CValidationState st;
            try { handler_cb(fallback_item, st); } catch (...) {}
        }
        return;
    }

    WorkerId wid = RouteWorker(*tx);
    if (wid == WORKER_NONE || wid < 0 ||
        wid >= static_cast<WorkerId>(snap->size())) {
        wid = 0;
    }
    const TxId txid = tx->GetId();
    MarkQueued(txid, wid);

    LogPrint(BCLog::TXNVAL,
             "v2.6.1 SubmitAsync: tx=%s -> worker[%d] qsize=%zu source=%d\n",
             txid.ToString(), (int)wid, (*snap)[wid]->QueueSize(), (int)source);

    WorkItem item(tx);
    item.source = source;
    item.nAbsurdFee = absurdFee;
    item.fLimitFree = fLimitFree;
    item.accept_time = accept_time;
    // v3.4.0 finding 2 修：fire-and-forget 路径 Push 失败也要清 inflight QUEUED
    if (!(*snap)[wid]->Push(std::move(item))) {
        MarkAborted(txid);
    }
}

void ChainDispatcher::SubmitAsync(const CTransactionRef& tx) {
    SubmitAsync(tx, TxSource::p2p);
}

void ChainDispatcher::SubmitAsyncP2P(const CTransactionRef& tx,
                                     CNodePtr pfrom,
                                     int64_t accept_time,
                                     bool fLimitFree,
                                     int64_t nAbsurdFee) {
    if (!tx) return;

    auto snap = GetWorkers();
    const bool can_dispatch = !stopping.load(std::memory_order_acquire) &&
                              started.load(std::memory_order_acquire) &&
                              snap && !snap->empty();
    if (!can_dispatch) {
        if (handler_cb) {
            WorkItem fallback_item(tx);
            fallback_item.source = TxSource::p2p;
            fallback_item.pfrom = std::move(pfrom);
            fallback_item.accept_time = accept_time;
            fallback_item.fLimitFree = fLimitFree;
            fallback_item.nAbsurdFee = nAbsurdFee;
            // v3.4.0 finding 1 修：fire-and-forget 路径不需结果，但仍要正确 handler 签名
            CValidationState st;
            try { handler_cb(fallback_item, st); } catch (...) {}
        }
        return;
    }

    WorkerId wid = RouteWorker(*tx);
    if (wid == WORKER_NONE || wid < 0 ||
        wid >= static_cast<WorkerId>(snap->size())) {
        wid = 0;
    }
    const TxId txid = tx->GetId();
    MarkQueued(txid, wid);

    LogPrint(BCLog::TXNVAL,
             "v2.6.1 SubmitAsyncP2P: tx=%s -> worker[%d] qsize=%zu peer=%d\n",
             txid.ToString(), (int)wid, (*snap)[wid]->QueueSize(),
             pfrom ? pfrom->GetId() : -1);

    WorkItem item(tx);
    item.source = TxSource::p2p;
    item.pfrom = std::move(pfrom);
    item.accept_time = accept_time;
    item.fLimitFree = fLimitFree;
    item.nAbsurdFee = nAbsurdFee;
    // v3.4.0 finding 2 修：P2P fire-and-forget Push 失败清 inflight QUEUED
    if (!(*snap)[wid]->Push(std::move(item))) {
        MarkAborted(txid);
    }
}

// P2.7 Watchdog 主循环：每 5s 扫所有 worker.last_progress_us，
//                       超 60s 没进度记一行告警 + 增计数器（不重启 worker，避免误杀）
void ChainDispatcher::RunWatchdog() {
    while (watchdog_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(WATCHDOG_INTERVAL_MS));
        if (!watchdog_running.load(std::memory_order_acquire)) break;
        if (!started.load(std::memory_order_acquire)) continue;

        const int64_t now = GetTimeMicros();
        auto snap = GetWorkers();
        if (!snap) continue;
        for (size_t i = 0; i < snap->size(); i++) {
            const auto& w = (*snap)[i];
            if (!w) continue;
            int64_t last = w->LastProgressUs();
            if (last == 0) continue;
            int64_t idle_us = now - last;
            // queue 为空时 idle 不算 stall（worker 在 cv.wait）
            if (w->QueueSize() == 0) continue;
            if (idle_us >= WORKER_STALL_THRESHOLD_US) {
                watchdog_stall_alerts.fetch_add(1, std::memory_order_relaxed);
                LogPrintf("v2.6.1 WATCHDOG: worker[%zu] stalled %llds with queue=%zu items\n",
                          i, (long long)(idle_us / 1'000'000),
                          w->QueueSize());
            }
        }
    }
}

ChainDispatcher::ChainDispatcher() = default;

// H2: handler 提前注册 — 启动早期（dispatcher 没 Start 但 PTV 已就绪）
//     fallback 路径直接调 handler 走 PTV，不报 "no fallback handler"。
void ChainDispatcher::SetFallbackHandler(ValidationHandler handler) {
    std::lock_guard<std::mutex> lock(workers_mutate_mtx);
    if (!handler_cb) {
        handler_cb = std::move(handler);
    }
}

ChainDispatcher::~ChainDispatcher() {
    Stop();   // 幂等 — Start 没调过则直接返回
}

// H3: Drain Stash 后台线程主循环
//   - 每 500ms 醒一次，或被 NotifyDrainStash 唤醒（UpdateTip 调）
//   - 每轮 drain 100 笔 ReorgStash → SubmitAsync
//   - Phase B (post-Teranode-audit)：RaceStash 已删，仅 reorg 路径
//   - 不持 cs_main，无 hot-path 风险
void ChainDispatcher::RunDrainStash() {
    uint32_t last_seen_seq = 0;
    while (drain_running.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lock(drain_cv_mtx);
            drain_cv.wait_for(lock, std::chrono::milliseconds(500), [&] {
                return !drain_running.load(std::memory_order_acquire) ||
                       drain_signal_seq.load(std::memory_order_acquire) != last_seen_seq;
            });
            last_seen_seq = drain_signal_seq.load(std::memory_order_acquire);
        }
        if (!drain_running.load(std::memory_order_acquire)) break;
        if (!started.load(std::memory_order_acquire)) continue;

        // sub-A3 (task #154)：race_stash 已删，drain 仅处理 reorg-only resubmit
        size_t reorg_count = 0;
        auto reorg_batch = g_reorg_stash.Drain(100);
        for (auto& tx : reorg_batch) {
            if (!tx) continue;
            // review v7 a4 F-7：reorg drain 路径 fLimitFree=true 跟 UpdateMempoolForReorg 对齐
            SubmitAsync(tx, TxSource::reorg, /*absurdFee*/0, /*fLimitFree*/true);
            reorg_count++;
        }
        if (reorg_count > 0) {
            LogPrint(BCLog::TXNVAL,
                     "v2.6.1 drain: reorg=%zu\n", reorg_count);
        }
    }
}

// 全局实例
ChainDispatcher g_dispatcher;

// COMMITTED GC 延迟，init.cpp 通过 -dispatchercommitgcms 设置（默认 5ms）
std::atomic<int64_t> g_committed_gc_delay_us{COMMITTED_GC_DELAY_US_DEFAULT};

} // namespace validation
} // namespace tbc
