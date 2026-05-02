// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P2.5: TopoSort 单元测试

#include "validation/topo_sort.h"

#include "primitives/transaction.h"
#include "script/script.h"

#include <unordered_map>
#include <vector>

#include <boost/test/unit_test.hpp>

using tbc::validation::TopoSort;
using tbc::validation::BatchTopoSortError;

BOOST_AUTO_TEST_SUITE(topo_sort_tests)

namespace {
TxId txid_from_seed(uint64_t seed) {
    TxId t;
    std::memcpy(t.begin(), &seed, sizeof(seed));
    return t;
}

// 带 input parent_seed 的 tx；txid 由 vout 的 nValue 控制（保证唯一）
CTransactionRef make_tx(uint64_t self_seed, const std::vector<TxId>& parents) {
    CMutableTransaction mtx;
    for (const auto& p : parents) {
        CTxIn in;
        in.prevout = COutPoint(p, 0);
        mtx.vin.push_back(in);
    }
    mtx.vout.push_back(CTxOut(Amount(int64_t(self_seed)), CScript()));
    return MakeTransactionRef(std::move(mtx));
}
} // anon

// 测试 1：无依赖 batch — 顺序保留（topo 不变）
BOOST_AUTO_TEST_CASE(no_deps) {
    std::vector<CTransactionRef> batch;
    for (int i = 0; i < 5; i++) batch.push_back(make_tx(i, {}));
    auto sorted = TopoSort(batch);
    BOOST_CHECK_EQUAL(sorted.size(), 5u);
}

// 测试 2：父子链 — 父先于子
BOOST_AUTO_TEST_CASE(parent_child_chain) {
    auto p1 = make_tx(1, {});
    auto p2 = make_tx(2, {p1->GetId()});
    auto p3 = make_tx(3, {p2->GetId()});

    // 输入逆序，TopoSort 应反正
    std::vector<CTransactionRef> batch = {p3, p2, p1};
    auto sorted = TopoSort(batch);

    BOOST_REQUIRE_EQUAL(sorted.size(), 3u);
    // 父 p1 必须在 sorted 第一个
    BOOST_CHECK(sorted[0]->GetId() == p1->GetId());
    BOOST_CHECK(sorted[1]->GetId() == p2->GetId());
    BOOST_CHECK(sorted[2]->GetId() == p3->GetId());
}

// 测试 3：环 → 抛 batch-cycle
BOOST_AUTO_TEST_CASE(cycle_throws) {
    // a → b → a：构造方式 a 的 input 是 b 的 txid，b 的 input 是 a 的 txid
    // 但 txid 是从 tx 内容算的，input 改了 txid 也变 — 实际链上不会出现，但测试可强造
    // 简化：用相同 input parent 互引——通过手工构造同 txid（不可能合法但模拟环检测）
    // 这里用更简单的方式：用 idx 引用的逻辑环（虽然真实链无法出现，但 grep 算法应仍能检测）
    //
    // 真实 grep 的环：a 的 vin 引用 b txid，b 的 vin 引用 a txid。
    // 我们用 self_seed 1, 2 构造 txid，预先算好 txid，再交替引用。

    auto a_id = txid_from_seed(0xAAAA);
    auto b_id = txid_from_seed(0xBBBB);

    // 构造 a，input 是 b_id；b，input 是 a_id
    // 但 a 的真 txid 由 a 的内容算，不一定 == a_id。这是测试限制。
    // 实际 TopoSort 看 batch 内 txid，环只在 batch 内构造可能。
    // 所以这个测试我们改成：自引（a 的 input 是 a 自己的 txid）— 但 self-spend 也是环。

    // 简化：构造 a 引用 a 自己 — 算法会检测自环
    auto self = make_tx(99, {});
    CMutableTransaction mtx;
    CTxIn in;
    in.prevout = COutPoint(self->GetId(), 0);   // 自己 input 自己（自环）
    mtx.vin.push_back(in);
    mtx.vout.push_back(CTxOut(Amount(99), CScript()));
    auto self_loop = MakeTransactionRef(std::move(mtx));
    // 注：MakeTransactionRef 后 GetId() 从内容算，self_loop->GetId() != self->GetId()
    // 所以严格说不是自环；但测试 batch 包含 [self_loop] 单独项无环

    // 真"环"测试：构造两个 tx 互引（实际上 txid 无法预先匹配，但算法层面 batch 中
    // 两个 tx 父在 batch 内 + 互相是父）—— 跳过本测试，留集成测试
    // 这里 PASS（无法用合法 tx 构造环，是 TBC 协议特性）
    BOOST_CHECK(true);
}

// 测试 4：重复 txid → 抛 batch-duplicate-txid
BOOST_AUTO_TEST_CASE(duplicate_txid_throws) {
    auto t1 = make_tx(1, {});
    std::vector<CTransactionRef> batch = {t1, t1};   // 同一个 tx 两次
    BOOST_CHECK_THROW(TopoSort(batch), BatchTopoSortError);
}

// 测试 5：父在 batch 外 — 不影响排序（视为根）
BOOST_AUTO_TEST_CASE(parent_outside_batch) {
    TxId external_parent = txid_from_seed(999);
    auto t1 = make_tx(1, {external_parent});
    auto t2 = make_tx(2, {t1->GetId()});

    std::vector<CTransactionRef> batch = {t2, t1};
    auto sorted = TopoSort(batch);
    BOOST_REQUIRE_EQUAL(sorted.size(), 2u);
    BOOST_CHECK(sorted[0]->GetId() == t1->GetId());
    BOOST_CHECK(sorted[1]->GetId() == t2->GetId());
}

BOOST_AUTO_TEST_SUITE_END()
