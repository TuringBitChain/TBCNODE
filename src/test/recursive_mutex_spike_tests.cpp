// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P0.0a.5: boost::recursive_mutex try_lock 语义 spike
// + H-F 异常注入 RAII unique_lock 平衡测试
//
// 验证 F2 单写者守卫的核心前提：
//   - 同线程已持锁 → try_lock 返回 true 且递增 count
//   - 跨线程持锁 → try_lock 返回 false（核心断言）
//   - 异常路径 → RAII unique_lock 析构平衡 count
//
// 当前 build 用的 boost 版本：1.74.0（CMake configure 输出确认）
// 4 boost 版本完整 spike (1.65/1.71/1.74/1.83) 在 P0.0a CI matrix 跑

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include <boost/test/unit_test.hpp>
#include <boost/thread/lock_guard.hpp>
#include <boost/thread/recursive_mutex.hpp>

BOOST_AUTO_TEST_SUITE(recursive_mutex_spike)

// ============================================================
// 测试 1：主线程持锁 → 主线程 try_lock → 期望 true
// ============================================================
BOOST_AUTO_TEST_CASE(self_thread_already_holding) {
    boost::recursive_mutex m;
    boost::lock_guard<boost::recursive_mutex> lg(m);
    BOOST_REQUIRE(m.try_lock());   // 期望 true，count 递增
    m.unlock();   // 还回去
    // 再次 try_lock 仍 true（主线程仍持有 lg 那一层）
    BOOST_REQUIRE(m.try_lock());
    m.unlock();
}

// ============================================================
// 测试 2：主线程持锁 → 副线程 try_lock → 期望 false（核心断言）
// ============================================================
BOOST_AUTO_TEST_CASE(other_thread_blocked) {
    boost::recursive_mutex m;
    std::atomic<bool> result{true};

    boost::lock_guard<boost::recursive_mutex> lg(m);
    std::thread t([&] {
        result = m.try_lock();
        if (result) m.unlock();
    });
    t.join();
    BOOST_REQUIRE(!result);   // 期望 false
}

// ============================================================
// 测试 3：递归 N 次持锁 + try_lock + unlock 平衡
// ============================================================
BOOST_AUTO_TEST_CASE(recursive_count_balanced) {
    boost::recursive_mutex m;
    constexpr int N = 100;
    for (int i = 0; i < N; i++) m.lock();
    BOOST_REQUIRE(m.try_lock());   // 期望 true
    m.unlock();   // 还回 try_lock 那一层
    for (int i = 0; i < N; i++) m.unlock();   // 还回所有层

    // 验证：count 真归零，跨线程能拿
    std::atomic<bool> other_can_lock{false};
    std::thread t([&] {
        other_can_lock = m.try_lock();
        if (other_can_lock) m.unlock();
    });
    t.join();
    BOOST_REQUIRE(other_can_lock);
}

// ============================================================
// 测试 4：高频路径 baseline（< 50ns/op）
// 该 KPI 是软 KPI，在 spike 阶段实测确定具体阈值
// ============================================================
BOOST_AUTO_TEST_CASE(performance_baseline) {
    boost::recursive_mutex m;
    constexpr int N = 100'000;   // 缩到 10w 避免单元测试拖太久
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; i++) {
        m.lock();
        if (m.try_lock()) m.unlock();
        m.unlock();
    }
    auto t1 = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    double per_op = double(ns) / N;
    BOOST_TEST_MESSAGE("per try_lock+unlock: " << per_op << " ns");
    // 软 KPI：50ns 是宽松上限（生产硬件应远低于此）
    BOOST_CHECK_LT(per_op, 500.0);   // 容错宽放（CI / sanitizer build 慢 10x）
}

// ============================================================
// 测试 5：H-F 异常注入 — RAII unique_lock 析构应平衡 count
// 模拟 F2 守卫路径：try_lock → ... → 异常 → 析构
// 关键：std::unique_lock<boost::recursive_mutex>(m, std::try_to_lock)
//       析构时 owns_lock() 为 true 才 unlock，异常路径必须 count 平衡
// ============================================================
BOOST_AUTO_TEST_CASE(exception_safety_raii_balanced) {
    boost::recursive_mutex m;
    m.lock();   // 主线程持有 1 层

    bool exception_thrown = false;
    try {
        // F2 守卫的等价模式（用 std::unique_lock 包 boost::recursive_mutex）
        // 注：std::unique_lock 能用任何符合 BasicLockable 的 mutex 类型
        std::unique_lock<boost::recursive_mutex> probe(m, std::try_to_lock);
        BOOST_REQUIRE(probe.owns_lock());   // 同线程已持 → try_to_lock 成功
        // 模拟 LogPrintf OOM 等异常
        throw std::runtime_error("simulated FATAL log path");
    } catch (const std::runtime_error&) {
        exception_thrown = true;
        // probe 析构已发生，应自动 unlock（count 减 1）
    }
    BOOST_REQUIRE(exception_thrown);

    // 验证 count 平衡：主线程仍持 1 层（外面 m.lock() 那层）
    m.unlock();   // 减最后 1 层

    // 验证真归零：副线程能拿
    std::atomic<bool> other_can_lock{false};
    std::thread t([&] {
        other_can_lock = m.try_lock();
        if (other_can_lock) m.unlock();
    });
    t.join();
    BOOST_REQUIRE(other_can_lock);   // 期望 true，确认 count 平衡
}

BOOST_AUTO_TEST_SUITE_END()
