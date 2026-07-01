// Copyright (c) 2011-2016 The Bitcoin Core developers
// Copyright (c) 2020 Bitcoin Association
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "config.h"
#include "mining/journal.h"
#include "mining/journal_builder.h"
#include "mining/journal_change_set.h"
#include "mining/journal_entry.h"
#include "policy/policy.h"
#include "txmempool.h"
#include "util.h"

#include "test/test_bitcoin.h"

#include <boost/test/unit_test.hpp>
#include <list>
#include <vector>

namespace
{
    mining::CJournalChangeSetPtr nullChangeSet {nullptr};
}

BOOST_FIXTURE_TEST_SUITE(mempool_tests, TestingSetup)

BOOST_AUTO_TEST_CASE(MempoolRemoveTest) {
    // Test CTxMemPool::remove functionality

    TestMemPoolEntryHelper entry;
    // Parent transaction with three children, and three grand-children:
    CMutableTransaction txParent;
    txParent.vin.resize(1);
    txParent.vin[0].scriptSig = CScript() << OP_11;
    txParent.vout.resize(3);
    for (int i = 0; i < 3; i++) {
        txParent.vout[i].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
        txParent.vout[i].nValue = Amount(33000LL);
    }
    CMutableTransaction txChild[3];
    for (int i = 0; i < 3; i++) {
        txChild[i].vin.resize(1);
        txChild[i].vin[0].scriptSig = CScript() << OP_11;
        txChild[i].vin[0].prevout = COutPoint(txParent.GetId(), i);
        txChild[i].vout.resize(1);
        txChild[i].vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
        txChild[i].vout[0].nValue = Amount(11000LL);
    }
    CMutableTransaction txGrandChild[3];
    for (int i = 0; i < 3; i++) {
        txGrandChild[i].vin.resize(1);
        txGrandChild[i].vin[0].scriptSig = CScript() << OP_11;
        txGrandChild[i].vin[0].prevout = COutPoint(txChild[i].GetId(), 0);
        txGrandChild[i].vout.resize(1);
        txGrandChild[i].vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
        txGrandChild[i].vout[0].nValue = Amount(11000LL);
    }

    CTxMemPool testPool;

    // Nothing in pool, remove should do nothing:
    unsigned int poolSize = testPool.Size();
    testPool.RemoveRecursive(CTransaction(txParent), nullChangeSet);
    BOOST_CHECK_EQUAL(testPool.Size(), poolSize);

    // Just the parent:
    testPool.AddUnchecked(txParent.GetId(), entry.FromTx(txParent), nullChangeSet);
    poolSize = testPool.Size();
    testPool.RemoveRecursive(CTransaction(txParent), nullChangeSet);
    BOOST_CHECK_EQUAL(testPool.Size(), poolSize - 1);

    // Parent, children, grandchildren:
    testPool.AddUnchecked(txParent.GetId(), entry.FromTx(txParent), nullChangeSet);
    for (int i = 0; i < 3; i++) {
        testPool.AddUnchecked(txChild[i].GetId(), entry.FromTx(txChild[i]), nullChangeSet);
        testPool.AddUnchecked(txGrandChild[i].GetId(),
                              entry.FromTx(txGrandChild[i]), nullChangeSet);
    }
    // Remove Child[0], GrandChild[0] should be removed:
    poolSize = testPool.Size();
    testPool.RemoveRecursive(CTransaction(txChild[0]), nullChangeSet);
    BOOST_CHECK_EQUAL(testPool.Size(), poolSize - 2);
    // ... make sure grandchild and child are gone:
    poolSize = testPool.Size();
    testPool.RemoveRecursive(CTransaction(txGrandChild[0]), nullChangeSet);
    BOOST_CHECK_EQUAL(testPool.Size(), poolSize);
    poolSize = testPool.Size();
    testPool.RemoveRecursive(CTransaction(txChild[0]), nullChangeSet);
    BOOST_CHECK_EQUAL(testPool.Size(), poolSize);
    // Remove parent, all children/grandchildren should go:
    poolSize = testPool.Size();
    testPool.RemoveRecursive(CTransaction(txParent), nullChangeSet);
    BOOST_CHECK_EQUAL(testPool.Size(), poolSize - 5);
    BOOST_CHECK_EQUAL(testPool.Size(), 0UL);

    // Add children and grandchildren, but NOT the parent (simulate the parent
    // being in a block)
    for (int i = 0; i < 3; i++) {
        testPool.AddUnchecked(txChild[i].GetId(), entry.FromTx(txChild[i]), nullChangeSet);
        testPool.AddUnchecked(txGrandChild[i].GetId(),
                              entry.FromTx(txGrandChild[i]), nullChangeSet);
    }

    // Now remove the parent, as might happen if a block-re-org occurs but the
    // parent cannot be put into the mempool (maybe because it is non-standard):
    poolSize = testPool.Size();
    testPool.RemoveRecursive(CTransaction(txParent), nullChangeSet);
    BOOST_CHECK_EQUAL(testPool.Size(), poolSize - 6);
    BOOST_CHECK_EQUAL(testPool.Size(), 0UL);
}

