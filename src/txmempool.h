// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2019-2020 Bitcoin Association
// Distributed under the Open TBC software license, see the accompanying file LICENSE.

#ifndef BITCOIN_TXMEMPOOL_H
#define BITCOIN_TXMEMPOOL_H

#include "amount.h"
#include "coins.h"
#include "indirectmap.h"
#include "mining/journal_builder.h"
#include "random.h"
#include "sync.h"
#include "time_locked_mempool.h"
#include "tx_mempool_info.h"
#include "policy/policy.h"

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <boost/signals2/signal.hpp>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <mutex>
#include <shared_mutex>

class CAutoFile;
class CBlockIndex;
class Config;

inline double AllowFreeThreshold() {
    return COIN.GetSatoshis() * 144 / 250;
}

inline bool AllowFree(double dPriority) {
    // Large (in bytes) low-priority (new, small-coin) transactions need a fee.
    return dPriority > AllowFreeThreshold();
}

/**
 * Fake height value used in Coins to signify they are only in the memory
 * pool(since 0.8)
 */
static const uint32_t MEMPOOL_HEIGHT = 0x7FFFFFFF;

struct LockPoints {
    // Will be set to the blockchain height and median time past values that
    // would be necessary to satisfy all relative locktime constraints (BIP68)
    // of this tx given our view of block chain history
    int height;
    int64_t time;
    // As long as the current chain descends from the highest height block
    // containing one of the inputs used in the calculation, then the cached
    // values are still valid even after a reorg.
    CBlockIndex *maxInputBlock;

    LockPoints() : height(0), time(0), maxInputBlock(nullptr) {}
};

class CTxMemPool;

/**
 * Shared ancestor/descendant count information.
 */
struct AncestorDescendantCounts
{
    AncestorDescendantCounts(uint64_t ancestors, uint64_t descendants)
    : nCountWithAncestors{ancestors}, nCountWithDescendants{descendants}
    {}

    // These don't actually need to be atomic currently, but there's no cost
    // if they are and we might want to access them across threads in the future.
    std::atomic_uint64_t nCountWithAncestors   {0};
    std::atomic_uint64_t nCountWithDescendants {0};
};
using AncestorDescendantCountsPtr = std::shared_ptr<AncestorDescendantCounts>;

/** \class CTxPrioritizer
 *
 * The aim of this class is to support txn prioritisation and cleanup
 * for the given set of transactions (in RAII style).
 * If txn, that was prioritised, did not go to the mempool, then it's
 * prioritisation entry needs to be cleared during destruction of the object.
 */
class CTxPrioritizer final
{
private:
    CTxMemPool& mMempool;
    std::vector<TxId> mTxnsToPrioritise {};

public:
    CTxPrioritizer(CTxMemPool& mempool, TxId txnToPrioritise);
    CTxPrioritizer(CTxMemPool& mempool, std::vector<TxId> txnsToPrioritise);
    ~CTxPrioritizer();
    // Forbid copying/assignment
    CTxPrioritizer(const CTxPrioritizer&) = delete;
    CTxPrioritizer(CTxPrioritizer&&) = delete;
    CTxPrioritizer& operator=(const CTxPrioritizer&) = delete;
    CTxPrioritizer& operator=(CTxPrioritizer&&) = delete;
};

/** \class CTxMemPoolEntry
 *
 * CTxMemPoolEntry stores data about the corresponding transaction, as well as
 * data about all in-mempool transactions that depend on the transaction
 * ("descendant" transactions).
 *
 * When a new entry is added to the mempool, we update the descendant state
 * (nCountWithDescendants, nSizeWithDescendants, and nModFeesWithDescendants)
 * for all ancestors of the newly added transaction.
 *
 * If updating the descendant state is skipped, we can mark the entry as
 * "dirty", and set nSizeWithDescendants/nModFeesWithDescendants to equal
 * nTxSize/nFee+feeDelta. (This can potentially happen during a reorg, where we
 * limit the amount of work we're willing to do to avoid consuming too much
 * CPU.)
 */

class CTxMemPoolEntry {
private:
    CTransactionRef tx;
    //!< Cached to avoid expensive parent-transaction lookups
    Amount nFee;
    //!< ... and avoid recomputing tx size
    size_t nTxSize;
    //!< ... and modified size for priority
    size_t nModSize;
    //!< ... and total memory usage
    size_t nUsageSize;
    //!< Local time when entering the mempool
    int64_t nTime;
    //!< Priority when entering the mempool
    double entryPriority;
    //!< Sum of all txin values that are already in blockchain
    Amount inChainInputValue;
    //!< Total sigop plus P2SH sigops count
    int64_t sigOpCount;
    //!< Used for determining the priority of the transaction for mining in a
    //! block
    Amount feeDelta;
    //!< Track the height and time at which tx was final
    LockPoints lockPoints;

    // Information about descendants of this transaction that are in the
    // mempool; if we remove this transaction we must remove all of these
    // descendants as well.  if nCountWithDescendants is 0, treat this entry as
    // dirty, and nSizeWithDescendants and nModFeesWithDescendants will not be
    // correct.
    //!< number of descendant transactions
    AncestorDescendantCountsPtr ancestorDescendantCounts;
    //!< ... and size
    uint64_t nSizeWithDescendants;
    //!< ... and total fees (all including us)
    Amount nModFeesWithDescendants;

