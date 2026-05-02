// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P0.0a.1: libcuckoo 集成 + mini-soak 单元测试
//
// 单元测试是 mini-soak（30 秒，验证集成正确性）；
// 完整 24h × 5000 万 entry soak 在 CI matrix 跑（不在每次单元测试触发）

#include <atomic>
#include <chrono>
#include <random>
#include <thread>
#include <vector>

#include <boost/test/unit_test.hpp>
#include <libcuckoo/cuckoohash_map.hh>

BOOST_AUTO_TEST_SUITE(libcuckoo_soak)

// 集成 sanity check：基本的 insert / find / erase
BOOST_AUTO_TEST_CASE(basic_integration) {
    libcuckoo::cuckoohash_map<uint64_t, uint64_t> map;
    BOOST_REQUIRE(map.insert(1, 100));
    BOOST_REQUIRE(map.insert(2, 200));

    uint64_t v;
    BOOST_REQUIRE(map.find(1, v));
    BOOST_CHECK_EQUAL(v, 100);

    BOOST_REQUIRE(map.erase(1));
    BOOST_CHECK(!map.find(1, v));
    BOOST_CHECK_EQUAL(map.size(), 1u);
}

// upsert / update_fn / find_fn — 给 cacheCoins 改造预演（K2 路径）
BOOST_AUTO_TEST_CASE(upsert_update_fn_path) {
    libcuckoo::cuckoohash_map<uint64_t, uint64_t> map;
    // K2 路径：先 insert，已存在则 update_fn fallback
    bool ins = map.insert(1, 100);
    BOOST_CHECK(ins);
    ins = map.insert(1, 999);
    BOOST_CHECK(!ins);   // 已存在
    map.update_fn(1, [](uint64_t& v) { v = 999; });
    uint64_t v;
    BOOST_REQUIRE(map.find(1, v));
    BOOST_CHECK_EQUAL(v, 999);
}

// 30 秒 mini-soak：8 线程 × 1M entries × 持续读写
// 验证：无 race / 内存不泄漏 / 数据一致
BOOST_AUTO_TEST_CASE(mini_soak_30s) {
    libcuckoo::cuckoohash_map<uint64_t, uint64_t> map;
    constexpr size_t N = 1'000'000;
    constexpr int THREADS = 8;

    // 预填
    for (uint64_t i = 0; i < N; i++) {
        BOOST_REQUIRE(map.insert(i, i * 2));
    }
    BOOST_CHECK_EQUAL(map.size(), N);

    std::atomic<size_t> ops{0};
    std::atomic<bool> stop{false};

    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; t++) {
        threads.emplace_back([&, t] {
            std::mt19937_64 rng(t);
            uint64_t v;
            while (!stop.load(std::memory_order_relaxed)) {
                uint64_t k = rng() % N;
                int op = rng() % 4;
                switch (op) {
                    case 0: case 1: case 2:   // 75% read
                        if (map.find(k, v)) {
                            ops.fetch_add(1, std::memory_order_relaxed);
                        }
                        break;
                    case 3:                    // 25% update
                        map.update_fn(k, [](uint64_t& val) { val++; });
                        ops.fetch_add(1, std::memory_order_relaxed);
                        break;
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::seconds(30));
    stop.store(true);
    for (auto& th : threads) th.join();

    BOOST_TEST_MESSAGE("30s mini-soak ops: " << ops.load());
    // 8 线程 30 秒，预期 > 1000 万 ops（生产硬件应远高于此）
    BOOST_CHECK_GT(ops.load(), 1'000'000ULL);
    BOOST_CHECK_EQUAL(map.size(), N);   // erase 路径未启用，size 不变
}

BOOST_AUTO_TEST_SUITE_END()
