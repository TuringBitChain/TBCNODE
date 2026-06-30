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

template <typename name>
void CheckSort(CTxMemPool &pool, std::vector<std::string> &sortedOrder) {
    BOOST_CHECK_EQUAL(pool.Size(), sortedOrder.size());
    typename CTxMemPool::indexed_transaction_set::index<name>::type::iterator
        it = pool.mapTx.get<name>().begin();
    int count = 0;
    for (; it != pool.mapTx.get<name>().end(); ++it, ++count) {
        BOOST_CHECK_EQUAL(it->GetTx().GetId().ToString(), sortedOrder[count]);
    }
}

BOOST_AUTO_TEST_CASE(MempoolIndexingTest) {
    CTxMemPool pool;
    TestMemPoolEntryHelper entry;

    /* 3rd highest fee */
    CMutableTransaction tx1 = CMutableTransaction();
    tx1.vout.resize(1);
    tx1.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx1.vout[0].nValue = 10 * COIN;
    pool.AddUnchecked(tx1.GetId(),
                      entry.Fee(Amount(10000LL)).Priority(10.0).FromTx(tx1), nullChangeSet);

    /* highest fee */
    CMutableTransaction tx2 = CMutableTransaction();
    tx2.vout.resize(1);
    tx2.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx2.vout[0].nValue = 2 * COIN;
    pool.AddUnchecked(tx2.GetId(),
                      entry.Fee(Amount(20000LL)).Priority(9.0).FromTx(tx2), nullChangeSet);

    /* lowest fee */
    CMutableTransaction tx3 = CMutableTransaction();
    tx3.vout.resize(1);
    tx3.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx3.vout[0].nValue = 5 * COIN;
    pool.AddUnchecked(tx3.GetId(),
                      entry.Fee(Amount(0LL)).Priority(100.0).FromTx(tx3), nullChangeSet);

    /* 2nd highest fee */
    CMutableTransaction tx4 = CMutableTransaction();
    tx4.vout.resize(1);
    tx4.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx4.vout[0].nValue = 6 * COIN;
    pool.AddUnchecked(tx4.GetId(),
                      entry.Fee(Amount(15000LL)).Priority(1.0).FromTx(tx4), nullChangeSet);

    /* equal fee rate to tx1, but newer */
    CMutableTransaction tx5 = CMutableTransaction();
    tx5.vout.resize(1);
    tx5.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx5.vout[0].nValue = 11 * COIN;
    entry.nTime = 1;
    entry.dPriority = 10.0;
    pool.AddUnchecked(tx5.GetId(), entry.Fee(Amount(10000LL)).FromTx(tx5), nullChangeSet);
    BOOST_CHECK_EQUAL(pool.Size(), 5UL);

    std::vector<std::string> sortedOrder;
    sortedOrder.resize(5);
    sortedOrder[0] = tx3.GetId().ToString(); // 0
    sortedOrder[1] = tx5.GetId().ToString(); // 10000
    sortedOrder[2] = tx1.GetId().ToString(); // 10000
    sortedOrder[3] = tx4.GetId().ToString(); // 15000
    sortedOrder[4] = tx2.GetId().ToString(); // 20000
    CheckSort<descendant_score>(pool, sortedOrder);

    /* low fee but with high fee child */
    /* tx6 -> tx7 -> tx8, tx9 -> tx10 */
    CMutableTransaction tx6 = CMutableTransaction();
    tx6.vout.resize(1);
    tx6.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx6.vout[0].nValue = 20 * COIN;
    pool.AddUnchecked(tx6.GetId(), entry.Fee(Amount(0LL)).FromTx(tx6), nullChangeSet);
    BOOST_CHECK_EQUAL(pool.Size(), 6UL);
    // Check that at this point, tx6 is sorted low
    sortedOrder.insert(sortedOrder.begin(), tx6.GetId().ToString());
    CheckSort<descendant_score>(pool, sortedOrder);

    CTxMemPool::setEntries setAncestors;
    setAncestors.insert(pool.mapTx.find(tx6.GetId()));
    CMutableTransaction tx7 = CMutableTransaction();
    tx7.vin.resize(1);
    tx7.vin[0].prevout = COutPoint(tx6.GetId(), 0);
    tx7.vin[0].scriptSig = CScript() << OP_11;
    tx7.vout.resize(2);
    tx7.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx7.vout[0].nValue = 10 * COIN;
    tx7.vout[1].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx7.vout[1].nValue = 1 * COIN;

    CTxMemPool::setEntries setAncestorsCalculated;
    std::string dummy;
    BOOST_CHECK_EQUAL(
        pool.CalculateMemPoolAncestors(entry.Fee(Amount(2000000LL)).FromTx(tx7),
                                       setAncestorsCalculated, 100, 1000000,
                                       1000, 1000000, dummy),
        true);
    BOOST_CHECK(setAncestorsCalculated == setAncestors);

    pool.AddUnchecked(tx7.GetId(), entry.FromTx(tx7), setAncestors, nullChangeSet);
    BOOST_CHECK_EQUAL(pool.Size(), 7UL);

    // Now tx6 should be sorted higher (high fee child): tx7, tx6, tx2, ...
    sortedOrder.erase(sortedOrder.begin());
    sortedOrder.push_back(tx6.GetId().ToString());
    sortedOrder.push_back(tx7.GetId().ToString());
    CheckSort<descendant_score>(pool, sortedOrder);

    /* low fee child of tx7 */
    CMutableTransaction tx8 = CMutableTransaction();
    tx8.vin.resize(1);
    tx8.vin[0].prevout = COutPoint(tx7.GetId(), 0);
    tx8.vin[0].scriptSig = CScript() << OP_11;
    tx8.vout.resize(1);
    tx8.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx8.vout[0].nValue = 10 * COIN;
    setAncestors.insert(pool.mapTx.find(tx7.GetId()));
    pool.AddUnchecked(tx8.GetId(), entry.Fee(Amount(0LL)).Time(2).FromTx(tx8),
                      setAncestors, nullChangeSet);

    // Now tx8 should be sorted low, but tx6/tx both high
    sortedOrder.insert(sortedOrder.begin(), tx8.GetId().ToString());
    CheckSort<descendant_score>(pool, sortedOrder);

    /* low fee child of tx7 */
    CMutableTransaction tx9 = CMutableTransaction();
    tx9.vin.resize(1);
    tx9.vin[0].prevout = COutPoint(tx7.GetId(), 1);
    tx9.vin[0].scriptSig = CScript() << OP_11;
    tx9.vout.resize(1);
    tx9.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx9.vout[0].nValue = 1 * COIN;
    pool.AddUnchecked(tx9.GetId(), entry.Fee(Amount(0LL)).Time(3).FromTx(tx9),
                      setAncestors, nullChangeSet);

    // tx9 should be sorted low
    BOOST_CHECK_EQUAL(pool.Size(), 9UL);
    sortedOrder.insert(sortedOrder.begin(), tx9.GetId().ToString());
    CheckSort<descendant_score>(pool, sortedOrder);

    std::vector<std::string> snapshotOrder = sortedOrder;

    setAncestors.insert(pool.mapTx.find(tx8.GetId()));
    setAncestors.insert(pool.mapTx.find(tx9.GetId()));
    /* tx10 depends on tx8 and tx9 and has a high fee*/
    CMutableTransaction tx10 = CMutableTransaction();
    tx10.vin.resize(2);
    tx10.vin[0].prevout = COutPoint(tx8.GetId(), 0);
    tx10.vin[0].scriptSig = CScript() << OP_11;
    tx10.vin[1].prevout = COutPoint(tx9.GetId(), 0);
    tx10.vin[1].scriptSig = CScript() << OP_11;
    tx10.vout.resize(1);
    tx10.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx10.vout[0].nValue = 10 * COIN;

    setAncestorsCalculated.clear();
    BOOST_CHECK_EQUAL(pool.CalculateMemPoolAncestors(
                          entry.Fee(Amount(200000LL)).Time(4).FromTx(tx10),
                          setAncestorsCalculated, 100, 1000000, 1000, 1000000,
                          dummy),
                      true);
    BOOST_CHECK(setAncestorsCalculated == setAncestors);

    pool.AddUnchecked(tx10.GetId(), entry.FromTx(tx10), setAncestors, nullChangeSet);

    /**
     *  tx8 and tx9 should both now be sorted higher
     *  Final order after tx10 is added:
     *
     *  tx3 = 0 (1)
     *  tx5 = 10000 (1)
     *  tx1 = 10000 (1)
     *  tx4 = 15000 (1)
     *  tx2 = 20000 (1)
     *  tx9 = 200k (2 txs)
     *  tx8 = 200k (2 txs)
     *  tx10 = 200k (1 tx)
     *  tx6 = 2.2M (5 txs)
     *  tx7 = 2.2M (4 txs)
     */
    // take out tx9, tx8 from the beginning
    sortedOrder.erase(sortedOrder.begin(), sortedOrder.begin() + 2);
    sortedOrder.insert(sortedOrder.begin() + 5, tx9.GetId().ToString());
    sortedOrder.insert(sortedOrder.begin() + 6, tx8.GetId().ToString());
    // tx10 is just before tx6
    sortedOrder.insert(sortedOrder.begin() + 7, tx10.GetId().ToString());
    CheckSort<descendant_score>(pool, sortedOrder);

    // there should be 10 transactions in the mempool
    BOOST_CHECK_EQUAL(pool.Size(), 10UL);

    // Now try removing tx10 and verify the sort order returns to normal
    pool.RemoveRecursive(pool.mapTx.find(tx10.GetId())->GetTx(), nullChangeSet);
    CheckSort<descendant_score>(pool, snapshotOrder);

    pool.RemoveRecursive(pool.mapTx.find(tx9.GetId())->GetTx(), nullChangeSet);
    pool.RemoveRecursive(pool.mapTx.find(tx8.GetId())->GetTx(), nullChangeSet);
}

