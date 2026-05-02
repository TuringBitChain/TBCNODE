// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P2.4 (F6 + N7 + H-E): TxStash 模板 — reorg 兜底队列
//
// Phase B (post-Teranode-audit)：删除 RaceStash + ResubmitRateLimiter
//   （sub-A3 race-retry 后台 thread 路径删除后无 caller）。
//
// 唯一 stash 池：
//   - ReorgStash：DisconnectTip 回放 + reorg-resubmit race 失败 push 回，drain 路径周期重派 → 容量 200k / 10 分钟 TTL
//
// 所有方法（Push/Drain/GC/Size）持单一 stash mtx，互斥串行。
// Drain 是消费式（取出即删），不存在跟 GC 同时操作同一 entry 的窗口。
// 外部调用方持其它锁的时机：必须先释放外部锁再调 Stash 方法。
// stash mtx 是叶子锁，不参与 lock-hierarchy 任何层级。
//
// metrics：监控持续溢出（push_total / drop_full / drop_ttl / drain_total）

#ifndef BITCOIN_VALIDATION_TX_STASH_H
#define BITCOIN_VALIDATION_TX_STASH_H

#include "primitives/transaction.h"   // CTransactionRef
#include "utiltime.h"                 // GetTimeMicros

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace tbc {
namespace validation {

template<size_t MAX_SIZE_, int64_t TTL_US_>
class TxStash {
public:
    static constexpr size_t MAX_SIZE = MAX_SIZE_;
    static constexpr int64_t TTL_US = TTL_US_;

    struct Metrics {
        uint64_t push_total;
        uint64_t drop_full;
        uint64_t drop_ttl;
        uint64_t drain_total;
        uint64_t current_size;
    };

    void Push(CTransactionRef tx) {
        std::lock_guard lock(mtx);
        if (stash.size() >= MAX_SIZE) {
            stash.pop_front();
            drop_full.fetch_add(1, std::memory_order_relaxed);
        }
        stash.emplace_back(GetTimeMicros(), std::move(tx));
        push_total.fetch_add(1, std::memory_order_relaxed);
    }

    // 取出 max_count 条；按 FIFO；取出即删
    std::vector<CTransactionRef> Drain(size_t max_count = 1000) {
        std::vector<CTransactionRef> out;
        out.reserve(max_count);
        std::lock_guard lock(mtx);
        while (!stash.empty() && out.size() < max_count) {
            out.push_back(std::move(stash.front().second));
            stash.pop_front();
        }
        drain_total.fetch_add(out.size(), std::memory_order_relaxed);
        return out;
    }

    // 后台 GC：清超过 TTL 的项；budget 限制单次扫描数防长持锁
    void GC(size_t budget = 1000) {
        int64_t now = GetTimeMicros();
        std::lock_guard lock(mtx);
        while (!stash.empty() && budget > 0) {
            if (now - stash.front().first <= TTL_US) break;
            stash.pop_front();
            drop_ttl.fetch_add(1, std::memory_order_relaxed);
            --budget;
        }
    }

    size_t Size() const {
        std::lock_guard lock(mtx);
        return stash.size();
    }

    Metrics GetMetrics() const {
        return {
            push_total.load(std::memory_order_relaxed),
            drop_full.load(std::memory_order_relaxed),
            drop_ttl.load(std::memory_order_relaxed),
            drain_total.load(std::memory_order_relaxed),
            Size()
        };
    }

private:
    mutable std::mutex mtx;   // 叶子锁
    std::deque<std::pair<int64_t, CTransactionRef>> stash;   // (timestamp_us, tx)

    std::atomic<uint64_t> push_total{0};
    std::atomic<uint64_t> drop_full{0};
    std::atomic<uint64_t> drop_ttl{0};
    std::atomic<uint64_t> drain_total{0};
};

// F6: reorg 兜底队列
using ReorgStash = TxStash<200000, 10 * 60 * 1000000LL>;   // 10 分钟 TTL

extern ReorgStash g_reorg_stash;

} // namespace validation
} // namespace tbc

#endif // BITCOIN_VALIDATION_TX_STASH_H
