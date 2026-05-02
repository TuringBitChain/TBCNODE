// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P3.1 + H-D：doubleCheck 协议 + perInputScriptFlags 等价性矩阵（5 项）

#include "validation/double_check.h"
#include "validation/chainstate.h"

#include <boost/test/unit_test.hpp>

using tbc::validation::Chainstate;
using tbc::validation::DerivePerInputScriptFlags;
using tbc::validation::DeriveAllPerInputScriptFlags;
using tbc::validation::CheckSnapStable;
using tbc::validation::DoubleCheckResult;

BOOST_AUTO_TEST_SUITE(double_check_tests)

namespace {
Chainstate::Snapshot make_snap(int32_t height, int32_t genesis_height) {
    Chainstate::Snapshot s;
    s.height = height;
    s.genesisActivationHeight = genesis_height;
    s.script_flags = 0;
    s.isGenesisEnabled = (height >= genesis_height);
    return s;
}
} // anon

// ============================================================================
// H-D 等价性矩阵（5 项 — P-1 修复后版本）
// 验证：对 snap.height/genesisActivationHeight 跟 coin.height 各组合
//      DerivePerInputScriptFlags 返回值跟 src/validation.cpp:3597 现行逻辑等价
// ============================================================================

// 测试 1：chain UTXO height < genesisActivationHeight → flags = 0
BOOST_AUTO_TEST_CASE(hd_chain_pre_genesis) {
    auto snap = make_snap(/*height=*/100, /*genesis=*/200);
    uint32_t f = DerivePerInputScriptFlags(/*coin_height=*/50, snap);
    BOOST_CHECK_EQUAL(f, 0u);
}

// 测试 2：chain UTXO height >= genesisActivationHeight → SCRIPT_UTXO_AFTER_GENESIS
BOOST_AUTO_TEST_CASE(hd_chain_post_genesis) {
    auto snap = make_snap(/*height=*/100, /*genesis=*/50);
    uint32_t f = DerivePerInputScriptFlags(/*coin_height=*/80, snap);
    BOOST_CHECK_EQUAL(f, SCRIPT_UTXO_AFTER_GENESIS);
}

// 测试 3：MEMPOOL_HEIGHT，snap.height+1 < genesisActivationHeight → 0
BOOST_AUTO_TEST_CASE(hd_mempool_pre_genesis) {
    auto snap = make_snap(/*height=*/99, /*genesis=*/200);
    uint32_t f = DerivePerInputScriptFlags(MEMPOOL_HEIGHT_SENTINEL, snap);
    BOOST_CHECK_EQUAL(f, 0u);   // 99+1=100 < 200
}

// 测试 4：MEMPOOL_HEIGHT，snap.height+1 >= genesisActivationHeight → SCRIPT_UTXO_AFTER_GENESIS
BOOST_AUTO_TEST_CASE(hd_mempool_post_genesis) {
    auto snap = make_snap(/*height=*/199, /*genesis=*/200);
    uint32_t f = DerivePerInputScriptFlags(MEMPOOL_HEIGHT_SENTINEL, snap);
    BOOST_CHECK_EQUAL(f, SCRIPT_UTXO_AFTER_GENESIS);   // 199+1=200 >= 200
}

// 测试 5：边缘 — coin.height == genesisActivationHeight - 1（最后一个 pre-genesis）
BOOST_AUTO_TEST_CASE(hd_edge_one_below_genesis) {
    auto snap = make_snap(/*height=*/300, /*genesis=*/200);
    BOOST_CHECK_EQUAL(DerivePerInputScriptFlags(199, snap), 0u);
    BOOST_CHECK_EQUAL(DerivePerInputScriptFlags(200, snap), SCRIPT_UTXO_AFTER_GENESIS);
}

// 测试 6：批量版本 DeriveAllPerInputScriptFlags
BOOST_AUTO_TEST_CASE(hd_batch_derive) {
    auto snap = make_snap(/*height=*/300, /*genesis=*/200);
    std::vector<Coin> coins;
    // 3 个 coin：50（pre）, 200（at genesis），MEMPOOL（snap.height+1=301 post）
    CTxOut out1, out2, out3;
    coins.push_back(Coin(out1, 50, false));
    coins.push_back(Coin(out2, 200, false));
    coins.push_back(Coin(out3, MEMPOOL_HEIGHT_SENTINEL, false));

    auto flags = DeriveAllPerInputScriptFlags(coins, snap);
    BOOST_REQUIRE_EQUAL(flags.size(), 3u);
    BOOST_CHECK_EQUAL(flags[0], 0u);
    BOOST_CHECK_EQUAL(flags[1], SCRIPT_UTXO_AFTER_GENESIS);
    BOOST_CHECK_EQUAL(flags[2], SCRIPT_UTXO_AFTER_GENESIS);
}

// ============================================================================
// doubleCheck #1+#2 测试（CheckSnapStable）
// ============================================================================

// 测试 7：tip_hash + script_flags 都没变 → OK
BOOST_AUTO_TEST_CASE(snap_stable_ok) {
    Chainstate::Snapshot s1, s2;
    std::memset(s1.tip_hash.begin(), 0xab, 32);
    std::memcpy(s2.tip_hash.begin(), s1.tip_hash.begin(), 32);
    s1.script_flags = 0x100;
    s2.script_flags = 0x100;
    BOOST_CHECK(CheckSnapStable(s1, s2) == DoubleCheckResult::OK);
}

// 测试 8：tip_hash 变 → TipChanged
BOOST_AUTO_TEST_CASE(snap_tip_changed) {
    Chainstate::Snapshot s1, s2;
    std::memset(s1.tip_hash.begin(), 0xab, 32);
    std::memset(s2.tip_hash.begin(), 0xcd, 32);
    BOOST_CHECK(CheckSnapStable(s1, s2) == DoubleCheckResult::TipChanged);
}

// 测试 9：script_flags 变 → FlagsChanged
BOOST_AUTO_TEST_CASE(snap_flags_changed) {
    Chainstate::Snapshot s1, s2;
    std::memset(s1.tip_hash.begin(), 0xab, 32);
    std::memcpy(s2.tip_hash.begin(), s1.tip_hash.begin(), 32);
    s1.script_flags = 0x100;
    s2.script_flags = 0x200;
    BOOST_CHECK(CheckSnapStable(s1, s2) == DoubleCheckResult::FlagsChanged);
}

BOOST_AUTO_TEST_SUITE_END()