    // Analogous statistics for ancestor transactions
    uint64_t nSizeWithAncestors;
    Amount nModFeesWithAncestors;
    int64_t nSigOpCountWithAncestors;

    //!< Chain height when entering the mempool
    unsigned int entryHeight;
    //!< keep track of transactions that spend a coinbase
    bool spendsCoinbase;

public:
    CTxMemPoolEntry(const CTransactionRef &_tx, const Amount _nFee,
                    int64_t _nTime, double _entryPriority,
                    unsigned int _entryHeight, Amount _inChainInputValue,
                    bool spendsCoinbase, int64_t nSigOpsCost, LockPoints lp);

    CTxMemPoolEntry(const CTxMemPoolEntry &other) = default;
    CTxMemPoolEntry& operator=(const CTxMemPoolEntry&) = default;

    const CTransaction &GetTx() const { return *this->tx; }
    CTransactionRef GetSharedTx() const { return this->tx; }
    /**
     * Fast calculation of lower bound of current priority as update from entry
     * priority. Only inputs that were originally in-chain will age.
     */
    double GetPriority(unsigned int currentHeight) const;
    const Amount GetFee() const { return nFee; }
    size_t GetTxSize() const { return nTxSize; }
    int64_t GetTime() const { return nTime; }
    unsigned int GetHeight() const { return entryHeight; }
    int64_t GetSigOpCount() const { return sigOpCount; }
    Amount GetModifiedFee() const { return nFee + feeDelta; }
    size_t DynamicMemoryUsage() const { return nUsageSize; }
    const LockPoints &GetLockPoints() const { return lockPoints; }

    // Adjusts the descendant state, if this entry is not dirty.
    void UpdateDescendantState(int64_t modifySize, Amount modifyFee,
                               int64_t modifyCount);
    // Adjusts the ancestor state
    void UpdateAncestorState(int64_t modifySize, Amount modifyFee,
                             int64_t modifyCount, int modifySigOps);
    // Updates the fee delta used for mining priority score, and the
    // modified fees with descendants.
    void UpdateFeeDelta(Amount feeDelta);
    // Update the LockPoints after a reorg
    void UpdateLockPoints(const LockPoints &lp);

    const AncestorDescendantCountsPtr& GetAncestorDescendantCounts() const { return ancestorDescendantCounts; }
    uint64_t GetCountWithDescendants() const { return ancestorDescendantCounts->nCountWithDescendants; }
    uint64_t GetSizeWithDescendants() const { return nSizeWithDescendants; }
    Amount GetModFeesWithDescendants() const { return nModFeesWithDescendants; }

    bool GetSpendsCoinbase() const { return spendsCoinbase; }

    uint64_t GetCountWithAncestors() const { return ancestorDescendantCounts->nCountWithAncestors; }
    uint64_t GetSizeWithAncestors() const { return nSizeWithAncestors; }
    Amount GetModFeesWithAncestors() const { return nModFeesWithAncestors; }
    int64_t GetSigOpCountWithAncestors() const {
        return nSigOpCountWithAncestors;
    }

    //!< Index in mempool's vTxHashes
    mutable size_t vTxHashesIdx;
};

// Helpers for modifying CTxMemPool::mapTx, which is a boost multi_index.
struct update_descendant_state {
    update_descendant_state(int64_t _modifySize, Amount _modifyFee,
                            int64_t _modifyCount)
        : modifySize(_modifySize), modifyFee(_modifyFee),
          modifyCount(_modifyCount) {}

    void operator()(CTxMemPoolEntry &e) {
        e.UpdateDescendantState(modifySize, modifyFee, modifyCount);
    }

private:
    int64_t modifySize;
    Amount modifyFee;
    int64_t modifyCount;
};

struct update_ancestor_state {
    update_ancestor_state(int64_t _modifySize, Amount _modifyFee,
                          int64_t _modifyCount, int64_t _modifySigOpsCost)
        : modifySize(_modifySize), modifyFee(_modifyFee),
          modifyCount(_modifyCount), modifySigOpsCost(_modifySigOpsCost) {}

    void operator()(CTxMemPoolEntry &e) {
        e.UpdateAncestorState(modifySize, modifyFee, modifyCount,
                              modifySigOpsCost);
    }

private:
    int64_t modifySize;
    Amount modifyFee;
    int64_t modifyCount;
    int64_t modifySigOpsCost;
};

struct update_fee_delta {
    update_fee_delta(Amount _feeDelta) : feeDelta(_feeDelta) {}

    void operator()(CTxMemPoolEntry &e) { e.UpdateFeeDelta(feeDelta); }

private:
    Amount feeDelta;
};

struct update_lock_points {
    update_lock_points(const LockPoints &_lp) : lp(_lp) {}

    void operator()(CTxMemPoolEntry &e) { e.UpdateLockPoints(lp); }

private:
    const LockPoints &lp;
};

// extracts a transaction hash from CTxMempoolEntry or CTransactionRef
struct mempoolentry_txid {
    typedef uint256 result_type;
    result_type operator()(const CTxMemPoolEntry &entry) const {
        return entry.GetTx().GetId();
    }

    result_type operator()(const CTransactionRef &tx) const {
        return tx->GetId();
    }
};

/** \class CompareTxMemPoolEntryByDescendantScore
 *
 *  Sort an entry by max(score/size of entry's tx, score/size with all
 * descendants).
 */