BOOST_AUTO_TEST_CASE(MempoolAncestorIndexingTest) {
    CTxMemPool pool;
    TestMemPoolEntryHelper entry;

    /* 3rd highest fee */
    CMutableTransaction tx1 = CMutableTransaction();
    tx1.vout.resize(1);
    tx1.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx1.vout[0].nValue = 10 * COIN;
    pool.AddUnchecked(tx1.GetId(),
                      entry.Fee(Amount(10000LL)).Priority(10.0).FromTx(tx1), nullChangeSet);

    /* highest fee */
    CMutableTransaction tx2 = CMutableTransaction();
    tx2.vout.resize(1);
    tx2.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx2.vout[0].nValue = 2 * COIN;
    pool.AddUnchecked(tx2.GetId(),
                      entry.Fee(Amount(20000LL)).Priority(9.0).FromTx(tx2), nullChangeSet);
    uint64_t tx2Size = CTransaction(tx2).GetTotalSize();

    /* lowest fee */
    CMutableTransaction tx3 = CMutableTransaction();
    tx3.vout.resize(1);
    tx3.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx3.vout[0].nValue = 5 * COIN;
    pool.AddUnchecked(tx3.GetId(),
                      entry.Fee(Amount(0LL)).Priority(100.0).FromTx(tx3), nullChangeSet);

    /* 2nd highest fee */
    CMutableTransaction tx4 = CMutableTransaction();
    tx4.vout.resize(1);
    tx4.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx4.vout[0].nValue = 6 * COIN;
    pool.AddUnchecked(tx4.GetId(),
                      entry.Fee(Amount(15000LL)).Priority(1.0).FromTx(tx4), nullChangeSet);

    /* equal fee rate to tx1, but newer */
    CMutableTransaction tx5 = CMutableTransaction();
    tx5.vout.resize(1);
    tx5.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx5.vout[0].nValue = 11 * COIN;
    pool.AddUnchecked(tx5.GetId(), entry.Fee(Amount(10000LL)).FromTx(tx5), nullChangeSet);
    BOOST_CHECK_EQUAL(pool.Size(), 5UL);

    std::vector<std::string> sortedOrder;
    sortedOrder.resize(5);
    sortedOrder[0] = tx2.GetId().ToString(); // 20000
    sortedOrder[1] = tx4.GetId().ToString(); // 15000
    // tx1 and tx5 are both 10000
    // Ties are broken by hash, not timestamp, so determine which hash comes
    // first.
    if (tx1.GetId() < tx5.GetId()) {
        sortedOrder[2] = tx1.GetId().ToString();
        sortedOrder[3] = tx5.GetId().ToString();
    } else {
        sortedOrder[2] = tx5.GetId().ToString();
        sortedOrder[3] = tx1.GetId().ToString();
    }
    sortedOrder[4] = tx3.GetId().ToString(); // 0

    CheckSort<ancestor_score>(pool, sortedOrder);

    /* low fee parent with high fee child */
    /* tx6 (0) -> tx7 (high) */
    CMutableTransaction tx6 = CMutableTransaction();
    tx6.vout.resize(1);
    tx6.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx6.vout[0].nValue = 20 * COIN;
    uint64_t tx6Size = CTransaction(tx6).GetTotalSize();

    pool.AddUnchecked(tx6.GetId(), entry.Fee(Amount(0LL)).FromTx(tx6), nullChangeSet);
    BOOST_CHECK_EQUAL(pool.Size(), 6UL);
    // Ties are broken by hash
    if (tx3.GetId() < tx6.GetId()) {
        sortedOrder.push_back(tx6.GetId().ToString());
    } else {
        sortedOrder.insert(sortedOrder.end() - 1, tx6.GetId().ToString());
    }

    CheckSort<ancestor_score>(pool, sortedOrder);

    CMutableTransaction tx7 = CMutableTransaction();
    tx7.vin.resize(1);
    tx7.vin[0].prevout = COutPoint(tx6.GetId(), 0);
    tx7.vin[0].scriptSig = CScript() << OP_11;
    tx7.vout.resize(1);
    tx7.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx7.vout[0].nValue = 10 * COIN;
    uint64_t tx7Size = CTransaction(tx7).GetTotalSize();

    /* set the fee to just below tx2's feerate when including ancestor */
    Amount fee((20000 / tx2Size) * (tx7Size + tx6Size) - 1);

    // CTxMemPoolEntry entry7(tx7, fee, 2, 10.0, 1, true);
    pool.AddUnchecked(tx7.GetId(), entry.Fee(Amount(fee)).FromTx(tx7), nullChangeSet);
    BOOST_CHECK_EQUAL(pool.Size(), 7UL);
    sortedOrder.insert(sortedOrder.begin() + 1, tx7.GetId().ToString());
    CheckSort<ancestor_score>(pool, sortedOrder);

    /* after tx6 is mined, tx7 should move up in the sort */
    std::vector<CTransactionRef> vtx;
    vtx.push_back(MakeTransactionRef(tx6));
    std::vector<CTransactionRef> txNew;
    pool.RemoveForBlock(vtx, nullChangeSet, txNew);

    sortedOrder.erase(sortedOrder.begin() + 1);
    // Ties are broken by hash
    if (tx3.GetId() < tx6.GetId())
        sortedOrder.pop_back();
    else
        sortedOrder.erase(sortedOrder.end() - 2);
    sortedOrder.insert(sortedOrder.begin(), tx7.GetId().ToString());
    CheckSort<ancestor_score>(pool, sortedOrder);
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