BOOST_AUTO_TEST_CASE(MempoolClearTest) {
    // Test CTxMemPool::clear functionality

    TestMemPoolEntryHelper entry;
    // Create a transaction
    CMutableTransaction txParent;
    txParent.vin.resize(1);
    txParent.vin[0].scriptSig = CScript() << OP_11;
    txParent.vout.resize(3);
    for (int i = 0; i < 3; i++) {
        txParent.vout[i].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
        txParent.vout[i].nValue = Amount(33000LL);
    }

    CTxMemPool testPool;

    // Nothing in pool, clear should do nothing:
    testPool.Clear();
    BOOST_CHECK_EQUAL(testPool.Size(), 0UL);

    // Add the transaction
    testPool.AddUnchecked(txParent.GetId(), entry.FromTx(txParent), nullChangeSet);
    BOOST_CHECK_EQUAL(testPool.Size(), 1UL);
    BOOST_CHECK_EQUAL(testPool.mapTx.size(), 1UL);
    BOOST_CHECK_EQUAL(testPool.mapNextTx.size(), 1UL);
    BOOST_CHECK_EQUAL(testPool.vTxHashes.size(), 1UL);

    // CTxMemPool's members should be empty after a clear
    testPool.Clear();
    BOOST_CHECK_EQUAL(testPool.Size(), 0UL);
    BOOST_CHECK_EQUAL(testPool.mapTx.size(), 0UL);
    BOOST_CHECK_EQUAL(testPool.mapNextTx.size(), 0UL);
    BOOST_CHECK_EQUAL(testPool.vTxHashes.size(), 0UL);
}

