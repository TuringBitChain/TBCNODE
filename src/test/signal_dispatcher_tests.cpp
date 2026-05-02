// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.

#include "validation/signal_dispatcher.h"

#include "primitives/transaction.h"
#include "script/script.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <boost/test/unit_test.hpp>

using namespace tbc::validation;

BOOST_AUTO_TEST_SUITE(signal_dispatcher_tests)

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

// 测试 1：Enqueue + subscriber 接收
BOOST_AUTO_TEST_CASE(enqueue_dispatch_to_subscriber) {
    SignalDispatcher d;
    std::atomic<int> received{0};
    d.Subscribe([&](const SignalEvent&) { received.fetch_add(1, std::memory_order_relaxed); });
    d.Start();

    for (int i = 0; i < 50; i++) {
        d.Enqueue(SignalType::TransactionAddedToMempool, make_dummy_tx(uint64_t(i)));
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (received.load() < 50 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    BOOST_CHECK_EQUAL(received.load(), 50);
    d.Stop();
}

// 测试 2：FIFO 顺序保证（global_seq 单调）
BOOST_AUTO_TEST_CASE(fifo_global_seq) {
    SignalDispatcher d;
    std::vector<uint64_t> seen_seqs;
    std::mutex seen_mtx;
    d.Subscribe([&](const SignalEvent& e) {
        std::lock_guard l(seen_mtx);
        seen_seqs.push_back(e.global_seq);
    });
    d.Start();

    for (int i = 0; i < 100; i++) {
        d.Enqueue(SignalType::TransactionAddedToMempool, make_dummy_tx(uint64_t(i)));
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (true) {
        std::lock_guard l(seen_mtx);
        if (seen_seqs.size() >= 100) break;
        if (std::chrono::steady_clock::now() > deadline) break;
        seen_mtx.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        seen_mtx.lock();
    }
    d.Stop();

    BOOST_REQUIRE_EQUAL(seen_seqs.size(), 100u);
    for (size_t i = 1; i < seen_seqs.size(); i++) {
        BOOST_CHECK_LT(seen_seqs[i-1], seen_seqs[i]);
    }
}

// 测试 3：subscriber 抛异常不阻塞 dispatch
BOOST_AUTO_TEST_CASE(subscriber_exception_swallowed) {
    SignalDispatcher d;
    std::atomic<int> received_a{0}, received_b{0};
    d.Subscribe([&](const SignalEvent&) {
        received_a.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error("simulated subscriber error");
    });
    d.Subscribe([&](const SignalEvent&) {
        received_b.fetch_add(1, std::memory_order_relaxed);
    });
    d.Start();

    for (int i = 0; i < 20; i++) {
        d.Enqueue(SignalType::TransactionAddedToMempool, make_dummy_tx(uint64_t(i)));
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((received_a.load() < 20 || received_b.load() < 20) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    BOOST_CHECK_EQUAL(received_a.load(), 20);
    BOOST_CHECK_EQUAL(received_b.load(), 20);   // subscriber B 不受 A 异常影响
    d.Stop();
}

// 测试 4：Stop 优雅退出
BOOST_AUTO_TEST_CASE(stop_clean) {
    SignalDispatcher d;
    d.Start();
    d.Enqueue(SignalType::TransactionAddedToMempool, make_dummy_tx(1));
    d.Stop();
    BOOST_CHECK(true);   // 走到这说明 thread.join 完成
}

BOOST_AUTO_TEST_SUITE_END()