class CompareTxMemPoolEntryByDescendantScore {
public:
    bool operator()(const CTxMemPoolEntry &a, const CTxMemPoolEntry &b) const {
        bool fUseADescendants = UseDescendantScore(a);
        bool fUseBDescendants = UseDescendantScore(b);

        double aModFee = (fUseADescendants ? a.GetModFeesWithDescendants()
                                           : a.GetModifiedFee())
                             .GetSatoshis();
        double aSize =
            fUseADescendants ? a.GetSizeWithDescendants() : a.GetTxSize();

        double bModFee = (fUseBDescendants ? b.GetModFeesWithDescendants()
                                           : b.GetModifiedFee())
                             .GetSatoshis();
        double bSize =
            fUseBDescendants ? b.GetSizeWithDescendants() : b.GetTxSize();

        // Avoid division by rewriting (a/b > c/d) as (a*d > c*b).
        double f1 = aModFee * bSize;
        double f2 = aSize * bModFee;

        if (f1 == f2) {
            return a.GetTime() >= b.GetTime();
        }
        return f1 < f2;
    }

    // Calculate which score to use for an entry (avoiding division).
    bool UseDescendantScore(const CTxMemPoolEntry &a) const {
        double f1 = double(a.GetSizeWithDescendants() *
                           a.GetModifiedFee().GetSatoshis());
        double f2 =
            double(a.GetTxSize() * a.GetModFeesWithDescendants().GetSatoshis());
        return f2 > f1;
    }
};

/** \class CompareTxMemPoolEntryByScore
 *
 *  Sort by score of entry ((fee+delta)/size) in descending order
 */
class CompareTxMemPoolEntryByScore {
public:
    bool operator()(const CTxMemPoolEntry &a, const CTxMemPoolEntry &b) const {
        double f1 = double(b.GetTxSize() * a.GetModifiedFee().GetSatoshis());
        double f2 = double(a.GetTxSize() * b.GetModifiedFee().GetSatoshis());
        if (f1 == f2) {
            return b.GetTx().GetId() < a.GetTx().GetId();
        }
        return f1 > f2;
    }
};

class CompareTxMemPoolEntryByEntryTime {
public:
    bool operator()(const CTxMemPoolEntry &a, const CTxMemPoolEntry &b) const {
        return a.GetTime() < b.GetTime();
    }
};

class CompareTxMemPoolEntryByAncestorFee {
public:
    bool operator()(const CTxMemPoolEntry &a, const CTxMemPoolEntry &b) const {
        double aFees = double(a.GetModFeesWithAncestors().GetSatoshis());
        double aSize = a.GetSizeWithAncestors();

        double bFees = double(b.GetModFeesWithAncestors().GetSatoshis());
        double bSize = b.GetSizeWithAncestors();

        // Avoid division by rewriting (a/b > c/d) as (a*d > c*b).
        double f1 = aFees * bSize;
        double f2 = aSize * bFees;

        if (f1 == f2) {
            return a.GetTx().GetId() < b.GetTx().GetId();
        }

        return f1 > f2;
    }
};

// Multi_index tag names
struct descendant_score {};
struct entry_time {};
struct ancestor_score {};


/**
 * Reason why a transaction was removed from the mempool, this is passed to the
 * notification signal.
 */
enum class MemPoolRemovalReason {
    //! Manually removed or unknown reason
    UNKNOWN = 0,
    //! Expired from mempool
    EXPIRY,
    //! Removed in size limiting
    SIZELIMIT,
    //! Removed for reorganization
    REORG,
    //! Removed for block
    BLOCK,
    //! Removed for conflict with in-block transaction
    CONFLICT,
    //! Removed for replacement
    REPLACED
};

class SaltedTxidHasher {
private:
    /** Salt */
    const uint64_t k0, k1;

public:
    SaltedTxidHasher();

    size_t operator()(const uint256 &txid) const {
        return SipHashUint256(k0, k1, txid);
    }
};

