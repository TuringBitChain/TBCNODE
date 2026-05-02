// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P2.4: TxStash 单元测试（Phase B 后 ResubmitRateLimiter 已退役）

#include "validation/tx_stash.h"

#include "primitives/transaction.h"
#include "script/script.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <boost/test/unit_test.hpp>

using tbc::validation::TxStash;
using tbc::validation::ReorgStash;

BOOST_AUTO_TEST_SUITE(tx_stash_tests)

namespace {
CTransactionRef make_dummy_tx(uint64_t seed) {
    CMutableTransaction mtx;
    CTxIn in;
    TxId t;
    std::memcpy(t.begin(), &seed, sizeof(seed));
    in.prevout = COutPoint(t, 0);
    mtx.vin.push_back(in);
    mtx.vout.push_back(CTxOut(Amount(1000), CScript()));
    return MakeTransactionRef(std::move(mtx));
}
} // anon

// 测试 1：Push + Drain 基本路径
BOOST_AUTO_TEST_CASE(push_drain_basic) {
    using TestStash = TxStash<100, 60'000'000LL>;
    TestStash s;

    for (int i = 0; i < 50; i++) s.Push(make_dummy_tx(uint64_t(i)));
    BOOST_CHECK_EQUAL(s.Size(), 50u);

    auto drained = s.Drain(20);
    BOOST_CHECK_EQUAL(drained.size(), 20u);
    BOOST_CHECK_EQUAL(s.Size(), 30u);

    // FIFO：drained[0] = 第一次 push 的 seed=0
    uint64_t first_seed = 0;
    std::memcpy(&first_seed, drained[0]->vin[0].prevout.GetTxId().begin(), sizeof(first_seed));
    BOOST_CHECK_EQUAL(first_seed, 0u);
}

// 测试 2：Push 满 → drop_full（FIFO，最老的丢）
BOOST_AUTO_TEST_CASE(push_full_drops_oldest) {
    using TestStash = TxStash<10, 60'000'000LL>;
    TestStash s;

    for (int i = 0; i < 15; i++) s.Push(make_dummy_tx(uint64_t(i)));
    BOOST_CHECK_EQUAL(s.Size(), 10u);

    auto m = s.GetMetrics();
    BOOST_CHECK_EQUAL(m.push_total, 15u);
    BOOST_CHECK_EQUAL(m.drop_full, 5u);

    // 剩下的应该是 seed 5..14
    auto drained = s.Drain(10);
    uint64_t first_seed = 0;
    std::memcpy(&first_seed, drained[0]->vin[0].prevout.GetTxId().begin(), sizeof(first_seed));
    BOOST_CHECK_EQUAL(first_seed, 5u);
}

// 测试 3：GC TTL 路径
BOOST_AUTO_TEST_CASE(gc_ttl_drops) {
    using TestStash = TxStash<100, 100'000LL>;   // 100ms TTL
    TestStash s;

    s.Push(make_dummy_tx(1));
    BOOST_CHECK_EQUAL(s.Size(), 1u);

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    s.GC();   // 超 TTL，清掉
    BOOST_CHECK_EQUAL(s.Size(), 0u);

    auto m = s.GetMetrics();
    BOOST_CHECK_EQUAL(m.drop_ttl, 1u);
}

// 测试 4：metrics 准确
BOOST_AUTO_TEST_CASE(metrics_accuracy) {
    using TestStash = TxStash<100, 60'000'000LL>;
    TestStash s;

    for (int i = 0; i < 30; i++) s.Push(make_dummy_tx(uint64_t(i)));
    auto drained = s.Drain(10);

    auto m = s.GetMetrics();
    BOOST_CHECK_EQUAL(m.push_total, 30u);
    BOOST_CHECK_EQUAL(m.drain_total, 10u);
    BOOST_CHECK_EQUAL(m.current_size, 20u);
    BOOST_CHECK_EQUAL(m.drop_full, 0u);
    BOOST_CHECK_EQUAL(m.drop_ttl, 0u);
}

// 测试 5：100 线程并发 Push/Drain 0 race
BOOST_AUTO_TEST_CASE(concurrent_push_drain) {
    using TestStash = TxStash<100000, 60'000'000LL>;
    TestStash s;

    std::atomic<bool> stop{false};
    constexpr int PRODUCERS = 50;
    constexpr int CONSUMERS = 10;

    std::vector<std::thread> threads;
    std::atomic<size_t> total_pushed{0};
    std::atomic<size_t> total_drained{0};

    for (int p = 0; p < PRODUCERS; p++) {
        threads.emplace_back([&, p] {
            uint64_t seed = uint64_t(p) * 1000;
            while (!stop.load(std::memory_order_relaxed)) {
                s.Push(make_dummy_tx(seed++));
                total_pushed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (int c = 0; c < CONSUMERS; c++) {
        threads.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                auto v = s.Drain(50);
                total_drained.fetch_add(v.size(), std::memory_order_relaxed);
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::seconds(1));
    stop.store(true);
    for (auto& t : threads) t.join();

    BOOST_TEST_MESSAGE("1s concurrent: pushed=" << total_pushed.load()
                       << " drained=" << total_drained.load()
                       << " size=" << s.Size());
    BOOST_CHECK_GT(total_pushed.load(), 1000u);
    BOOST_CHECK_GT(total_drained.load(), 100u);
}

// Phase B (post-Teranode-audit)：测试 6/7 已删除 — ResubmitRateLimiter 类已退役
//   （sub-A3 后无 caller，dead code）。

BOOST_AUTO_TEST_SUITE_END()
