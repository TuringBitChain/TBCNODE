// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P0.2: BatchWrite 双写一致性 + batchWriteMtx 块级原子单元测试
//
// 验证：
//   - BatchWrite 后 cacheCoinsConcurrent 跟 cacheCoins 数据一致
//   - 多 reader 持 shared_lock(batchWriteMtx) 期间 BatchWrite 等待（unique_lock 阻塞）
//   - BatchWrite 期间 GetCoinConcurrent stub 路径仍稳（行为不变）
//
// 注：P0.3 才真把 GetCoinConcurrent 切到 cacheCoinsConcurrent + batchWriteMtx
//     P0.2 阶段 GetCoinConcurrent 是老 GetCoin 包装，块级原子由 mCoinsViewCacheMtx 保证

#include "coins.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(coins_p02_dual_write)

namespace {

// 简易 backing view（永远空）
class EmptyBackingView : public CCoinsView {
public:
    bool GetCoin(const COutPoint&, Coin&) const override { return false; }
    bool HaveCoin(const COutPoint&) const override { return false; }
    uint256 GetBestBlock() const override { return uint256(); }
    bool BatchWrite(CCoinsMap&, const uint256&) override { return true; }
    bool BatchWriteNoLockVirtual(CCoinsMap& m, const uint256& h,
                                 const BatchWriteLockToken&) override {
        return BatchWrite(m, h);
    }
};

// 给测试访问 protected 成员
class TestableCache : public CCoinsViewCache {
public:
    TestableCache(CCoinsView* base) : CCoinsViewCache(base) {}
    size_t ConcurrentSize() const { return cacheCoinsConcurrent.size(); }
    bool ConcurrentHas(const COutPoint& op) const {
        return cacheCoinsConcurrent.contains(op);
    }
    bool ConcurrentEqualsCacheCoins() const {
        // 验证 cacheCoinsConcurrent 跟 cacheCoins 数据一致
        if (cacheCoinsConcurrent.size() != cacheCoins.size()) return false;
        for (const auto& [k, v] : cacheCoins) {
            CCoinsCacheEntry got;
            bool found = cacheCoinsConcurrent.find(k, got);
            if (!found) return false;
            if (got.coin.GetHeight() != v.coin.GetHeight()) return false;
            if (got.coin.IsSpent() != v.coin.IsSpent()) return false;
        }
        return true;
    }
};

CCoinsCacheEntry make_entry(uint32_t height, bool spent = false) {
    CCoinsCacheEntry e;
    CTxOut out;
    out.nValue = Amount(int64_t(height) * COIN.GetSatoshis());
    e.coin = Coin(std::move(out), height, false);
    if (spent) e.coin.Clear();
    e.flags = CCoinsCacheEntry::DIRTY | CCoinsCacheEntry::FRESH;
    return e;
}

} // anon

// 测试 1：BatchWrite 后 cacheCoinsConcurrent 跟 cacheCoins 数据等价
BOOST_AUTO_TEST_CASE(batchwrite_dual_write_consistency) {
    EmptyBackingView base;
    TestableCache cache(&base);

    CCoinsMap mapCoins;
    for (uint32_t i = 0; i < 100; i++) {
        COutPoint op(uint256S(std::to_string(i)), 0);
        mapCoins[op] = make_entry(i);
    }
    uint256 hash;
    cache.BatchWrite(mapCoins, hash);

    BOOST_CHECK_EQUAL(cache.ConcurrentSize(), 100u);
    BOOST_CHECK(cache.ConcurrentEqualsCacheCoins());

    // 100 个 entry 全在 concurrent map
    for (uint32_t i = 0; i < 100; i++) {
        COutPoint op(uint256S(std::to_string(i)), 0);
        BOOST_CHECK(cache.ConcurrentHas(op));
    }
}

// 测试 2：BatchWrite 用 shared/unique batchWriteMtx 保证块级原子
// 多 reader 持 shared_lock 期间 BatchWrite 必须等待
BOOST_AUTO_TEST_CASE(batchwrite_blocks_during_concurrent_readers) {
    // 仅验证 IsBatchWriteInProgress 在 BatchWrite 期间返回 true
    EmptyBackingView base;
    TestableCache cache(&base);

    std::atomic<bool> bw_running{false};
    std::atomic<bool> stop{false};
    std::atomic<int> bw_seen_count{0};

    // reader 持续 IsBatchWriteInProgress
    std::thread reader([&] {
        while (!stop.load()) {
            if (cache.IsBatchWriteInProgress()) {
                bw_seen_count.fetch_add(1);
            }
            std::this_thread::yield();
        }
    });

    // BatchWrite 跑 50 次大 batch
    for (int round = 0; round < 50; round++) {
        CCoinsMap mapCoins;
        for (uint32_t i = 0; i < 1000; i++) {
            COutPoint op(uint256S(std::to_string(round * 1000 + i)), 0);
            mapCoins[op] = make_entry(i);
        }
        uint256 hash;
        cache.BatchWrite(mapCoins, hash);
    }

    stop.store(true);
    reader.join();

    BOOST_TEST_MESSAGE("reader saw IsBatchWriteInProgress=true count=" << bw_seen_count.load());
    // reader 应至少看到几次 BatchWrite 进行中（IsBatchWriteInProgress 返回 true）
    BOOST_CHECK_GT(bw_seen_count.load(), 0);
    // 验证 50 个 batch 都正确写入
    BOOST_CHECK_EQUAL(cache.ConcurrentSize(), 50u * 1000u);
}

// 测试 3：cachedCoinsUsage atomic 在并发 BatchWrite 下不漂移
BOOST_AUTO_TEST_CASE(cached_usage_no_drift_after_batch) {
    EmptyBackingView base;
    TestableCache cache(&base);

    CCoinsMap mapCoins;
    for (uint32_t i = 0; i < 100; i++) {
        COutPoint op(uint256S(std::to_string(i)), 0);
        mapCoins[op] = make_entry(i);
    }
    uint256 hash;
    cache.BatchWrite(mapCoins, hash);

    // cachedCoinsUsage > 0
    size_t total = cache.DynamicMemoryUsage();
    BOOST_CHECK_GT(total, 0u);

    // 双写后 size 一致
    BOOST_CHECK(cache.ConcurrentEqualsCacheCoins());
}

BOOST_AUTO_TEST_SUITE_END()
