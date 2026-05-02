// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P0.0b.2: BatchWrite p99 prototype sanity check
//
// 单元测试只做轻量 sanity（< 5 秒）——验证 libcuckoo + shared_mutex prototype 路径正确
// 真 KPI（10 万 UTXO p99 ≤ 200ms）是 bench 目标，在 src/bench/ 跑（不在单元测试触发）
// 完整 5000 万 entry × 30 batch × 32 reader 的 benchmark 留 GATE-M0 跑
//
// 此卡只验证：(a) libcuckoo upsert 路径正确 (b) BatchWrite 跟 shared_lock 配合无 race

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <random>
#include <shared_mutex>
#include <thread>
#include <vector>

#include <boost/test/unit_test.hpp>
#include <libcuckoo/cuckoohash_map.hh>

BOOST_AUTO_TEST_SUITE(batchwrite_p99)

namespace {

// 简化 Coin（32 字节）
struct CoinMock {
    std::array<uint8_t, 32> bytes;
};

using CoinMap = libcuckoo::cuckoohash_map<uint64_t, CoinMock>;

// 用 K2 路径写：insert，已存在用 update_fn fallback
void write_one(CoinMap& map, uint64_t k, const CoinMock& v) {
    bool ins = map.insert(k, v);
    if (!ins) {
        map.update_fn(k, [&](CoinMock& existing) noexcept { existing = v; });
    }
}

} // anon

// 测试 1：BatchWrite + 4 reader 并发（轻量 sanity，< 1 秒）
BOOST_AUTO_TEST_CASE(batchwrite_concurrent_no_race) {
    CoinMap map;
    std::shared_mutex bw_mtx;

    constexpr size_t N = 10'000;          // 1 万 baseline（不是 5000 万）
    constexpr size_t BATCH = 1'000;       // 1000 / batch（不是 10 万）
    constexpr int N_BATCHES = 3;
    constexpr int READERS = 4;            // 4 reader（不是 32）

    // 预填
    for (uint64_t i = 0; i < N; i++) {
        CoinMock v{};
        std::memcpy(v.bytes.data(), &i, sizeof(i));
        write_one(map, i, v);
    }
    BOOST_REQUIRE_EQUAL(map.size(), N);

    std::atomic<bool> stop_readers{false};
    std::atomic<size_t> reader_ops{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < READERS; i++) {
        readers.emplace_back([&, i] {
            std::mt19937_64 rng(i);
            while (!stop_readers.load(std::memory_order_relaxed)) {
                std::shared_lock<std::shared_mutex> lk(bw_mtx);
                uint64_t k = rng() % N;
                CoinMock v;
                if (map.find(k, v)) {
                    reader_ops.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    // BatchWrite 3 次
    std::mt19937_64 rng(99);
    std::vector<double> latencies_ms;
    for (int b = 0; b < N_BATCHES; b++) {
        std::vector<std::pair<uint64_t, CoinMock>> batch;
        batch.reserve(BATCH);
        uint64_t base = rng() % (N - BATCH);
        for (size_t i = 0; i < BATCH; i++) {
            CoinMock v{};
            uint64_t k = base + i;
            std::memcpy(v.bytes.data(), &k, sizeof(k));
            batch.emplace_back(k, v);
        }

        auto t0 = std::chrono::steady_clock::now();
        {
            std::unique_lock<std::shared_mutex> bw(bw_mtx);
            for (auto& [k, v] : batch) {
                write_one(map, k, v);
            }
        }
        auto t1 = std::chrono::steady_clock::now();
        latencies_ms.push_back(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0);
    }

    stop_readers.store(true);
    for (auto& t : readers) t.join();

    // 报告（信息性，不强制 KPI——单元测试只验证路径正确）
    double max_ms = *std::max_element(latencies_ms.begin(), latencies_ms.end());
    BOOST_TEST_MESSAGE("BatchWrite 1k UTXO with 4 readers: max=" << max_ms
                       << "ms; reader ops=" << reader_ops.load());

    BOOST_CHECK_EQUAL(map.size(), N);   // size 不变（写已有 key）
    BOOST_CHECK_GT(reader_ops.load(), 0u);   // reader 真在跑
    BOOST_CHECK_LT(max_ms, 5000.0);   // 1k 个写不应超过 5 秒（容错宽放）
}

BOOST_AUTO_TEST_SUITE_END()
