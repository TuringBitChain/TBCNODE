// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.

#include "validation/per_chain_worker.h"

#include "logging.h"    // LogPrintf
#include "utiltime.h"   // GetTimeMicros
#include "validation/metrics.h"  // Phase pre-J: g_metrics

#include <utility>

namespace tbc {
namespace validation {

PerChainWorker::PerChainWorker(WorkerId id_, ItemHandler handler_, size_t capacity)
    : id(id_), handler(std::move(handler_)), capacity_(capacity) {
    last_progress_us.store(GetTimeMicros(), std::memory_order_relaxed);
    thread = std::thread([this] { Run(); });
}

PerChainWorker::~PerChainWorker() {
    Stop();
}

bool PerChainWorker::Push(WorkItem&& item) {
    {
        std::lock_guard lock(queue_mtx);
        // review v7 F-14：Stop 后 Push 必须立即拒，否则新 item 进 queue 但 worker
        //   thread 已 join 退出 → caller future.wait 永挂。
        if (!running.load(std::memory_order_acquire)) {
            // v3.4.0 finding 1' 修：节点临时状态 (worker stopped) 不是 tx 永久 invalid。
            // 用 CValidationState::Error 让 RPC 客户端区分"内部错误"vs"tx 真无效"
            // — 客户端可以重发，不要把这种当成 reject_code=REJECT_INVALID 永久失败。
            CValidationState st;
            st.Error("worker stopped");
            item.SetResult(std::move(st));
            return false;
        }
        if (queue.size() >= capacity_) {
            // task #157：满 queue 不阻塞 dispatcher。
            //   有 promise（SubmitSync 来源）：写失败结果让 caller 立即返回。
            //   无 promise（fire-and-forget）：直接 drop。
            //   两路都计 metric。
            g_metrics.worker_queue_push_timeout_total.fetch_add(
                1, std::memory_order_relaxed);
            // 锁内调 SetResult 没问题：set_value 不取 user mutex。
            // v3.4.0 finding 1' 修：queue 拥塞不是 tx 永久 invalid，用 Error 表临时拥塞。
            CValidationState st;
            st.Error("worker queue full");
            item.SetResult(std::move(st));
            return false;
        }
        queue.push_back(std::move(item));
    }
    cv.notify_one();
    // review v5 MEDIUM：current 用 delta（+1）防多 worker last-writer-wins。
    // review v6 MEDIUM：Delta 内部按 aggregate after 维护 max，无需再调 RecordQueueDepth。
    g_metrics.RecordQueueDepthDelta(+1);
    return true;
}

size_t PerChainWorker::QueueSize() const {
    std::lock_guard lock(queue_mtx);
    return queue.size();
}

void PerChainWorker::Stop() {
    std::deque<WorkItem> drained;
    {
        std::lock_guard lock(queue_mtx);
        running.store(false, std::memory_order_release);
        // task #157：shutdown 时 drain queue，防 SubmitSync caller 永挂在
        //   future.wait_for(30s) — 把所有未消费 item 的 promise 设失败结果。
        //   注意：worker thread 可能还在跑当前 item 的 handler（不在 queue 里），
        //   那个 item 走 handler 内部的 SetResult / 异常 catch 路径。
        drained.swap(queue);
        // review v7 F-12：notify_all 必须在 lock 内 — 防 worker 在 lock 释放跟
        //   notify 之间从某次 Push 醒来重判 predicate (running=false, queue=空) →
        //   立即返回 → 之后 Push 可能又入队（F-14 漏修），新 thread 永不消费。
        //   锁内 notify 让 worker 醒来时 lock 已释放，predicate 一致看到 stopped。
        cv.notify_all();
    }
    if (thread.joinable()) {
        thread.join();
    }
    if (!drained.empty()) {
        const uint64_t n = static_cast<uint64_t>(drained.size());
        g_metrics.worker_stop_promise_drained_total.fetch_add(
            n, std::memory_order_relaxed);
        // review v5 MEDIUM：drain 出去的项也要从 current gauge 减掉。
        // review v7 F-22：thread.join 已退出 + F-14 后续 Push 都拒，无并发 Push
        //   再 Delta(+1)，此处 Delta(-n) 不会跟 push 竞争。CAS-loop saturate 在
        //   metrics.h 已防 underflow。
        g_metrics.RecordQueueDepthDelta(-static_cast<int64_t>(n));
        for (auto& it : drained) {
            // v3.4.0 finding 1' 修：节点关闭不是 tx 无效，用 Error 而非 REJECT_INVALID。
            CValidationState st;
            st.Error("node shutting down");
            it.SetResult(std::move(st));
        }
        LogPrintf("v2.6.1 worker[%d]: drained %u items on Stop\n",
                  id, static_cast<unsigned>(n));
    }
}

void PerChainWorker::Run() {
    while (true) {
        WorkItem item;
        {
            std::unique_lock lock(queue_mtx);
            cv.wait(lock, [this] {
                return !queue.empty() || !running.load(std::memory_order_acquire);
            });

            if (!running.load(std::memory_order_acquire) && queue.empty()) {
                return;
            }
            if (queue.empty()) continue;

            item = std::move(queue.front());
            queue.pop_front();
            // Phase pre-J：pop 之后更新 depth gauge
            // review v5 MEDIUM：current 用 delta（-1）；max 不变（pop 不会让 max 升）
            g_metrics.RecordQueueDepthDelta(-1);
        }

        last_progress_us.store(GetTimeMicros(), std::memory_order_relaxed);

        // 跑 handler（P3.3 真接入 AcceptToMemoryPoolWorker）
        // 异常容忍：handler 抛异常不能让 worker thread 死掉。
        // H4 修补：catch 内必须调 item.SetResult 防 broken_promise 让 SubmitSync 调用方
        //         挂在 future.wait_for 上 30s。注意 handler 可能已经把 item move 走了，
        //         此处保留对 promise 的引用副本：进 try 之前 stash promise，
        //         catch 后用 stash 调 set_value。
        auto stashed_promise = item.result_promise;  // 保留 promise 引用副本（shared_ptr）
        try {
            handler(std::move(item));
        } catch (const std::exception& e) {
            LogPrintf("v2.6.1 worker[%d] handler exception: %s\n", id, e.what());
            if (stashed_promise) {
                try {
                    CValidationState st;
                    st.Error(std::string("worker exception: ") + e.what());
                    stashed_promise->set_value(std::move(st));
                } catch (const std::future_error&) {
                    // promise 已被 set 过 — 忽略
                }
            }
        } catch (...) {
            LogPrintf("v2.6.1 worker[%d] handler exception (unknown)\n", id);
            if (stashed_promise) {
                try {
                    CValidationState st;
                    st.Error("worker exception (unknown)");
                    stashed_promise->set_value(std::move(st));
                } catch (const std::future_error&) {}
            }
        }
    }
}

} // namespace validation
} // namespace tbc