/**
 * CTxMemPool stores valid-according-to-the-current-best-chain transactions that
 * may be included in the next block.
 *
 * Transactions are added when they are seen on the network (or created by the
 * local node), but not all transactions seen are added to the pool. For
 * example, the following new transactions may not be added to the mempool:
 * - a transaction which doesn't meet the minimum fee requirements.
 * - a new transaction that double-spends an input of a transaction already in
 * the pool
 * - a non-standard transaction.
 * However, note that the determination of whether a transaction is to be added to
 * the pool is not made in this class.
 *
 * CTxMemPool::mapTx, and CTxMemPoolEntry bookkeeping:
 *
 * mapTx is a boost::multi_index that sorts the mempool on 4 criteria:
 * - transaction hash
 * - feerate [we use max(feerate of tx, feerate of tx with all descendants)]
 * - time in mempool
 * - mining score (feerate modified by any fee deltas from PrioritiseTransaction)
 *
 * Note: mapTx will become private and may be modified extensively in the future. It will
 * not be part of the public definition of this class.
 *
 * Note: the term "descendant" refers to in-mempool transactions that depend on
 * this one, while "ancestor" refers to in-mempool transactions that a given
 * transaction depends on.
 *
 * Note: the feerate sort (referenced below) will be removed in future.
 * In order for the feerate sort to remain correct, we must update transactions
 * in the mempool when new descendants arrive. To facilitate this, we track the
 * set of in-mempool direct parents and direct children in mapLinks. Within each
 * CTxMemPoolEntry, we track the size and fees of all descendants.
 *
 * Note: tracking of ancestors and descendants may be removed in the future.
 *
 * Usually when a new transaction is added to the mempool, it has no in-mempool
 * children (because any such children would be an orphan). So in
 * AddUnchecked(), we:
 * - update a new entry's setMemPoolParents to include all in-mempool parents
 * - update the new entry's direct parents to include the new tx as a child
 * - update all ancestors of the transaction to include the new tx's size/fee
 *
 * When a transaction is removed from the mempool, we must:
 * - update all in-mempool parents to not track the tx in setMemPoolChildren
 * - update all ancestors to not include the tx's size/fees in descendant state
 * - update all in-mempool children to not include it as a parent
 *
 * These happen in updateForRemoveFromMempoolNL(). (Note that when removing a
 * transaction along with its descendants, we must calculate that set of
 * transactions to be removed before doing the removal, or else the mempool can
 * be in an inconsistent state where it's impossible to walk the ancestors of a
 * transaction.)
 *
 * In the event of a reorg, the assumption that a newly added tx has no
 * in-mempool children is false.  In particular, the mempool is in an
 * inconsistent state while new transactions are being added, because there may
 * be descendant transactions of a tx coming from a disconnected block that are
 * unreachable from just looking at transactions in the mempool (the linking
 * transactions may also be in the disconnected block, waiting to be added).
 * Because of this, there's not much benefit in trying to search for in-mempool
 * children in AddUnchecked(). Instead, in the special case of transactions
 * being added from a disconnected block, we require the caller to clean up the
 * state, to account for in-mempool, out-of-block descendants for all the
 * in-block transactions by calling UpdateTransactionsFromBlock(). Note that
 * until this is called, the mempool state is not consistent, and in particular
 * mapLinks may not be correct (and therefore functions like
 * CalculateMemPoolAncestors() and CalculateDescendants() that rely on them to
 * walk the mempool are not generally safe to use).
 *
 * Computational limits:
 *
 * Updating all in-mempool ancestors of a newly added transaction can be slow,
 * if no bound exists on how many in-mempool ancestors there may be.
 * CalculateMemPoolAncestors() takes configurable limits that are designed to
 * prevent these calculations from being too CPU intensive.
 *
 * Adding transactions from a disconnected block can be very time consuming,
 * because we don't have a way to limit the number of in-mempool descendants. To
 * bound CPU processing, we limit the amount of work we're willing to do to
 * properly update the descendant information for a tx being added from a
 * disconnected block. If we would exceed the limit, then we instead mark the
 * entry as "dirty", and set the feerate for sorting purposes to be equal the
 * feerate of the transaction without any descendants.
 */
class CTxMemPool {
private:
    //!< Value n means that n times in 2^32 we check.
    std::atomic_uint32_t nCheckFrequency;
    std::atomic_uint nTransactionsUpdated;

    CFeeRate blockMinTxfee {DEFAULT_BLOCK_MIN_TX_FEE};

    //!< sum of all mempool tx's virtual sizes.
    uint64_t totalTxSize;
    //!< sum of dynamic memory usage of all the map elements (NOT the maps
    //! themselves)
    uint64_t cachedInnerUsage;

    mutable int64_t lastRollingFeeUpdate;
    mutable bool blockSinceLastRollingFeeBump;
    //!< minimum fee to get into the pool, decreases exponentially
    mutable double rollingMinimumFeeRate;

    // Our journal builder
    mutable mining::CJournalBuilder mJournalBuilder;

    // Sub-pool for time locked txns
    CTimeLockedMempool mTimeLockedPool {};

public:
    // public only for testing
    static const int ROLLING_FEE_HALFLIFE = 60 * 60 * 12;

    // DEPRECATED - this will become private and ultimately changed or removed
    typedef boost::multi_index_container<
        CTxMemPoolEntry, boost::multi_index::indexed_by<
                             // sorted by txid
                             boost::multi_index::hashed_unique<
                                 mempoolentry_txid, SaltedTxidHasher>,
                             // sorted by fee rate
                             boost::multi_index::ordered_non_unique<
                                 boost::multi_index::tag<descendant_score>,
                                 boost::multi_index::identity<CTxMemPoolEntry>,
                                 CompareTxMemPoolEntryByDescendantScore>,
                             // sorted by entry time
                             boost::multi_index::ordered_non_unique<
                                 boost::multi_index::tag<entry_time>,
                                 boost::multi_index::identity<CTxMemPoolEntry>,
                                 CompareTxMemPoolEntryByEntryTime>,
                             // sorted by fee rate with ancestors
                             boost::multi_index::ordered_non_unique<
                                 boost::multi_index::tag<ancestor_score>,
                                 boost::multi_index::identity<CTxMemPoolEntry>,
                                 CompareTxMemPoolEntryByAncestorFee>>>
        indexed_transaction_set;

    // DEPRECATED - this will become private and ultimately changed or removed
    mutable std::shared_mutex smtx;
    // DEPRECATED - this will become private and ultimately changed or removed
    indexed_transaction_set mapTx;

    // DEPRECATED - this will become private and ultimately changed or removed
    typedef indexed_transaction_set::nth_index<0>::type::iterator txiter;
    //!< All tx hashes/entries in mapTx, in random order
    // DEPRECATED - this will become private and ultimately changed or removed
    std::vector<std::pair<uint256, txiter>> vTxHashes;

