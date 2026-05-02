// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.

#include "mining/gbt_snapshot.h"

#include <atomic>
#include <chrono>
#include <thread>

#include <boost/test/unit_test.hpp>

using tbc::mining::GbtSnapshotProvider;
using tbc::mining::MempoolSnapshot;

BOOST_AUTO_TEST_SUITE(gbt_snapshot_tests)

namespace {
uint256 hash_of(uint8_t b) {
    uint256 h;
    std::memset(h.begin(), b, 32);
    return h;
}
} // anon

// 测试 1：RefreshAsync 触发 worker 拍快照
BOOST_AUTO_TEST_CASE(refresh_async_triggers_build) {
    std::atomic<int> built_count{0};
    std::atomic<int> tip_byte{0xaa};
    GbtSnapshotProvider p([&] {
        auto s = std::make_shared<MempoolSnapshot>();
        s->tip_hash = hash_of(static_cast<uint8_t>(tip_byte.load()));
        s->height = 100;
        built_count.fetch_add(1, std::memory_order_relaxed);
        return s;
    });
    p.Start();

    p.RefreshAsync();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (built_count.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    BOOST_CHECK_GT(built_count.load(), 0);
    p.Stop();
}

// 测试 2：连续 N 次 RefreshAsync 合并为少数实际拷贝
BOOST_AUTO_TEST_CASE(merge_concurrent_refresh) {
    std::atomic<int> built_count{0};
    GbtSnapshotProvider p([&] {
        auto s = std::make_shared<MempoolSnapshot>();
        s->tip_hash = hash_of(0x11);
        built_count.fetch_add(1, std::memory_order_relaxed);
        // 模拟拍 snapshot 慢一点
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return s;
    });
    p.Start();

    // 高速触发 100 次
    for (int i = 0; i < 100; i++) p.RefreshAsync();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    int total = built_count.load();
    p.Stop();

    BOOST_TEST_MESSAGE("100 RefreshAsync → built " << total << " times（合并）");
    BOOST_CHECK_LT(total, 100);   // 合并：实际拷贝 < 触发数
    BOOST_CHECK_GE(total, 1);
}

// 测试 3：WaitFresh 等到 expected_tip 返回
BOOST_AUTO_TEST_CASE(wait_fresh_matches_tip) {
    GbtSnapshotProvider p([&] {
        auto s = std::make_shared<MempoolSnapshot>();
        s->tip_hash = hash_of(0x22);
        s->height = 200;
        return s;
    });
    p.Start();
    p.RefreshAsync();

    auto snap = p.WaitFresh(hash_of(0x22), std::chrono::seconds(2));
    BOOST_REQUIRE(snap);
    BOOST_CHECK(snap->tip_hash == hash_of(0x22));
    BOOST_CHECK_EQUAL(snap->height, 200);
    p.Stop();
}

// 测试 4：WaitFresh timeout 仍返回 last_stable
BOOST_AUTO_TEST_CASE(wait_fresh_timeout_returns_last) {
    GbtSnapshotProvider p([&] {
        auto s = std::make_shared<MempoolSnapshot>();
        s->tip_hash = hash_of(0x33);
        return s;
    });
    p.Start();
    p.RefreshAsync();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 等不存在的 expected_tip → 100ms 后 timeout
    auto t0 = std::chrono::steady_clock::now();
    auto snap = p.WaitFresh(hash_of(0xFF), std::chrono::milliseconds(100));
    auto t1 = std::chrono::steady_clock::now();

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    BOOST_CHECK_GE(elapsed_ms, 90);
    BOOST_CHECK_LT(elapsed_ms, 1000);
    // snap 是 last_stable（tip 0x33，不是 0xFF）
    if (snap) BOOST_CHECK(snap->tip_hash == hash_of(0x33));
    p.Stop();
}

BOOST_AUTO_TEST_SUITE_END()
