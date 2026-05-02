// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P2.3: PerChainWorker thread loop + queue + cv 测试

#include "validation/per_chain_worker.h"

#include "primitives/transaction.h"
#include "script/script.h"

#include <atomic>
#include <chrono>
#include <thread>

#include <boost/test/unit_test.hpp>

using tbc::validation::PerChainWorker;
using tbc::validation::WorkItem;

BOOST_AUTO_TEST_SUITE(per_chain_worker_tests)

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

// 测试 1：Push + worker 线程消费 + Stop 优雅退出
BOOST_AUTO_TEST_CASE(push_consume_stop) {
    std::atomic<int> processed{0};
    PerChainWorker w(0, [&](WorkItem&&) {
        processed.fetch_add(1, std::memory_order_relaxed);
    });

    for (int i = 0; i < 100; i++) {
        w.Push(WorkItem(make_dummy_tx(uint64_t(i))));
    }

    // 等 worker 处理完
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (processed.load() < 100 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    BOOST_CHECK_EQUAL(processed.load(), 100);

    w.Stop();   // dtor 也会调，这里显式调测试
}

// 测试 2：FIFO 顺序保证
BOOST_AUTO_TEST_CASE(fifo_order) {
    std::vector<uint64_t> processed_order;
    std::mutex order_mtx;

    PerChainWorker w(0, [&](WorkItem&& item) {
        uint64_t seed = 0;
        std::memcpy(&seed, item.tx->vin[0].prevout.GetTxId().begin(), sizeof(seed));
        std::lock_guard lock(order_mtx);
        processed_order.push_back(seed);
    });

    for (uint64_t i = 0; i < 50; i++) {
        w.Push(WorkItem(make_dummy_tx(i)));
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (true) {
        std::lock_guard lock(order_mtx);
        if (processed_order.size() >= 50) break;
        if (std::chrono::steady_clock::now() > deadline) break;
        order_mtx.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        order_mtx.lock();
    }

    w.Stop();
    BOOST_REQUIRE_EQUAL(processed_order.size(), 50u);
    for (uint64_t i = 0; i < 50; i++) {
        BOOST_CHECK_EQUAL(processed_order[i], i);
    }
}

// 测试 3：handler 抛异常不让 worker 死
BOOST_AUTO_TEST_CASE(handler_exception_swallowed) {
    std::atomic<int> processed{0};
    PerChainWorker w(0, [&](WorkItem&& item) {
        int n = processed.fetch_add(1, std::memory_order_relaxed);
        if (n % 2 == 0) {
            throw std::runtime_error("simulated handler exception");
        }
    });

    for (int i = 0; i < 20; i++) {
        w.Push(WorkItem(make_dummy_tx(uint64_t(i))));
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (processed.load() < 20 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    BOOST_CHECK_EQUAL(processed.load(), 20);   // 即使一半抛异常，全部仍被处理
    w.Stop();
}

// 测试 4：32 producer 并发 Push 0 race
BOOST_AUTO_TEST_CASE(concurrent_push_no_race) {
    std::atomic<int> processed{0};
    PerChainWorker w(0, [&](WorkItem&&) {
        processed.fetch_add(1, std::memory_order_relaxed);
    });

    constexpr int PRODUCERS = 32;
    constexpr int PER_PRODUCER = 100;
    std::vector<std::thread> producers;
    for (int p = 0; p < PRODUCERS; p++) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < PER_PRODUCER; i++) {
                w.Push(WorkItem(make_dummy_tx(uint64_t(p * PER_PRODUCER + i))));
            }
        });
    }
    for (auto& t : producers) t.join();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (processed.load() < PRODUCERS * PER_PRODUCER &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    BOOST_CHECK_EQUAL(processed.load(), PRODUCERS * PER_PRODUCER);
    w.Stop();
}

// 测试 5：LastProgressUs 单调递增
BOOST_AUTO_TEST_CASE(last_progress_us_advances) {
    PerChainWorker w(0, [](WorkItem&&) {});
    int64_t t0 = w.LastProgressUs();
    w.Push(WorkItem(make_dummy_tx(1)));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int64_t t1 = w.LastProgressUs();
    BOOST_CHECK_GT(t1, t0);
    w.Stop();
}

BOOST_AUTO_TEST_SUITE_END()