// Regression test for the P0 "mis-ordered block template" bug.
//
// RemoveForBlock only refreshes survivors' ancestorsHeight, not their
// nCountWithAncestors. After a block is mined, a surviving descendant keeps a
// stale (inflated) nCountWithAncestors that still counts the just-removed
// ancestors, while a transaction added afterwards gets a correct (lower) count.
// When such a parent/child pair are later pulled into the journal together (here
// via CPFP), topo-sorting by nCountWithAncestors places the child before the
// parent, producing a journal/block template that is not topologically valid.
//
// The journal must instead be sorted by insertionIndex, which is assigned once at
// admission (parents always before children) and never goes stale.
BOOST_AUTO_TEST_CASE(MempoolJournalTopoOrderAfterRemoveForBlock) {
    CTxMemPool pool;
    TestMemPoolEntryHelper entry;

    // Require a positive fee for journal acceptance so that zero-fee
    // transactions stay out of the journal until a paying descendant covers
    // their debt (the CPFP path that performs the topo sort).
    pool.SetBlockMinTxFee(CFeeRate(Amount(1000)));

    auto chainChild = [](const CTransaction& parent) {
        CMutableTransaction tx;
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint(parent.GetId(), 0);
        tx.vin[0].scriptSig = CScript() << OP_11;
        tx.vout.resize(1);
        tx.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
        tx.vout[0].nValue = 10 * COIN;
        return tx;
    };

    // Root of the chain: r1 -> r2 -> p. r1/r2 are mined away to inflate p's
    // stale ancestor count by two.
    CMutableTransaction r1;
    r1.vin.resize(1);
    r1.vin[0].prevout = COutPoint(InsecureRand256(), 0);
    r1.vin[0].scriptSig = CScript() << OP_11;
    r1.vout.resize(1);
    r1.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    r1.vout[0].nValue = 10 * COIN;
    pool.AddUnchecked(r1.GetId(), entry.Fee(Amount(0LL)).FromTx(r1), nullChangeSet);

    CMutableTransaction r2 = chainChild(CTransaction(r1));
    pool.AddUnchecked(r2.GetId(), entry.Fee(Amount(0LL)).FromTx(r2), nullChangeSet);

    CMutableTransaction p = chainChild(CTransaction(r2));
    pool.AddUnchecked(p.GetId(), entry.Fee(Amount(0LL)).FromTx(p), nullChangeSet);

    // Mine r1 and r2. p survives; its nCountWithAncestors is now stale (3),
    // while its true ancestor count is 1.
    std::vector<CTransactionRef> vtx{MakeTransactionRef(r1), MakeTransactionRef(r2)};
    std::vector<CTransactionRef> txNew;
    pool.RemoveForBlock(vtx, nullChangeSet, txNew);
    BOOST_CHECK_EQUAL(pool.Size(), 1UL);

    // c spends p and is added afterwards: its ancestor count is computed fresh
    // (2) and so is *smaller* than its stale parent p (3).
    CMutableTransaction c = chainChild(CTransaction(p));
    pool.AddUnchecked(c.GetId(), entry.Fee(Amount(0LL)).FromTx(c), nullChangeSet);

    // d spends c and pays generously, covering the debt of both p and c, which
    // pulls them into the journal together (CPFP) and triggers the topo sort.
    CMutableTransaction d = chainChild(CTransaction(c));
    pool.AddUnchecked(d.GetId(), entry.Fee(100 * COIN).FromTx(d), nullChangeSet);

    using mining::CJournalEntry;
    using mining::CJournalPtr;
    using mining::CJournalTester;
    CJournalPtr journal{pool.getJournalBuilder().getCurrentJournal()};

    CJournalEntry pEntry{*pool.mapTx.find(p.GetId())};
    CJournalEntry cEntry{*pool.mapTx.find(c.GetId())};
    CJournalEntry dEntry{*pool.mapTx.find(d.GetId())};

    // The CPFP pull-in must place the whole chain in the journal...
    BOOST_CHECK(CJournalTester{journal}.checkTxnExists(pEntry));
    BOOST_CHECK(CJournalTester{journal}.checkTxnExists(cEntry));
    BOOST_CHECK(CJournalTester{journal}.checkTxnExists(dEntry));

    // ...in valid topological order: parent before child. With the stale-count
    // sort this fails (c is ordered before its parent p).
    BOOST_CHECK(CJournalTester{journal}.checkTxnOrdering(pEntry, cEntry) ==
                CJournalTester::TxnOrder::BEFORE);
    BOOST_CHECK(CJournalTester{journal}.checkTxnOrdering(cEntry, dEntry) ==
                CJournalTester::TxnOrder::BEFORE);
}

