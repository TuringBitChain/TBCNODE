// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P3.6 / F9 / N4 GbtSnapshotProvider — 单 refresh worker + 合并队列
//
// 设计：
//   - ConnectBlock 释放 smtx 后调 RefreshAsync()，仅设 pending atomic 标志（不阻塞）
//   - 单 refresh worker 主循环 wait pending → 拍 mempool snapshot → 写 last_stable
//   - 连续 N 次 RefreshAsync 合并为 1 次实际拷贝（不爆炸线程）
//   - WaitFresh condvar 长轮询最长 timeout（避免 100ms 重试风暴）
//   - N4 修复：tip_hash 走 chainstate seqlock Capture，不持 cs_main → 不破坏锁层级

#ifndef BITCOIN_MINING_GBT_SNAPSHOT_H
#define BITCOIN_MINING_GBT_SNAPSHOT_H

#include "primitives/transaction.h"
#include "uint256.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

namespace tbc {
namespace mining {

struct MempoolSnapshot {
    uint256 tip_hash;
    int32_t height = 0;
    std::vector<CTransactionRef> txs;
};

class GbtSnapshotProvider {
public:
    using FreshSnapBuilder = std::function<std::shared_ptr<MempoolSnapshot>()>;

    explicit GbtSnapshotProvider(FreshSnapBuilder builder)
        : build_fresh(std::move(builder)) {}

    void Start() {
        running.store(true, std::memory_order_release);
        worker = std::thread([this] { RefreshLoop(); });
    }

    void Stop() {
        running.store(false, std::memory_order_release);
        refresh_trigger_cv.notify_all();
        if (worker.joinable()) worker.join();
    }

    ~GbtSnapshotProvider() { Stop(); }

    // ConnectBlock 释放 smtx 后调，仅设 pending（合并多次触发为 1 次拷贝）
    void RefreshAsync() noexcept {
        pending.store(true, std::memory_order_release);
        refresh_trigger_cv.notify_one();
    }

    // condvar 长轮询：最长 timeout，等到 last_stable.tip_hash == expected_tip 才返回
    std::shared_ptr<const MempoolSnapshot>
    WaitFresh(const uint256& expected_tip, std::chrono::milliseconds timeout) {
        std::unique_lock l(snap_mtx);
        if (refresh_cv.wait_for(l, timeout, [&] {
                return last_stable && last_stable->tip_hash == expected_tip;
            })) {
            return last_stable;
        }
        return last_stable;   // 超时返回 last_stable（可能 stale）
    }

    GbtSnapshotProvider(const GbtSnapshotProvider&) = delete;
    GbtSnapshotProvider& operator=(const GbtSnapshotProvider&) = delete;

private:
    void RefreshLoop() {
        while (running.load(std::memory_order_acquire)) {
            {
                std::unique_lock l(refresh_mtx);
                refresh_trigger_cv.wait(l, [this] {
                    return pending.load(std::memory_order_acquire) ||
                           !running.load(std::memory_order_acquire);
                });
                if (!running.load(std::memory_order_acquire)) return;
                pending.store(false, std::memory_order_release);
            }

            std::shared_ptr<MempoolSnapshot> fresh;
            try { fresh = build_fresh(); }
            catch (...) { fresh.reset(); }

            if (fresh) {
                {
                    std::unique_lock l(snap_mtx);
                    last_stable = fresh;
                }
                refresh_cv.notify_all();
            }
        }
    }

    FreshSnapBuilder build_fresh;

    mutable std::shared_mutex snap_mtx;
    std::shared_ptr<const MempoolSnapshot> last_stable;
    std::condition_variable_any refresh_cv;

    std::mutex refresh_mtx;
    std::condition_variable refresh_trigger_cv;
    std::atomic<bool> pending{false};
    std::atomic<bool> running{false};
    std::thread worker;
};

} // namespace mining
} // namespace tbc

#endif // BITCOIN_MINING_GBT_SNAPSHOT_H
