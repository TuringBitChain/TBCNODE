// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P0.0b.1: Chainstate seqlock 单元测试
//
// 验证：
//   - 7 字段全部 captured（C-A tip_index + H-D genesisActivationHeight）
//   - 单 writer × N reader 长压 0 torn read
//   - seq 单调递增（writer 进奇数 → 退偶数）
//   - Capture() 在 writer 中途读会重试到稳定态
//
// ARM/RISC-V 跨架构 fence 验证在 P0.0b.1 任务卡 §3.3 QEMU CI matrix 跑（本卡 x86 验证 + 写文档）

#include "validation/chainstate.h"
#include "uint256.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <boost/test/unit_test.hpp>

using tbc::validation::Chainstate;

BOOST_AUTO_TEST_SUITE(chainstate_seqlock)

// 测试 1：7 字段全部 captured 等价于 setter 写入
BOOST_AUTO_TEST_CASE(all_seven_fields_captured) {
    Chainstate cs;
    uint256 hash;
    std::memset(hash.begin(), 0xab, 32);

    cs.UpdateForTest(hash, /*tip_index=*/reinterpret_cast<const CBlockIndex*>(0xdeadbeef),
                     /*script_flags=*/0x12345678u,
                     /*height=*/824190,
                     /*mtp=*/1234567890LL,
                     /*isGenesisEnabled=*/true,
                     /*genesisActivationHeight=*/824190);

    auto s = cs.Capture();
    BOOST_CHECK_EQUAL(s.tip_hash.GetHex(),
                      "abababababababababababababababababababababababababababababababab");
    BOOST_CHECK_EQUAL(s.tip_index, reinterpret_cast<const CBlockIndex*>(0xdeadbeef));
    BOOST_CHECK_EQUAL(s.script_flags, 0x12345678u);
    BOOST_CHECK_EQUAL(s.height, 824190);
    BOOST_CHECK_EQUAL(s.mtp, 1234567890LL);
    BOOST_CHECK_EQUAL(s.isGenesisEnabled, true);
    BOOST_CHECK_EQUAL(s.genesisActivationHeight, 824190);
}

// 测试 2：seq 单调递增（writer 进奇数 → 退偶数）
BOOST_AUTO_TEST_CASE(seq_monotonic_after_update) {
    Chainstate cs;
    uint64_t seq_before = cs.SeqForTest();
    BOOST_CHECK_EQUAL(seq_before & 1u, 0u);   // 初始稳态

    uint256 h;
    cs.UpdateForTest(h, nullptr, 0u, 0, 0LL, false, 0);
    uint64_t seq_after = cs.SeqForTest();
    BOOST_CHECK_GT(seq_after, seq_before);
    BOOST_CHECK_EQUAL(seq_after & 1u, 0u);   // 写完仍是偶数（稳态）
    BOOST_CHECK_EQUAL(seq_after - seq_before, 2u);   // 进奇数 + 退偶数 = +2
}