// Regression test for the reorg insertionIndex fixup (Phase 4).
//
// A reorg re-adds previously-confirmed transactions and can re-add a parent
// after a child that is already in the mempool, leaving the parent with a larger
// (later) insertionIndex than its child -- which would invert the topological
// order everything now derives from insertionIndex. UpdateTransactionsFromBlock
// must restore a topological insertionIndex over the affected component.
BOOST_AUTO_TEST_CASE(MempoolReorgReassignsInsertionIndex) {
    CTxMemPool pool;
    TestMemPoolEntryHelper entry;

    CMutableTransaction p;
    p.vin.resize(1);
    p.vin[0].prevout = COutPoint(InsecureRand256(), 0);
    p.vin[0].scriptSig = CScript() << OP_11;
    p.vout.resize(1);
    p.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    p.vout[0].nValue = 10 * COIN;

    CMutableTransaction c;
    c.vin.resize(1);
    c.vin[0].prevout = COutPoint(p.GetId(), 0);
    c.vin[0].scriptSig = CScript() << OP_11;
    c.vout.resize(1);
    c.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    c.vout[0].nValue = 9 * COIN;

    // Child enters the mempool first (its parent is still confirmed at this
    // point), then the parent is re-added -- mimicking a block disconnect that
    // re-adds the parent underneath an existing child.
    pool.AddUnchecked(c.GetId(), entry.Fee(Amount(10000LL)).FromTx(c), nullChangeSet);
    pool.AddUnchecked(p.GetId(), entry.Fee(Amount(10000LL)).FromTx(p), nullChangeSet);

    auto pIdx = [&] { return pool.mapTx.find(p.GetId())->GetInsertionIndex(); };
    auto cIdx = [&] { return pool.mapTx.find(c.GetId())->GetInsertionIndex(); };

    // Before fixup the parent has the larger (later) insertionIndex.
    BOOST_CHECK(cIdx() < pIdx());

    std::vector<uint256> toUpdate{p.GetId()};
    pool.UpdateTransactionsFromBlock(toUpdate, nullChangeSet);

    // After fixup the parent precedes the child topologically.
    BOOST_CHECK(pIdx() < cIdx());
}

BOOST_AUTO_TEST_CASE(MempoolAdmissionFeeCurveTest) {
    // The mempool is never trimmed; admission is bounded by a size-dependent
    // fee floor (CTxMemPool::GetMinFee). Below the ramp start (N1) the floor is
    // returned verbatim; between N1 and N2 the required feerate rises
    // hyperbolically with a pole at N2; at/above N2 it clamps to the ceiling.
    CTxMemPool pool;
    TestMemPoolEntryHelper entry;

    // Add a couple of transactions so the pool has a non-trivial memory usage.
    CMutableTransaction tx1 = CMutableTransaction();
    tx1.vin.resize(1);
    tx1.vin[0].scriptSig = CScript() << OP_1;
    tx1.vout.resize(1);
    tx1.vout[0].scriptPubKey = CScript() << OP_1 << OP_EQUAL;
    tx1.vout[0].nValue = 10 * COIN;
    pool.AddUnchecked(tx1.GetId(),
                      entry.Fee(Amount(10000LL)).FromTx(tx1, &pool), nullChangeSet);

    CMutableTransaction tx2 = CMutableTransaction();
    tx2.vin.resize(1);
    tx2.vin[0].scriptSig = CScript() << OP_2;
    tx2.vout.resize(1);
    tx2.vout[0].scriptPubKey = CScript() << OP_2 << OP_EQUAL;
    tx2.vout[0].nValue = 10 * COIN;
    pool.AddUnchecked(tx2.GetId(),
                      entry.Fee(Amount(5000LL)).FromTx(tx2, &pool), nullChangeSet);

    const size_t usage = pool.DynamicMemoryUsage();
    BOOST_CHECK(usage > 0);

    GlobalConfig& config = GlobalConfig::GetConfig();
    const int64_t floorRate = 1000; // satoshis per kB
    std::string err;
    BOOST_CHECK(config.SetMempoolMinFeePerKB(floorRate, &err));

    // Below the ramp start (N1 > usage): the floor applies regardless of N2.
    BOOST_CHECK(config.SetMempoolFeeRampStart(usage + 1, &err));
    BOOST_CHECK_EQUAL(pool.GetMinFee(usage * 4).GetFeePerK(), Amount(floorRate));

    // On the ramp (N1 = 0): rate = floor * (N2 - N1) / (N2 - usage).
    BOOST_CHECK(config.SetMempoolFeeRampStart(0, &err));
    //   N2 = 2*usage -> floor * 2u / u = 2 * floor.
    BOOST_CHECK_EQUAL(pool.GetMinFee(usage * 2).GetFeePerK(),
                      Amount(2 * floorRate));
    //   N2 = 4*usage -> floor * 4u / 3u = 1333 (integer division, exact for any u).
    BOOST_CHECK_EQUAL(pool.GetMinFee(usage * 4).GetFeePerK(),
                      Amount(4 * floorRate / 3));

    // At/above the hard cap (N2 <= usage): clamp to the ceiling.
    BOOST_CHECK_EQUAL(pool.GetMinFee(usage).GetFeePerK(),
                      MAX_MEMPOOL_RAMP_FEE_RATE);

    // Inclusive lower boundary: usage == N1 is still the flat floor (not the
    // ramp), because the branch uses usage <= N1.
    BOOST_CHECK(config.SetMempoolFeeRampStart(usage, &err));
    BOOST_CHECK_EQUAL(pool.GetMinFee(usage * 2).GetFeePerK(), Amount(floorRate));

    // The hyperbolic ramp itself clamps when the computed rate would exceed the
    // ceiling. With the floor sitting at the ceiling, any ramp factor > 1
    // saturates: N1 = 0, N2 = 2*usage -> floor * 2 -> clamped back to the cap.
    BOOST_CHECK(config.SetMempoolFeeRampStart(0, &err));
    BOOST_CHECK(config.SetMempoolMinFeePerKB(
        MAX_MEMPOOL_RAMP_FEE_RATE.GetSatoshis(), &err));
    BOOST_CHECK_EQUAL(pool.GetMinFee(usage * 2).GetFeePerK(),
                      MAX_MEMPOOL_RAMP_FEE_RATE);
    BOOST_CHECK(config.SetMempoolMinFeePerKB(floorRate, &err)); // restore floor

    // Degenerate config (N2 <= N1) is treated as flat even when usage > N1: the
    // floor is returned rather than falling through to the ramp/clamp branches.
    BOOST_CHECK(config.SetMempoolFeeRampStart(usage / 2, &err));
    BOOST_CHECK(usage / 2 < usage);                          // usage is above N1
    BOOST_CHECK_EQUAL(pool.GetMinFee(usage / 4).GetFeePerK(), // and N2 <= N1
                      Amount(floorRate));

    // Restore policy defaults so other tests are unaffected.
    BOOST_CHECK(config.SetMempoolMinFeePerKB(
        DEFAULT_MEMPOOL_MIN_FEE_RATE.GetSatoshis(), &err));
    BOOST_CHECK(config.SetMempoolFeeRampStart(
        DEFAULT_MEMPOOL_FEE_RAMP_START * ONE_MEGABYTE, &err));
}

