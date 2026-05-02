// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.

#include "validation/async_trim.h"

#include <atomic>
#include <chrono>
#include <thread>

#include <boost/test/unit_test.hpp>

using tbc::validation::AsyncTrim;

BOOST_AUTO_TEST_SUITE(async_trim_tests)

// 测试 1：Notify 触发 trim_func 调用
BOOST_AUTO_TEST_CASE(notify_triggers_trim) {
    std::atomic<int> calls{0};
    AsyncTrim trim([&](size_t max_evict) -> size_t {
        calls.fetch_add(1, std::memory_order_relaxed);
        return 0;   // 立即返回 0 表示 trim 完成
    });

    trim.NotifyUrgent();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (calls.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    BOOST_CHECK_GT(calls.load(), 0);
    trim.Stop();
}

// 测试 2：Stop 优雅退出
BOOST_AUTO_TEST_CASE(stop_clean_exit) {
    AsyncTrim trim([](size_t) { return 0; });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    trim.Stop();   // dtor 也会调，这里显式调测试
    // 走到这说明 thread.join 完成
    BOOST_CHECK(true);
}

// 测试 3：拆批 — 单次 wake 最多 MAX_BATCHES_PER_WAKE 批
BOOST_AUTO_TEST_CASE(batch_split) {
    std::atomic<int> calls{0};
    AsyncTrim trim([&](size_t max_evict) -> size_t {
        calls.fetch_add(1, std::memory_order_relaxed);
        return max_evict;   // 返回满批，触发下一批
    });

    trim.NotifyUrgent();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 拆批：单次 wake ≤ 10 批；200ms 内 idle wait 10ms × N 次多次 wake 累积
    int n = calls.load();
    BOOST_TEST_MESSAGE("async_trim 200ms 调用次数: " << n);
    BOOST_CHECK_GT(n, 0);
    BOOST_CHECK_LT(n, 5000);   // 防 runaway
    trim.Stop();
}

// 测试 4：trim_func 抛异常不让线程死
BOOST_AUTO_TEST_CASE(trim_exception_swallowed) {
    std::atomic<int> calls{0};
    AsyncTrim trim([&](size_t) -> size_t {
        int n = calls.fetch_add(1, std::memory_order_relaxed);
        if (n % 2 == 0) throw std::runtime_error("simulated");
        return 0;
    });

    for (int i = 0; i < 5; i++) {
        trim.NotifyUrgent();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    BOOST_CHECK_GT(calls.load(), 1);   // 多次 trim 仍被调
    trim.Stop();
}

BOOST_AUTO_TEST_SUITE_END()