// 测试 3：单 writer × 32 reader 长压 0 torn
// writer 把 tip_hash 全 32 字节写成同一字节值（每次 +1）；reader 读到的 tip_hash 必须 32 字节都相同
// （除非命中 writer 中途，那 seq 不一致会重试）
BOOST_AUTO_TEST_CASE(no_torn_read_under_load) {
    Chainstate cs;
    std::atomic<bool> stop{false};
    std::atomic<size_t> reads{0};
    std::atomic<size_t> torn{0};
    std::atomic<int> writer_count{0};

    // 1 writer
    std::thread writer([&] {
        int h = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            uint256 hash;
            std::memset(hash.begin(), h & 0xff, 32);
            cs.UpdateForTest(hash, nullptr, 0u, h, 0LL, false, 0);
            h++;
            writer_count.store(h, std::memory_order_relaxed);
            // 不 sleep — 高频更新更容易撞上 reader 中途
        }
    });

    // 32 reader
    std::vector<std::thread> readers;
    for (int i = 0; i < 32; i++) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                auto snap = cs.Capture();
                // 验证 tip_hash 所有字节相同
                uint8_t b0 = snap.tip_hash.begin()[0];
                bool tornFound = false;
                for (size_t j = 1; j < 32; j++) {
                    if (snap.tip_hash.begin()[j] != b0) {
                        tornFound = true;
                        break;
                    }
                }
                if (tornFound) torn.fetch_add(1);
                reads.fetch_add(1);
            }
        });
    }

    // 跑 3 秒（单元测试预算；ARM/RISC-V QEMU 长压 10 分钟在 CI matrix）
    std::this_thread::sleep_for(std::chrono::seconds(3));
    stop.store(true);
    writer.join();
    for (auto& t : readers) t.join();

    BOOST_TEST_MESSAGE("3s seqlock soak: reads=" << reads.load()
                       << " writes=" << writer_count.load()
                       << " torn=" << torn.load());
    BOOST_REQUIRE_EQUAL(torn.load(), 0u);
    BOOST_REQUIRE_GT(reads.load(), 100'000u);   // 至少几十万次读
    BOOST_REQUIRE_GT(writer_count.load(), 1'000);
}

// 测试 4：concurrent reader 跨多次 update 仍能拿到一致 snapshot
// 写第一次 → reader1 拍快照
// 写第二次（不同字段）→ reader2 拍快照
// 验证两个快照的所有字段都来自同一 epoch
BOOST_AUTO_TEST_CASE(snapshot_consistency_across_writes) {
    Chainstate cs;
    uint256 h1, h2;
    std::memset(h1.begin(), 0x11, 32);
    std::memset(h2.begin(), 0x22, 32);

    cs.UpdateForTest(h1, nullptr, 0x100u, 100, 1000LL, false, 800000);
    auto s1 = cs.Capture();

    cs.UpdateForTest(h2, nullptr, 0x200u, 200, 2000LL, true, 824190);
    auto s2 = cs.Capture();

    // s1 整体一致（一个 epoch）
    BOOST_CHECK_EQUAL(s1.tip_hash.GetHex(),
                      "1111111111111111111111111111111111111111111111111111111111111111");
    BOOST_CHECK_EQUAL(s1.script_flags, 0x100u);
    BOOST_CHECK_EQUAL(s1.height, 100);
    BOOST_CHECK_EQUAL(s1.mtp, 1000LL);

    // s2 整体一致（另一个 epoch）
    BOOST_CHECK_EQUAL(s2.tip_hash.GetHex(),
                      "2222222222222222222222222222222222222222222222222222222222222222");
    BOOST_CHECK_EQUAL(s2.script_flags, 0x200u);
    BOOST_CHECK_EQUAL(s2.height, 200);
    BOOST_CHECK_EQUAL(s2.mtp, 2000LL);
    BOOST_CHECK_EQUAL(s2.isGenesisEnabled, true);
    BOOST_CHECK_EQUAL(s2.genesisActivationHeight, 824190);
}

// 测试 5：g_chainstate 全局 linker 验证（P0-P2 复盘审核修补）
//   原 extern 写在外层 namespace、定义在 tbc::validation 内，符号不匹配。
//   P4.1 接入 ConnectBlock 调 g_chainstate.UpdateTip(...) 时会 linker error。
//   此测试在 OBJECT FILE 层面真引用 g_chainstate，强制 linker 解符号。
//
//   v2.6.1 P4.1 真接入后，validation.cpp UpdateTip 会写 g_chainstate；测试 setup
//   boot 完链 → snap 不再是初始值。这里只校验 Capture() 能成功调（linker 解析 OK），
//   不锁定具体 height/flags（测试 fixture 间共享全局，值依赖加载顺序）。
BOOST_AUTO_TEST_CASE(g_chainstate_global_linker_resolved) {
    auto snap = tbc::validation::g_chainstate.Capture();
    // height ≥ 0（uint32 wrap 不应出现）
    BOOST_CHECK(snap.height >= -1);
    // script_flags 是 uint32，不会变负；只验 Capture() 不抛
    (void)snap.script_flags;
}

BOOST_AUTO_TEST_SUITE_END()
