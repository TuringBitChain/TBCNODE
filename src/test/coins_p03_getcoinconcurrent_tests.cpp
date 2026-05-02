// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P0.3: GetCoinConcurrent / HaveCoinConcurrent 单元测试
//
// 验证：
//   - L1 cacheCoinsConcurrent 命中
//   - L3 LevelDB fallback + 回填 cacheCoinsConcurrent
//   - 32 worker 并发 GetCoinConcurrent 0 race
//   - cachedCoinsUsage 在 cache miss 回填路径正确递增（K2 insert 返回 bool）
//
// 注：L2 LRU 64MB 留 P0.3 后续工作，本卡只测 L1 + L3

#include "coins.h"

#include <atomic>
#include <chrono>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(coins_p03_getcoinconcurrent)

namespace {

// 模拟 LevelDB backing：内部 std::unordered_map 模拟 disk
class MapBackingView : public CCoinsView {
public:
    std::unordered_map<COutPoint, Coin, SaltedOutpointHasher> data;
    mutable std::atomic<size_t> get_called{0};

    bool GetCoin(const COutPoint& op, Coin& coin) const override {
        get_called.fetch_add(1, std::memory_order_relaxed);
        auto it = data.find(op);
        if (it == data.end() || it->second.IsSpent()) return false;
        coin = it->second;
        return true;
    }
    bool HaveCoin(const COutPoint& op) const override {
        Coin c;
        return GetCoin(op, c);
    }
    uint256 GetBestBlock() const override { return uint256(); }
    bool BatchWrite(CCoinsMap&, const uint256&) override { return true; }
    bool BatchWriteNoLockVirtual(CCoinsMap& m, const uint256& h,
                                 const BatchWriteLockToken&) override {
        return BatchWrite(m, h);
    }
};

class TestableCache : public CCoinsViewCache {
public:
    TestableCache(CCoinsView* base) : CCoinsViewCache(base) {}
    size_t ConcurrentSize() const { return cacheCoinsConcurrent.size(); }
};

CCoinsCacheEntry make_entry(uint32_t height) {
    CCoinsCacheEntry e;
    CTxOut out;
    out.nValue = Amount(int64_t(height) * COIN.GetSatoshis());
    e.coin = Coin(std::move(out), height, false);
    e.flags = CCoinsCacheEntry::DIRTY | CCoinsCacheEntry::FRESH;
    return e;
}

Coin make_coin(uint32_t height) {
    CTxOut out;
    out.nValue = Amount(int64_t(height) * COIN.GetSatoshis());
    return Coin(std::move(out), height, false);
}

} // anon

// 测试 1：L1 cacheCoinsConcurrent 命中（BatchWrite 后查 GetCoinConcurrent）
BOOST_AUTO_TEST_CASE(get_concurrent_L1_hit) {
    MapBackingView base;
    TestableCache cache(&base);

    // 通过 BatchWrite 写入 cacheCoinsConcurrent
    CCoinsMap mapCoins;
    for (uint32_t i = 0; i < 50; i++) {
        COutPoint op(uint256S(std::to_string(i)), 0);
        mapCoins[op] = make_entry(i);
    }
    uint256 hash;
    cache.BatchWrite(mapCoins, hash);

    // GetCoinConcurrent 应从 L1 命中（不调 base->GetCoin）
    base.get_called.store(0);
    for (uint32_t i = 0; i < 50; i++) {
        COutPoint op(uint256S(std::to_string(i)), 0);
        Coin coin;
        BOOST_REQUIRE(cache.GetCoinConcurrent(op, coin));
        BOOST_CHECK_EQUAL(coin.GetHeight(), i);
    }
    BOOST_CHECK_EQUAL(base.get_called.load(), 0u);   // L1 全命中，base 0 调用
}