BOOST_AUTO_TEST_CASE(CTxPrioritizerTest) {
    TestMemPoolEntryHelper entry;
    // Create a transaction
    CMutableTransaction txParent;
    txParent.vin.resize(1);
    txParent.vin[0].scriptSig = CScript() << OP_11;
    txParent.vout.resize(3);
    for (int i = 0; i < 3; i++) {
        txParent.vout[i].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
        txParent.vout[i].nValue = Amount(33000LL);
    }

    CTxMemPool testPool;
    const TxId& txid = txParent.GetId();
    // Add txn to the testPool
    {
        BOOST_CHECK_EQUAL(testPool.Size(), 0UL);
        testPool.AddUnchecked(txid, entry.FromTx(txParent), nullChangeSet);
        BOOST_CHECK_EQUAL(testPool.Size(), 1UL);
        BOOST_CHECK(!testPool.mapDeltas.count(txid));
    }
    // Instantiate txPrioritizer to prioritise txParent.
    {
        CTxPrioritizer txPrioritizer(testPool, txid);
        // This should add a new entry into mapDeltas.
        BOOST_CHECK(testPool.mapDeltas.count(txid));
        BOOST_CHECK_EQUAL(testPool.mapDeltas[txid].first, 0UL);
        BOOST_CHECK_EQUAL(testPool.mapDeltas[txid].second, MAX_MONEY);
        // Remove txid from the mapTx.
        testPool.mapTx.erase(txid);
    }
    // During txPrioritizer's destruction txid should be removed from mapDeltas.
    BOOST_CHECK(!testPool.mapDeltas.count(txid));
}

BOOST_AUTO_TEST_SUITE_END()
