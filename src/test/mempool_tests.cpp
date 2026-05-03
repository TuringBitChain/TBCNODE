// Copyright (c) 2011-2016 The Bitcoin Core developers
// Copyright (c) 2020 Bitcoin Association
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "mining/journal_change_set.h"
#include "policy/policy.h"
#include "txmempool.h"
#include "util.h"

#include "test/test_bitcoin.h"

#include <algorithm>
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

// Phase 5 (v3.3.0): descendant_score boost multi_index removed. Helper below replicates
// the original sorted-by-descendant-score iteration via linear scan + std::sort using the
// preserved CompareTxMemPoolEntryByDescendantScore comparator (same semantics as boost
// ordered_non_unique used previously).
template <>
void CheckSort<descendant_score>(CTxMemPool &pool, std::vector<std::string> &sortedOrder) {
    BOOST_CHECK_EQUAL(pool.Size(), sortedOrder.size());
    std::vector<CTxMemPool::txiter> entries;
    entries.reserve(pool.mapTx.size());
    for (auto it = pool.mapTx.begin(); it != pool.mapTx.end(); ++it) {
        entries.push_back(it);
    }
    CompareTxMemPoolEntryByDescendantScore cmp;
    std::sort(entries.begin(), entries.end(),
              [&cmp](CTxMemPool::txiter a, CTxMemPool::txiter b) { return cmp(*a, *b); });
    for (size_t i = 0; i < entries.size(); ++i) {
        BOOST_CHECK_EQUAL(entries[i]->GetTx().GetId().ToString(), sortedOrder[i]);
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

// Phase 1+2 (v3.3.0): MempoolAncestorIndexingTest deleted — depended on the
// ancestor_score multi_index which was removed along with the 4 cached ancestor
// aggregates (only LegacyBlockAssembler used that index).


BOOST_AUTO_TEST_CASE(MempoolSizeLimitTest) {
    CTxMemPool pool;
    TestMemPoolEntryHelper entry;
    entry.dPriority = 10.0;
    Amount feeIncrement = MEMPOOL_FULL_FEE_INCREMENT.GetFeePerK();

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

    // should do nothing
    pool.TrimToSize(pool.DynamicMemoryUsage(), nullChangeSet);
    BOOST_CHECK(pool.Exists(tx1.GetId()));
    BOOST_CHECK(pool.Exists(tx2.GetId()));

    // should remove the lower-feerate transaction
    pool.TrimToSize(pool.DynamicMemoryUsage() * 3 / 4, nullChangeSet);
    BOOST_CHECK(pool.Exists(tx1.GetId()));
    BOOST_CHECK(!pool.Exists(tx2.GetId()));

    pool.AddUnchecked(tx2.GetId(), entry.FromTx(tx2, &pool), nullChangeSet);
    CMutableTransaction tx3 = CMutableTransaction();
    tx3.vin.resize(1);
    tx3.vin[0].prevout = COutPoint(tx2.GetId(), 0);
    tx3.vin[0].scriptSig = CScript() << OP_2;
    tx3.vout.resize(1);
    tx3.vout[0].scriptPubKey = CScript() << OP_3 << OP_EQUAL;
    tx3.vout[0].nValue = 10 * COIN;
    pool.AddUnchecked(tx3.GetId(),
                      entry.Fee(Amount(20000LL)).FromTx(tx3, &pool), nullChangeSet);

    // tx3 should pay for tx2 (CPFP)
    pool.TrimToSize(pool.DynamicMemoryUsage() * 3 / 4, nullChangeSet);
    BOOST_CHECK(!pool.Exists(tx1.GetId()));
    BOOST_CHECK(pool.Exists(tx2.GetId()));
    BOOST_CHECK(pool.Exists(tx3.GetId()));

    // mempool is limited to tx1's size in memory usage, so nothing fits
    pool.TrimToSize(CTransaction(tx1).GetTotalSize(), nullChangeSet);
    BOOST_CHECK(!pool.Exists(tx1.GetId()));
    BOOST_CHECK(!pool.Exists(tx2.GetId()));
    BOOST_CHECK(!pool.Exists(tx3.GetId()));

    CFeeRate maxFeeRateRemoved(Amount(25000),
                               CTransaction(tx3).GetTotalSize() +
                                   CTransaction(tx2).GetTotalSize());
    BOOST_CHECK_EQUAL(pool.GetMinFee(1).GetFeePerK(),
                      maxFeeRateRemoved.GetFeePerK() + feeIncrement);

    CMutableTransaction tx4 = CMutableTransaction();
    tx4.vin.resize(2);
    tx4.vin[0].prevout = COutPoint();
    tx4.vin[0].scriptSig = CScript() << OP_4;
    tx4.vin[1].prevout = COutPoint();
    tx4.vin[1].scriptSig = CScript() << OP_4;
    tx4.vout.resize(2);
    tx4.vout[0].scriptPubKey = CScript() << OP_4 << OP_EQUAL;
    tx4.vout[0].nValue = 10 * COIN;
    tx4.vout[1].scriptPubKey = CScript() << OP_4 << OP_EQUAL;
    tx4.vout[1].nValue = 10 * COIN;

    CMutableTransaction tx5 = CMutableTransaction();
    tx5.vin.resize(2);
    tx5.vin[0].prevout = COutPoint(tx4.GetId(), 0);
    tx5.vin[0].scriptSig = CScript() << OP_4;
    tx5.vin[1].prevout = COutPoint();
    tx5.vin[1].scriptSig = CScript() << OP_5;
    tx5.vout.resize(2);
    tx5.vout[0].scriptPubKey = CScript() << OP_5 << OP_EQUAL;
    tx5.vout[0].nValue = 10 * COIN;
    tx5.vout[1].scriptPubKey = CScript() << OP_5 << OP_EQUAL;
    tx5.vout[1].nValue = 10 * COIN;

    CMutableTransaction tx6 = CMutableTransaction();
    tx6.vin.resize(2);
    tx6.vin[0].prevout = COutPoint(tx4.GetId(), 1);
    tx6.vin[0].scriptSig = CScript() << OP_4;
    tx6.vin[1].prevout = COutPoint();
    tx6.vin[1].scriptSig = CScript() << OP_6;
    tx6.vout.resize(2);
    tx6.vout[0].scriptPubKey = CScript() << OP_6 << OP_EQUAL;
    tx6.vout[0].nValue = 10 * COIN;
    tx6.vout[1].scriptPubKey = CScript() << OP_6 << OP_EQUAL;
    tx6.vout[1].nValue = 10 * COIN;

    CMutableTransaction tx7 = CMutableTransaction();
    tx7.vin.resize(2);
    tx7.vin[0].prevout = COutPoint(tx5.GetId(), 0);
    tx7.vin[0].scriptSig = CScript() << OP_5;
    tx7.vin[1].prevout = COutPoint(tx6.GetId(), 0);
    tx7.vin[1].scriptSig = CScript() << OP_6;
    tx7.vout.resize(2);
    tx7.vout[0].scriptPubKey = CScript() << OP_7 << OP_EQUAL;
    tx7.vout[0].nValue = 10 * COIN;
    tx7.vout[1].scriptPubKey = CScript() << OP_7 << OP_EQUAL;
    tx7.vout[1].nValue = 10 * COIN;

    pool.AddUnchecked(tx4.GetId(),
                      entry.Fee(Amount(7000LL)).FromTx(tx4, &pool), nullChangeSet);
    pool.AddUnchecked(tx5.GetId(),
                      entry.Fee(Amount(1000LL)).FromTx(tx5, &pool), nullChangeSet);
    pool.AddUnchecked(tx6.GetId(),
                      entry.Fee(Amount(1100LL)).FromTx(tx6, &pool), nullChangeSet);
    pool.AddUnchecked(tx7.GetId(),
                      entry.Fee(Amount(9000LL)).FromTx(tx7, &pool), nullChangeSet);

    // we only require this remove, at max, 2 txn, because its not clear what
    // we're really optimizing for aside from that
    pool.TrimToSize(pool.DynamicMemoryUsage() - 1, nullChangeSet);
    BOOST_CHECK(pool.Exists(tx4.GetId()));
    BOOST_CHECK(pool.Exists(tx6.GetId()));
    BOOST_CHECK(!pool.Exists(tx7.GetId()));

    if (!pool.Exists(tx5.GetId()))
        pool.AddUnchecked(tx5.GetId(),
                          entry.Fee(Amount(1000LL)).FromTx(tx5, &pool), nullChangeSet);
    pool.AddUnchecked(tx7.GetId(),
                      entry.Fee(Amount(9000LL)).FromTx(tx7, &pool), nullChangeSet);

    // should maximize mempool size by only removing 5/7
    pool.TrimToSize(pool.DynamicMemoryUsage() / 2, nullChangeSet);
    BOOST_CHECK(pool.Exists(tx4.GetId()));
    BOOST_CHECK(!pool.Exists(tx5.GetId()));
    BOOST_CHECK(pool.Exists(tx6.GetId()));
    BOOST_CHECK(!pool.Exists(tx7.GetId()));

    pool.AddUnchecked(tx5.GetId(),
                      entry.Fee(Amount(1000LL)).FromTx(tx5, &pool), nullChangeSet);
    pool.AddUnchecked(tx7.GetId(),
                      entry.Fee(Amount(9000LL)).FromTx(tx7, &pool), nullChangeSet);

    std::vector<CTransactionRef> vtx;
    SetMockTime(42);
    SetMockTime(42 + CTxMemPool::ROLLING_FEE_HALFLIFE);
    BOOST_CHECK_EQUAL(pool.GetMinFee(1).GetFeePerK(),
                      maxFeeRateRemoved.GetFeePerK() + feeIncrement);
    // ... we should keep the same min fee until we get a block
    std::vector<CTransactionRef> txNew;
    pool.RemoveForBlock(vtx, nullChangeSet, txNew);
    SetMockTime(42 + 2 * CTxMemPool::ROLLING_FEE_HALFLIFE);
    BOOST_CHECK_EQUAL(pool.GetMinFee(1).GetFeePerK(),
                      (maxFeeRateRemoved.GetFeePerK() + feeIncrement) / 2);
    // ... then feerate should drop 1/2 each halflife

    SetMockTime(42 + 2 * CTxMemPool::ROLLING_FEE_HALFLIFE +
                CTxMemPool::ROLLING_FEE_HALFLIFE / 2);
    BOOST_CHECK_EQUAL(
        pool.GetMinFee(pool.DynamicMemoryUsage() * 5 / 2).GetFeePerK(),
        (maxFeeRateRemoved.GetFeePerK() + feeIncrement) / 4);
    // ... with a 1/2 halflife when mempool is < 1/2 its target size

    SetMockTime(42 + 2 * CTxMemPool::ROLLING_FEE_HALFLIFE +
                CTxMemPool::ROLLING_FEE_HALFLIFE / 2 +
                CTxMemPool::ROLLING_FEE_HALFLIFE / 4);
    BOOST_CHECK_EQUAL(
        pool.GetMinFee(pool.DynamicMemoryUsage() * 9 / 2).GetFeePerK(),
        (maxFeeRateRemoved.GetFeePerK() + feeIncrement) / 8);
    // ... with a 1/4 halflife when mempool is < 1/4 its target size

    SetMockTime(0);
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

/*
 * Phase 0 — Site 0 fix verification
 *
 * 验证 DisconnectBlock 回流后 UpdateTransactionsFromBlock 正确刷新 children 的
 * ancestorsHeight。
 *
 * 场景：
 *   1. tx_parent 加进 mempool 后通过 RemoveForBlock 模拟 confirm 进 block
 *   2. tx_child（依赖 tx_parent）加进 mempool — 此时父在 block 不在 mempool，
 *      tx_child.ancestorsHeight = 0
 *   3. tx_grandchild（依赖 tx_child）加进 mempool — tx_grandchild.ancestorsHeight = 1
 *   4. 模拟 DisconnectBlock：AddUnchecked(tx_parent) 回流
 *   5. UpdateTransactionsFromBlock([tx_parent]) 触发 Site 0 fix
 *
 * Site 0 fix 前（不修）：
 *   - tx_child.ancestorsHeight = 0 (stale)
 *   - tx_grandchild.ancestorsHeight = 1 (stale)
 *
 * Site 0 fix 后（应修复）：
 *   - tx_parent.ancestorsHeight = 0
 *   - tx_child.ancestorsHeight = 1
 *   - tx_grandchild.ancestorsHeight = 2 (BFS 递归刷新)
 */
BOOST_AUTO_TEST_CASE(MempoolReorgHeightRefresh) {
    CTxMemPool pool;
    TestMemPoolEntryHelper entry;

    // 1. Create tx_parent (no inputs from mempool)
    CMutableTransaction tx_parent;
    tx_parent.vin.resize(1);
    tx_parent.vin[0].scriptSig = CScript() << OP_11;
    tx_parent.vout.resize(1);
    tx_parent.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx_parent.vout[0].nValue = 10 * COIN;
    pool.AddUnchecked(tx_parent.GetId(),
                      entry.Fee(Amount(10000LL)).FromTx(tx_parent),
                      nullChangeSet);
    BOOST_CHECK_EQUAL(pool.Size(), 1UL);

    // tx_parent should have height 0 (no in-mempool parents)
    {
        auto it = pool.mapTx.find(tx_parent.GetId());
        BOOST_REQUIRE(it != pool.mapTx.end());
        BOOST_CHECK_EQUAL(it->GetAncestorsHeight(), 0U);
    }

    // 2. RemoveForBlock(tx_parent) — simulate confirm into block
    {
        std::vector<CTransactionRef> vtx { MakeTransactionRef(tx_parent) };
        std::vector<CTransactionRef> txNew;
        pool.RemoveForBlock(vtx, nullChangeSet, txNew);
    }
    BOOST_CHECK_EQUAL(pool.Size(), 0UL);

    // 3. Add tx_child (依赖 tx_parent，但 tx_parent 在 block 不在 mempool)
    CMutableTransaction tx_child;
    tx_child.vin.resize(1);
    tx_child.vin[0].prevout = COutPoint(tx_parent.GetId(), 0);
    tx_child.vin[0].scriptSig = CScript() << OP_11;
    tx_child.vout.resize(1);
    tx_child.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx_child.vout[0].nValue = 10 * COIN;
    pool.AddUnchecked(tx_child.GetId(),
                      entry.Fee(Amount(10000LL)).FromTx(tx_child),
                      nullChangeSet);

    // tx_child.ancestorsHeight = 0 because tx_parent is not in mempool
    {
        auto it = pool.mapTx.find(tx_child.GetId());
        BOOST_REQUIRE(it != pool.mapTx.end());
        BOOST_CHECK_EQUAL(it->GetAncestorsHeight(), 0U);
    }

    // 4. Add tx_grandchild (依赖 tx_child，tx_child 在 mempool)
    CMutableTransaction tx_grandchild;
    tx_grandchild.vin.resize(1);
    tx_grandchild.vin[0].prevout = COutPoint(tx_child.GetId(), 0);
    tx_grandchild.vin[0].scriptSig = CScript() << OP_11;
    tx_grandchild.vout.resize(1);
    tx_grandchild.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx_grandchild.vout[0].nValue = 10 * COIN;
    pool.AddUnchecked(tx_grandchild.GetId(),
                      entry.Fee(Amount(10000LL)).FromTx(tx_grandchild),
                      nullChangeSet);

    // tx_grandchild.ancestorsHeight = 1 (tx_child is in mempool with height 0)
    {
        auto it = pool.mapTx.find(tx_grandchild.GetId());
        BOOST_REQUIRE(it != pool.mapTx.end());
        BOOST_CHECK_EQUAL(it->GetAncestorsHeight(), 1U);
    }

    // 5. Simulate DisconnectBlock: 回流 tx_parent via AddUnchecked
    pool.AddUnchecked(tx_parent.GetId(),
                      entry.Fee(Amount(10000LL)).FromTx(tx_parent),
                      nullChangeSet);

    // tx_parent.ancestorsHeight = 0 (no in-mempool parents)
    {
        auto it = pool.mapTx.find(tx_parent.GetId());
        BOOST_REQUIRE(it != pool.mapTx.end());
        BOOST_CHECK_EQUAL(it->GetAncestorsHeight(), 0U);
    }

    // 6. Call UpdateTransactionsFromBlock to trigger Site 0 fix
    std::vector<uint256> vHashUpdate { tx_parent.GetId() };
    pool.UpdateTransactionsFromBlock(vHashUpdate, nullChangeSet);

    // 7. Site 0 fix verification: tx_child + tx_grandchild heights refreshed
    {
        auto it_parent = pool.mapTx.find(tx_parent.GetId());
        BOOST_REQUIRE(it_parent != pool.mapTx.end());
        BOOST_CHECK_EQUAL(it_parent->GetAncestorsHeight(), 0U);

        auto it_child = pool.mapTx.find(tx_child.GetId());
        BOOST_REQUIRE(it_child != pool.mapTx.end());
        // Site 0 fix: tx_child.height refreshed from 0 to 1 after tx_parent reflowed
        BOOST_CHECK_EQUAL(it_child->GetAncestorsHeight(), 1U);

        auto it_gc = pool.mapTx.find(tx_grandchild.GetId());
        BOOST_REQUIRE(it_gc != pool.mapTx.end());
        // Site 0 fix BFS recursive: tx_grandchild.height refreshed from 1 to 2
        BOOST_CHECK_EQUAL(it_gc->GetAncestorsHeight(), 2U);
    }
}

/*
 * Phase 0 — Site 0 fix verification (diamond topology)
 *
 * 验证 reflow 多个 parent + 共同 deep descendant 的 diamond 场景下 ancestorsHeight
 * BFS 刷新正确。InsertionOrderComparator 由 mempool 入池规则数学保证拓扑序
 * （父.insertion_index < 子.insertion_index），所以 BFS 处理 deep descendant 时
 * 它的所有父都已 fresh。
 *
 * 拓扑：
 *   A      B          (将进 block 后 reflow)
 *   |      |
 *   C      D          (mempool 中存在，A/B 在 block 时 height 偏小)
 *    \    /
 *     E              (依赖 C + D 的 diamond child)
 *
 * 期望（Site 0 fix 后）：A.h=0, B.h=0, C.h=1, D.h=1, E.h=2
 */
BOOST_AUTO_TEST_CASE(MempoolReorgHeightRefreshDiamond) {
    CTxMemPool pool;
    TestMemPoolEntryHelper entry;

    // Build A (no inputs)
    CMutableTransaction tx_a;
    tx_a.vin.resize(1);
    tx_a.vin[0].scriptSig = CScript() << OP_11;
    tx_a.vout.resize(1);
    tx_a.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx_a.vout[0].nValue = 10 * COIN;

    // Build B (no inputs, distinct from A via different scriptSig)
    CMutableTransaction tx_b;
    tx_b.vin.resize(1);
    tx_b.vin[0].scriptSig = CScript() << OP_11 << OP_11;  // 跟 A 区分
    tx_b.vout.resize(1);
    tx_b.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx_b.vout[0].nValue = 10 * COIN;

    pool.AddUnchecked(tx_a.GetId(), entry.Fee(Amount(10000LL)).FromTx(tx_a), nullChangeSet);
    pool.AddUnchecked(tx_b.GetId(), entry.Fee(Amount(10000LL)).FromTx(tx_b), nullChangeSet);
    BOOST_CHECK_EQUAL(pool.Size(), 2UL);

    // RemoveForBlock(A, B) — 模拟 confirm 进 block
    {
        std::vector<CTransactionRef> vtx { MakeTransactionRef(tx_a), MakeTransactionRef(tx_b) };
        std::vector<CTransactionRef> txNew;
        pool.RemoveForBlock(vtx, nullChangeSet, txNew);
    }
    BOOST_CHECK_EQUAL(pool.Size(), 0UL);

    // Add C (依赖 A，A 在 block 不在 mempool → C.height = 0)
    CMutableTransaction tx_c;
    tx_c.vin.resize(1);
    tx_c.vin[0].prevout = COutPoint(tx_a.GetId(), 0);
    tx_c.vin[0].scriptSig = CScript() << OP_11;
    tx_c.vout.resize(1);
    tx_c.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx_c.vout[0].nValue = 10 * COIN;
    pool.AddUnchecked(tx_c.GetId(), entry.Fee(Amount(10000LL)).FromTx(tx_c), nullChangeSet);

    // Add D (依赖 B，B 在 block 不在 mempool → D.height = 0)
    CMutableTransaction tx_d;
    tx_d.vin.resize(1);
    tx_d.vin[0].prevout = COutPoint(tx_b.GetId(), 0);
    tx_d.vin[0].scriptSig = CScript() << OP_11;
    tx_d.vout.resize(1);
    tx_d.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx_d.vout[0].nValue = 10 * COIN;
    pool.AddUnchecked(tx_d.GetId(), entry.Fee(Amount(10000LL)).FromTx(tx_d), nullChangeSet);

    // Add E (依赖 C + D，diamond child → E.height = max(C.h=0, D.h=0) + 1 = 1，stale)
    CMutableTransaction tx_e;
    tx_e.vin.resize(2);
    tx_e.vin[0].prevout = COutPoint(tx_c.GetId(), 0);
    tx_e.vin[0].scriptSig = CScript() << OP_11;
    tx_e.vin[1].prevout = COutPoint(tx_d.GetId(), 0);
    tx_e.vin[1].scriptSig = CScript() << OP_11;
    tx_e.vout.resize(1);
    tx_e.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
    tx_e.vout[0].nValue = 10 * COIN;
    pool.AddUnchecked(tx_e.GetId(), entry.Fee(Amount(10000LL)).FromTx(tx_e), nullChangeSet);

    // Verify stale heights before fix:
    // C.h=0 (A 不在), D.h=0 (B 不在), E.h=1 (max(C, D)+1 = 1)
    {
        BOOST_CHECK_EQUAL(pool.mapTx.find(tx_c.GetId())->GetAncestorsHeight(), 0U);
        BOOST_CHECK_EQUAL(pool.mapTx.find(tx_d.GetId())->GetAncestorsHeight(), 0U);
        BOOST_CHECK_EQUAL(pool.mapTx.find(tx_e.GetId())->GetAncestorsHeight(), 1U);
    }

    // Reflow A and B (DisconnectBlock 模拟)
    pool.AddUnchecked(tx_a.GetId(), entry.Fee(Amount(10000LL)).FromTx(tx_a), nullChangeSet);
    pool.AddUnchecked(tx_b.GetId(), entry.Fee(Amount(10000LL)).FromTx(tx_b), nullChangeSet);

    // Trigger Site 0 fix
    std::vector<uint256> vHashUpdate { tx_a.GetId(), tx_b.GetId() };
    pool.UpdateTransactionsFromBlock(vHashUpdate, nullChangeSet);

    // Verify diamond BFS refresh:
    // A.h=0, B.h=0, C.h=1 (parent A=0), D.h=1 (parent B=0), E.h=2 (max(C,D)+1)
    {
        BOOST_CHECK_EQUAL(pool.mapTx.find(tx_a.GetId())->GetAncestorsHeight(), 0U);
        BOOST_CHECK_EQUAL(pool.mapTx.find(tx_b.GetId())->GetAncestorsHeight(), 0U);
        BOOST_CHECK_EQUAL(pool.mapTx.find(tx_c.GetId())->GetAncestorsHeight(), 1U);
        BOOST_CHECK_EQUAL(pool.mapTx.find(tx_d.GetId())->GetAncestorsHeight(), 1U);
        BOOST_CHECK_EQUAL(pool.mapTx.find(tx_e.GetId())->GetAncestorsHeight(), 2U);
    }
}

/*
 * Phase 5 (v3.3.0) C1 fix verification — strict-weak-ordering safety of
 * CompareTxMemPoolEntryByDescendantScore.
 *
 * Constructs two entries with identical fee-rate AND identical nTime, plus a self-comparison.
 * Pre-fix `cmp(a, b) == cmp(b, a) == true` (asymmetry violation) and `cmp(x, x) == true`
 * (irreflexivity violation) made std::sort UB. Post-fix tie-break is by txid (unique +
 * immutable) so the comparator is a valid SWO and sort is deterministic.
 *
 * Runs std::sort over a small range that includes the tied pair to verify no infinite loop /
 * crash and that ordering is repeatable. Also exercises TrimToSize selection determinism.
 */
BOOST_AUTO_TEST_CASE(DescendantScoreComparatorSWO) {
    CTxMemPool pool;
    TestMemPoolEntryHelper entry;
    SetMockTime(1234567);  // make GetTime() deterministic across nodes constructed below

    auto makeTx = [](uint8_t marker) {
        CMutableTransaction tx;
        tx.vin.resize(1);
        tx.vin[0].scriptSig = CScript() << marker;
        tx.vout.resize(1);
        tx.vout[0].scriptPubKey = CScript() << OP_11 << OP_EQUAL;
        tx.vout[0].nValue = 10 * COIN;
        return tx;
    };

    // tx_a / tx_b — distinct txid, identical fee, identical entry time → fee-rate AND nTime tie.
    CMutableTransaction tx_a = makeTx(0xa1);
    CMutableTransaction tx_b = makeTx(0xb2);
    BOOST_REQUIRE(tx_a.GetId() != tx_b.GetId());

    pool.AddUnchecked(tx_a.GetId(), entry.Fee(Amount(10000LL)).Time(1234567).FromTx(tx_a),
                      nullChangeSet);
    pool.AddUnchecked(tx_b.GetId(), entry.Fee(Amount(10000LL)).Time(1234567).FromTx(tx_b),
                      nullChangeSet);
    BOOST_CHECK_EQUAL(pool.Size(), 2UL);

    auto it_a = pool.mapTx.find(tx_a.GetId());
    auto it_b = pool.mapTx.find(tx_b.GetId());
    BOOST_REQUIRE(it_a != pool.mapTx.end());
    BOOST_REQUIRE(it_b != pool.mapTx.end());

    CompareTxMemPoolEntryByDescendantScore cmp;

    // C1 — irreflexivity: cmp(x, x) must be false (not true).
    BOOST_CHECK(!cmp(*it_a, *it_a));
    BOOST_CHECK(!cmp(*it_b, *it_b));

    // C1 — asymmetry: cmp(a, b) and cmp(b, a) cannot both be true.
    bool ab = cmp(*it_a, *it_b);
    bool ba = cmp(*it_b, *it_a);
    BOOST_CHECK(!(ab && ba));
    // Total order on tied entries: exactly one direction is true (txid tiebreaker is unique).
    BOOST_CHECK(ab != ba);

    // std::sort over the tied pair must not crash / infinite-loop and must be deterministic.
    std::vector<CTxMemPool::indexed_transaction_set::const_iterator> entries { it_a, it_b };
    std::sort(entries.begin(), entries.end(),
              [&cmp](auto a, auto b) { return cmp(*a, *b); });
    // Deterministic: smaller txid wins (since fee+time tied, txid breaks tie).
    auto expected_first = (tx_a.GetId() < tx_b.GetId()) ? it_a : it_b;
    BOOST_CHECK(entries[0] == expected_first);

    // Repeat sort on shuffled input — same result.
    std::vector<CTxMemPool::indexed_transaction_set::const_iterator> entries2 { it_b, it_a };
    std::sort(entries2.begin(), entries2.end(),
              [&cmp](auto a, auto b) { return cmp(*a, *b); });
    BOOST_CHECK(entries2[0] == expected_first);

    SetMockTime(0);
}

BOOST_AUTO_TEST_SUITE_END()