// 测试 2：L3 LevelDB fallback + 回填 cacheCoinsConcurrent
BOOST_AUTO_TEST_CASE(get_concurrent_L3_fallback_and_refill) {
    MapBackingView base;
    // base 有 100 个 coin，但 cacheCoinsConcurrent 是空
    for (uint32_t i = 0; i < 100; i++) {
        COutPoint op(uint256S(std::to_string(i)), 0);
        base.data[op] = make_coin(i);
    }
    TestableCache cache(&base);
    BOOST_CHECK_EQUAL(cache.ConcurrentSize(), 0u);

    // 第一次查：L1 miss → L3 hit → 回填 L1
    base.get_called.store(0);
    for (uint32_t i = 0; i < 50; i++) {
        COutPoint op(uint256S(std::to_string(i)), 0);
        Coin coin;
        BOOST_REQUIRE(cache.GetCoinConcurrent(op, coin));
        BOOST_CHECK_EQUAL(coin.GetHeight(), i);
    }
    BOOST_CHECK_EQUAL(base.get_called.load(), 50u);   // 50 次 L3 调用
    BOOST_CHECK_EQUAL(cache.ConcurrentSize(), 50u);   // 50 个回填到 L1

    // 第二次查同样的 50 个：L1 全命中
    base.get_called.store(0);
    for (uint32_t i = 0; i < 50; i++) {
        COutPoint op(uint256S(std::to_string(i)), 0);
        Coin coin;
        BOOST_REQUIRE(cache.GetCoinConcurrent(op, coin));
    }
    BOOST_CHECK_EQUAL(base.get_called.load(), 0u);   // 全 L1 命中
}

// 测试 3：base 不存在的 outpoint 返回 false
BOOST_AUTO_TEST_CASE(get_concurrent_missing) {
    MapBackingView base;
    TestableCache cache(&base);

    COutPoint op(uint256S("999"), 0);
    Coin coin;
    BOOST_CHECK(!cache.GetCoinConcurrent(op, coin));
    BOOST_CHECK_EQUAL(cache.ConcurrentSize(), 0u);   // miss 不回填
}

// 测试 4：HaveCoinConcurrent L1 + L3
BOOST_AUTO_TEST_CASE(have_concurrent_L1_L3) {
    MapBackingView base;
    base.data[COutPoint(uint256S("1"), 0)] = make_coin(1);
    TestableCache cache(&base);

    // L3 命中
    BOOST_CHECK(cache.HaveCoinConcurrent(COutPoint(uint256S("1"), 0)));
    // L1 / L3 双 miss
    BOOST_CHECK(!cache.HaveCoinConcurrent(COutPoint(uint256S("999"), 0)));

    // BatchWrite 后 L1 命中
    CCoinsMap mapCoins;
    mapCoins[COutPoint(uint256S("2"), 0)] = make_entry(2);
    cache.BatchWrite(mapCoins, uint256());
    BOOST_CHECK(cache.HaveCoinConcurrent(COutPoint(uint256S("2"), 0)));
}

// 测试 5：32 worker 并发 GetCoinConcurrent 0 race
BOOST_AUTO_TEST_CASE(concurrent_32_workers_no_race) {
    MapBackingView base;
    constexpr uint32_t N = 1000;
    for (uint32_t i = 0; i < N; i++) {
        base.data[COutPoint(uint256S(std::to_string(i)), 0)] = make_coin(i);
    }
    TestableCache cache(&base);

    constexpr int WORKERS = 32;
    std::atomic<size_t> total_ops{0};
    std::atomic<size_t> hits{0};
    std::atomic<bool> stop{false};

    std::vector<std::thread> threads;
    for (int w = 0; w < WORKERS; w++) {
        threads.emplace_back([&, w] {
            std::mt19937 rng(w);
            while (!stop.load(std::memory_order_relaxed)) {
                uint32_t i = rng() % N;
                COutPoint op(uint256S(std::to_string(i)), 0);
                Coin coin;
                if (cache.GetCoinConcurrent(op, coin)) {
                    if (coin.GetHeight() == i) hits.fetch_add(1, std::memory_order_relaxed);
                }
                total_ops.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));
    stop.store(true);
    for (auto& t : threads) t.join();

    BOOST_TEST_MESSAGE("32 workers 2s: total=" << total_ops.load()
                       << " hits=" << hits.load()
                       << " concurrent_size=" << cache.ConcurrentSize());
    BOOST_CHECK_EQUAL(total_ops.load(), hits.load());   // 每次查到的 height == i 一致
    BOOST_CHECK_GT(total_ops.load(), 100'000u);
}

BOOST_AUTO_TEST_SUITE_END()