    // DEPRECATED - this will become private and ultimately changed or removed
    struct CompareIteratorByHash {
        bool operator()(const txiter &a, const txiter &b) const {
            return a->GetTx().GetId() < b->GetTx().GetId();
        }
    };
    // DEPRECATED - this will become private and ultimately changed or removed
    typedef std::set<txiter, CompareIteratorByHash> setEntries;

    // DEPRECATED - this will become private and ultimately changed or removed
    const setEntries &GetMemPoolParentsNL(txiter entry) const;
    // DEPRECATED - this will become private and ultimately changed or removed
    const setEntries &GetMemPoolChildrenNL(txiter entry) const;

private:
    typedef std::map<txiter, setEntries, CompareIteratorByHash> cacheMap;

    struct TxLinks {
        setEntries parents;
        setEntries children;
    };

    typedef std::map<txiter, TxLinks, CompareIteratorByHash> txlinksMap;
    txlinksMap mapLinks;

    void updateParentNL(txiter entry, txiter parent, bool add);
    void updateChildNL(txiter entry, txiter child, bool add);

    std::vector<indexed_transaction_set::const_iterator>
    getSortedDepthAndScoreNL() const;

public:
    // DEPRECATED - this will become private and ultimately changed or removed
    indirectmap<COutPoint, const CTransaction *> mapNextTx;
    // DEPRECATED - this will become private and ultimately changed or removed
    std::map<uint256, std::pair<double, Amount>> mapDeltas;

    /** Create a new CTxMemPool.
     */
    CTxMemPool();
    ~CTxMemPool();

    /**
     * If sanity-checking is turned on, check makes sure the pool is consistent
     * (does not contain two transactions that spend the same inputs, all inputs
     * are in the mapNextTx array, journal is in agreement with mempool).
     * If sanity-checking is turned off, check does nothing.
     */
    void CheckMempool(
        const CCoinsViewCache *pcoins,
        const mining::CJournalChangeSetPtr& changeSet) const;

    std::string CheckJournal() const;

    void SetSanityCheck(double dFrequency = 1.0);

    void SetBlockMinTxFee(CFeeRate feerate) { blockMinTxfee = feerate; };

    /** Rebuild the journal contents so they match the mempool */
    void RebuildJournal() const;

    // AddUnchecked must updated state for all ancestors of a given transaction,
    // to track size/count of descendant transactions. First version of
    // AddUnchecked can be used to have it call CalculateMemPoolAncestors(), and
    // then invoke the second version.
    void AddUnchecked(
            const uint256 &hash,
            const CTxMemPoolEntry &entry,
            const mining::CJournalChangeSetPtr& changeSet,
            size_t* pnMempoolSize = nullptr,
            size_t* pnDynamicMemoryUsage = nullptr);

    void AddUnchecked(
            const uint256 &hash,
            const CTxMemPoolEntry &entry,
            setEntries &setAncestors,
            const mining::CJournalChangeSetPtr& changeSet,
            size_t* pnMempoolSize = nullptr,
            size_t* pnDynamicMemoryUsage = nullptr);

    void RemoveRecursive(
        const CTransaction &tx,
        const mining::CJournalChangeSetPtr& changeSet,
        MemPoolRemovalReason reason = MemPoolRemovalReason::UNKNOWN);

    void RemoveForReorg(
            const Config &config,
            const CCoinsViewCache *pcoins,
            const mining::CJournalChangeSetPtr& changeSet,
            int nChainActiveHeight,
            int nMedianTimePast,
            int flags);

    void RemoveForBlock(
            const std::vector<CTransactionRef> &vtx,
            unsigned int nBlockHeight,
            const mining::CJournalChangeSetPtr& changeSet);

    void Clear();

    bool CompareDepthAndScore(
            const uint256 &hasha,
            const uint256 &hashb);
    // A non-locking version of CompareDepthAndScore
    // DEPRECATED - this will become private and ultimately changed or removed
    bool CompareDepthAndScoreNL(
            const uint256 &hasha,
            const uint256 &hashb);

    void QueryHashes(std::vector<uint256> &vtxid);
    bool IsSpent(const COutPoint &outpoint);
    // A non-locking version of IsSpent
    // DEPRECATED - this will become private and ultimately changed or removed
    bool IsSpentNL(const COutPoint &outpoint);
    unsigned int GetTransactionsUpdated() const;
    void AddTransactionsUpdated(unsigned int n);
    /**
     * Check that none of this transactions inputs are in the mempool, and thus
     * the tx is not dependent on other mempool transactions to be included in a
     * block.
     */
    bool HasNoInputsOf(const CTransaction &tx) const;

    /** Affect CreateNewBlock prioritisation of transactions */
    void PrioritiseTransaction(
            const uint256& hash,
            const std::string& strHash,
            double dPriorityDelta,
            const Amount nFeeDelta);
    void PrioritiseTransaction(
            const std::vector<TxId>& vTxToPrioritise,
            double dPriorityDelta,
            const Amount nFeeDelta);

    void ApplyDeltas(
            const uint256& hash,
            double &dPriorityDelta,
            Amount &nFeeDelta) const;
    // A non-locking version of ApplyDeltas
    // DEPRECATED - this will become private and ultimately changed or removed
    void ApplyDeltasNL(
            const uint256& hash,
            double &dPriorityDelta,
            Amount &nFeeDelta) const;

    // Get a reference to the journal builder
    mining::CJournalBuilder& getJournalBuilder() { return mJournalBuilder; }

