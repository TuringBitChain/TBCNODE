// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P4 §5.3 AsyncSubscriber unit tests

#include "validation/async_subscriber.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include <boost/test/unit_test.hpp>

using namespace tbc::validation;

BOOST_AUTO_TEST_SUITE(async_subscriber_tests)

// Test 1: 基本 enqueue + 异步消费
BOOST_AUTO_TEST_CASE(enqueue_and_consume) {
    AsyncSubscriber sub("test", 1024);
    std::atomic<int> count{0};
    sub.Start();
    for (int i = 0; i < 100; i++) {
        sub.Enqueue([&count] { count.fetch_add(1, std::memory_order_relaxed); });
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (count.load() < 100 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    BOOST_CHECK_EQUAL(count.load(), 100);
    sub.Stop();
}

// Test 2: FIFO 顺序保留
BOOST_AUTO_TEST_CASE(fifo_order) {
    AsyncSubscriber sub("test-fifo", 1024);
    std::vector<int> seen;
    std::mutex m;
    sub.Start();
    for (int i = 0; i < 200; i++) {
        sub.Enqueue([&seen, &m, i] {
            std::lock_guard<std::mutex> l(m);
            seen.push_back(i);
        });
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (true) {
        {
            std::lock_guard<std::mutex> l(m);
            if (seen.size() >= 200) break;
        }
        if (std::chrono::steady_clock::now() > deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    sub.Stop();
    BOOST_REQUIRE_EQUAL(seen.size(), 200u);
    for (size_t i = 0; i < seen.size(); i++) {
        BOOST_CHECK_EQUAL(seen[i], static_cast<int>(i));
    }
}

// Test 3: 容量上限触发丢消息计数
BOOST_AUTO_TEST_CASE(overflow_drops_oldest) {
    AsyncSubscriber sub("test-overflow", 8);
    // 先 push 满，但不 Start，让队列堆积
    sub.Start();
    // 让 worker 暂时不消费：用 latch 阻塞
    std::atomic<bool> blocked{true};
    std::atomic<bool> first_seen{false};
    sub.Enqueue([&blocked, &first_seen] {
        first_seen.store(true);
        while (blocked.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    // 等 worker 开始执行 first task
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!first_seen.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    BOOST_REQUIRE(first_seen.load());

    // 现在 worker 卡住，连续 push 100 笔，capacity=8 → 应有 ≥ 92 笔被丢
    for (int i = 0; i < 100; i++) {
        sub.Enqueue([] {});
    }
    BOOST_CHECK_GE(sub.DroppedOverflow(), 92u);

    blocked.store(false, std::memory_order_release);
    sub.Stop();
}

// Test 4: subscriber 抛异常不阻塞后续
BOOST_AUTO_TEST_CASE(exception_does_not_kill_worker) {
    AsyncSubscriber sub("test-exc", 64);
    std::atomic<int> good{0};
    sub.Start();
    sub.Enqueue([] { throw std::runtime_error("boom"); });
    sub.Enqueue([&good] { good.fetch_add(1, std::memory_order_relaxed); });
    sub.Enqueue([] { throw 42; });    // 非 std::exception
    sub.Enqueue([&good] { good.fetch_add(1, std::memory_order_relaxed); });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (good.load() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    BOOST_CHECK_EQUAL(good.load(), 2);
    sub.Stop();
}

// Test 5: Stop 幂等 + 未 Start 直接 Enqueue 计入 not_running 丢弃
BOOST_AUTO_TEST_CASE(not_running_drops) {
    AsyncSubscriber sub("test-nr", 64);
    sub.Enqueue([] {});
    sub.Enqueue([] {});
    BOOST_CHECK_EQUAL(sub.DroppedNotRunning(), 2u);
    sub.Stop();    // 幂等：未 Start 也能 Stop
    sub.Stop();    // 双重 Stop OK
}

BOOST_AUTO_TEST_SUITE_END()