    // Get a reference to the time-locked (non-final txn) mempool
    CTimeLockedMempool& getNonFinalPool() { return mTimeLockedPool; }

public:
    /**
     * Remove a set of transactions from the mempool. If a transaction is in
     * this set, then all in-mempool descendants must also be in the set, unless
     * this transaction is being removed for being in a block. Set
     * updateDescendants to true when removing a tx that was in a block, so that
     * any in-mempool descendants have their ancestor state updated.
     */
    void
    RemoveStaged(
        setEntries &stage,
        bool updateDescendants,
        const mining::CJournalChangeSetPtr& changeSet,
        MemPoolRemovalReason reason = MemPoolRemovalReason::UNKNOWN);

    /**
     * When adding transactions from a disconnected block back to the mempool,
     * new mempool entries may have children in the mempool (which is generally
     * not the case when otherwise adding transactions).
     * UpdateTransactionsFromBlock() will find child transactions and update the
     * descendant state for each transaction in hashesToUpdate (excluding any
     * child transactions present in hashesToUpdate, which are already accounted
     * for).  
     * Note: hashesToUpdate should be the set of transactions from the
     *       disconnected block that have been accepted back into the mempool.
     * Note: if the transaction from the disconnected block does not enter the journal
     *       but their descendants are already in the journal we have to remove 
     *       them using the changeSet. this can happen when low paying tx is mined
     *       and we have its descendant in the mempool and journal, and when the block 
     *       is disconnected low paying tx will end up in the mempool but will not be in the
     *       journal while its descendants are already in
     */
    void UpdateTransactionsFromBlock(
            const std::vector<uint256> &hashesToUpdate,
            const mining::CJournalChangeSetPtr &changeSet);

    /**
     * Try to calculate all in-mempool ancestors of entry.
     *  (these are all calculated including the tx itself)
     *  limitAncestorCount = max number of ancestors
     *  limitAncestorSize = max size of ancestors
     *  limitDescendantCount = max number of descendants any ancestor can have
     *  limitDescendantSize = max size of descendants any ancestor can have
     *  errString = populated with error reason if any limits are hit
     * fSearchForParents = whether to search a tx's vin for in-mempool parents,
     * or look up parents from mapLinks. Must be true for entries not in the
     * mempool
     */
    bool CalculateMemPoolAncestors(
            const CTxMemPoolEntry &entry,
            setEntries &setAncestors,
            uint64_t limitAncestorCount,
            uint64_t limitAncestorSize,
            uint64_t limitDescendantCount,
            uint64_t limitDescendantSize,
            std::string &errString,
            bool fSearchForParents = true) const;
    // A non-locking version of CalculateMemPoolAncestors
    // DEPRECATED - this will become private and ultimately changed or removed
    bool CalculateMemPoolAncestorsNL(
            const CTxMemPoolEntry &entry,
            setEntries &setAncestors,
            uint64_t limitAncestorCount,
            uint64_t limitAncestorSize,
            uint64_t limitDescendantCount,
            uint64_t limitDescendantSize,
            std::string &errString,
            bool fSearchForParents = true) const;

    /**
     * Populate setDescendants with all in-mempool descendants of hash.
     * Assumes that setDescendants includes all in-mempool descendants of
     * anything already in it.  */
    void CalculateDescendants(
            txiter it,
            setEntries &setDescendants);
    // A non-locking version of CalculateDescendants
    // DEPRECATED - this will become private and ultimately changed or removed
    void CalculateDescendantsNL(
            txiter it,
            setEntries &setDescendants);

    /**
     * The minimum fee to get into the mempool, which may itself not be enough
     * for larger-sized transactions.
     */
    CFeeRate GetMinFee(size_t sizelimit) const;

    /**
     * Remove transactions from the mempool until its dynamic size is <=
     * sizelimit. pvNoSpendsRemaining, if set, will be populated with the list
     * of outpoints which are not in mempool which no longer have any spends in
     * this mempool. Return TxIds which were removed (if pvNoSpendsRemaining is set).
     */
    std::vector<TxId> TrimToSize(
        size_t sizelimit,
        const mining::CJournalChangeSetPtr& changeSet,
        std::vector<COutPoint> *pvNoSpendsRemaining = nullptr);

    /** Expire all transaction (and their dependencies) in the mempool older
     * than time. Return the number of removed transactions. */
    int Expire(int64_t time, const mining::CJournalChangeSetPtr& changeSet);

    /**
     * Check for conflicts with in-mempool transactions.
     * @param tx A reference to the given txn
     * @param nonFinal A flag to indicate if tx is a non-final transaction
     */
    bool CheckTxConflicts(const CTransactionRef& tx, bool isFinal) const;

    /** Returns false if the transaction is in the mempool and not within the
     * chain limit specified. */
    bool TransactionWithinChainLimit(
            const uint256 &txid,
            size_t chainLimit) const;

    unsigned long Size();

    uint64_t GetTotalTxSize();

    bool Exists(const uint256& hash) const;
    // A non-locking version of Exists
    // DEPRECATED - this will become private and ultimately changed or removed
    bool ExistsNL(const uint256& hash) const;

    bool Exists(const COutPoint &outpoint) const;
    // A non-locking version of Exists
    // DEPRECATED - this will become private and ultimately changed or removed
    bool ExistsNL(const COutPoint &outpoint) const;

    CTransactionRef Get(const uint256& hash) const;
    // A non-locking version of Get
    // DEPRECATED - this will become private and ultimately changed or removed
    CTransactionRef GetNL(const uint256& hash) const;

    TxMempoolInfo Info(const uint256& hash) const;

    std::vector<TxMempoolInfo> InfoAll() const;
    // A non-locking version of InfoAll
    // DEPRECATED - this will become private and ultimately changed or removed
    std::vector<TxMempoolInfo> InfoAllNL() const;

    size_t DynamicMemoryUsage() const;

    CFeeRate estimateFee() const;

    boost::signals2::signal<void(CTransactionRef)> NotifyEntryAdded;
    boost::signals2::signal<void(CTransactionRef, MemPoolRemovalReason)>
        NotifyEntryRemoved;

    void ClearPrioritisation(const uint256 &hash);
    void ClearPrioritisation(const std::vector<TxId>& vTxIds);

private:
    /**
     * updateForDescendantsNL is used by UpdateTransactionsFromBlock to update the
     * descendants for a single transaction that has been added to the mempool
     * but may have child transactions in the mempool, eg during a chain reorg.
     * setExclude is the set of descendant transactions in the mempool that must
     * not be accounted for (because any descendants in setExclude were added to
     * the mempool after the transaction being updated and hence their state is
     * already reflected in the parent state).
     *
     * cachedDescendants will be updated with the descendants of the transaction
     * being updated, so that future invocations don't need to walk the same
     * transaction again, if encountered in another transaction chain.
     */
    void updateForDescendantsNL(
            txiter updateIt,
            cacheMap &cachedDescendants,
            const std::set<uint256> &setExclude);

    // A non-locking version of AddUnchecked
    // A signal NotifyEntryAdded is decoupled from AddUncheckedNL.
    // It needs to be called explicitly by a user if AddUncheckedNL is used.
    void AddUncheckedNL(
            const uint256& hash,
            const CTxMemPoolEntry &entry,
            setEntries &setAncestors,
            const mining::CJournalChangeSetPtr& changeSet,
            size_t* pnMempoolSize = nullptr,
            size_t* pnDynamicMemoryUsage = nullptr);

    /**
     * Update ancestors of hash to add/remove it as a descendant transaction.
     */
    void updateAncestorsOfNL(
            bool add,
            txiter hash,
            setEntries &setAncestors);

    /**
     * Set ancestor state for an entry
     */
    void updateEntryForAncestorsNL(
            txiter it,
            const setEntries &setAncestors);

    /**
     * For each transaction being removed, update ancestors and any direct
     * children. If updateDescendants is true, then also update in-mempool
     * descendants' ancestor state.
     */

    void updateForRemoveFromMempoolNL(
            const setEntries &entriesToRemove,
            bool updateDescendants);

    /**
     * Sever link between specified transaction and direct children.
     */
    void updateChildrenForRemovalNL(txiter entry);

    /**
     * Before calling removeUnchecked for a given transaction,
     * updateForRemoveFromMempoolNL must be called on the entire (dependent) set
     * of transactions being removed at the same time. We use each
     * CTxMemPoolEntry's setMemPoolParents in order to walk ancestors of a given
     * transaction that is removed, so we can't remove intermediate transactions
     * in a chain before we've updated all the state for the removal.
     */
    void removeUncheckedNL(
            txiter entry,
            const mining::CJournalChangeSetPtr& changeSet,
            MemPoolRemovalReason reason = MemPoolRemovalReason::UNKNOWN);

    void removeConflictsNL(
            const CTransaction &tx,
            const mining::CJournalChangeSetPtr& changeSet);

    void clearNL();

    void trackPackageRemovedNL(const CFeeRate &rate);

    /**
     * Remove a set of transactions from the mempool. If a transaction is in
     * this set, then all in-mempool descendants must also be in the set, unless
     * this transaction is being removed for being in a block. Set
     * updateDescendants to true when removing a tx that was in a block, so that
     * any in-mempool descendants have their ancestor state updated.
     */
    void removeStagedNL(
            setEntries &stage,
            bool updateDescendants,
            const mining::CJournalChangeSetPtr& changeSet,
            MemPoolRemovalReason reason = MemPoolRemovalReason::UNKNOWN,
            bool updateJournal = true);

    void prioritiseTransactionNL(
            const uint256& hash,
            double dPriorityDelta,
            const Amount nFeeDelta);

    void clearPrioritisationNL(const uint256& hash);

    // A non-locking version of RemoveRecursive
    void removeRecursiveNL(
            const CTransaction &tx,
            const mining::CJournalChangeSetPtr& changeSet,
            MemPoolRemovalReason reason = MemPoolRemovalReason::UNKNOWN);

    // A non-locking version of checkJournal
    std::string checkJournalNL() const;

    // A non-locking version of DynamicMemoryUsage.
    size_t DynamicMemoryUsageNL() const;

    // returns a set of entries which are connected to any of items in the given set, but not elements of the given set. transactions are considered "connected"  
    // if one transaction can reach another trough one or more child or parent relations
    // - only transactions for which filter returns true are considered
    // - every transaction from the set will have all its in-mempool ancestors included in the set (if they pass filter)
    // - if the limit is not std::nullopt, not all children/siblings will be added. adding new transactions to the set is stopped when the size of the 
    //   entries set is reached, but we have to ensure that all parents of all transactions in the set, so
    //   we have to add parents of the last added entry too, this can extend the size of the entries set by up to {-limitdescendantcount - 1} transactions
    setEntries getConnectedNL(const setEntries& entries, std::function<bool(txiter)> filter, std::optional<size_t> limit= std::nullopt) const;
    
    // returns a set of entries which are connected to any of items in the given set, but not elements of the given set.
    setEntries getConnectedNL(const setEntries& entries) const;

    // checks if affected transactions have met conditions to be mined, 
    // - if they could be mined but they are not in the journal they are added to the changeSet as ADD
    // - if they could not be mined but they are in the journal they are added to the changeSet as REMOVE
    // - "Child pays for parent" (cpfp) is taken into account here
    // - parents which are not in the affectedTransactions set are considered to be already mined or accepted to the journal
    void checkJournalAcceptanceNL(const setEntries& affectedTransactions, mining::CJournalChangeSet& changeSet) const;
};

/**
 * CCoinsView that brings transactions from a memorypool into view.
 * It does not check for spendings by memory pool transactions.
 */
class CCoinsViewMemPool : public CCoinsViewBacked {
protected:
    const CTxMemPool &mempool;

public:
    // The caller of the constructor needs to hold mempool.smtx.
    CCoinsViewMemPool(CCoinsView *baseIn, const CTxMemPool &mempoolIn);
    // The caller of GetCoin needs to hold mempool.smtx.
    bool GetCoin(const COutPoint &outpoint, Coin &coin) const override;
    bool HaveCoin(const COutPoint &outpoint) const override;
};

// We want to sort transactions by coin age priority
typedef std::pair<double, CTxMemPool::txiter> TxCoinAgePriority;

struct TxCoinAgePriorityCompare {
    bool operator()(const TxCoinAgePriority &a, const TxCoinAgePriority &b) {
        if (a.first == b.first) {
            // Reverse order to make sort less than
            return CompareTxMemPoolEntryByScore()(*(b.second), *(a.second));
        }
        return a.first < b.first;
    }
};

/**
 * DisconnectedBlockTransactions

 * During the reorg, it's desirable to re-add previously confirmed transactions
 * to the mempool, so that anything not re-confirmed in the new chain is
 * available to be mined. However, it's more efficient to wait until the reorg
 * is complete and process all still-unconfirmed transactions at that time,
 * since we expect most confirmed transactions to (typically) still be
 * confirmed in the new chain, and re-accepting to the memory pool is expensive
 * (and therefore better to not do in the middle of reorg-processing).
 * Instead, store the disconnected transactions (in order!) as we go, remove any
 * that are included in blocks in the new chain, and then process the remaining
 * still-unconfirmed transactions at the end.
 */

// multi_index tag names
struct txid_index {};
struct insertion_order {};

struct DisconnectedBlockTransactions {
    typedef boost::multi_index_container<
        CTransactionRef, boost::multi_index::indexed_by<
                             // sorted by txid
                             boost::multi_index::hashed_unique<
                                 boost::multi_index::tag<txid_index>,
                                 mempoolentry_txid, SaltedTxidHasher>,
                             // sorted by order in the blockchain
                             boost::multi_index::sequenced<
                                 boost::multi_index::tag<insertion_order>>>>
        indexed_disconnected_transactions;

    // It's almost certainly a logic bug if we don't clear out queuedTx before
    // destruction, as we add to it while disconnecting blocks, and then we
    // need to re-process remaining transactions to ensure mempool consistency.
    // For now, assert() that we've emptied out this object on destruction.
    // This assert() can always be removed if the reorg-processing code were
    // to be refactored such that this assumption is no longer true (for
    // instance if there was some other way we cleaned up the mempool after a
    // reorg, besides draining this object).
    ~DisconnectedBlockTransactions() { assert(queuedTx.empty()); }

    indexed_disconnected_transactions queuedTx;
    uint64_t cachedInnerUsage = 0;

    // Estimate the overhead of queuedTx to be 6 pointers + an allocation, as
    // no exact formula for boost::multi_index_contained is implemented.
    size_t DynamicMemoryUsage() const {
        return memusage::MallocUsage(sizeof(CTransactionRef) +
                                     6 * sizeof(void *)) *
                   queuedTx.size() +
               cachedInnerUsage;
    }

    void addTransaction(const CTransactionRef &tx) {
        queuedTx.insert(tx);
        cachedInnerUsage += RecursiveDynamicUsage(tx);
    }

    // Remove entries based on txid_index, and update memory usage.
    void removeForBlock(const std::vector<CTransactionRef> &vtx) {
        // Short-circuit in the common case of a block being added to the tip
        if (queuedTx.empty()) {
            return;
        }
        for (auto const &tx : vtx) {
            auto it = queuedTx.find(tx->GetHash());
            if (it != queuedTx.end()) {
                cachedInnerUsage -= RecursiveDynamicUsage(*it);
                queuedTx.erase(it);
            }
        }
    }

    // Remove an entry by insertion_order index, and update memory usage.
    void removeEntry(indexed_disconnected_transactions::index<
                     insertion_order>::type::iterator entry) {
        cachedInnerUsage -= RecursiveDynamicUsage(*entry);
        queuedTx.get<insertion_order>().erase(entry);
    }

    void clear() {
        cachedInnerUsage = 0;
        queuedTx.clear();
    }
};

#endif // BITCOIN_TXMEMPOOL_H
