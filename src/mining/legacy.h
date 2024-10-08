// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2019 Bitcoin Association
// Distributed under the Open TBC software license, see the accompanying file LICENSE.

#ifndef BITCOIN_MINER_H
#define BITCOIN_MINER_H

#include "mining/assembler.h"
#include "primitives/block.h"
#include "txmempool.h"

#include "boost/multi_index/ordered_index.hpp"
#include "boost/multi_index_container.hpp"

#include <cstdint>
#include <memory>

class CBlockIndex;
class CChainParams;
class Config;
class CReserveKey;
class CScript;
class CWallet;

static const bool DEFAULT_PRINTPRIORITY = false;

// Container for tracking updates to ancestor feerate as we include (parent)
// transactions in a block
struct CTxMemPoolModifiedEntry {
    CTxMemPoolModifiedEntry(CTxMemPool::txiter entry) {
        iter = entry;
        nSizeWithAncestors = entry->GetSizeWithAncestors();
        nModFeesWithAncestors = entry->GetModFeesWithAncestors();
        nSigOpCountWithAncestors = entry->GetSigOpCountWithAncestors();
    }

    CTxMemPool::txiter iter;
    uint64_t nSizeWithAncestors;
    Amount nModFeesWithAncestors;
    int64_t nSigOpCountWithAncestors;
};

/**
 * Comparator for CTxMemPool::txiter objects.
 * It simply compares the internal memory address of the CTxMemPoolEntry object
 * pointed to. This means it has no meaning, and is only useful for using them
 * as key in other indexes.
 */
struct CompareCTxMemPoolIter {
    bool operator()(const CTxMemPool::txiter &a,
                    const CTxMemPool::txiter &b) const {
        return &(*a) < &(*b);
    }
};

struct modifiedentry_iter {
    typedef CTxMemPool::txiter result_type;
    result_type operator()(const CTxMemPoolModifiedEntry &entry) const {
        return entry.iter;
    }
};

// This matches the calculation in CompareTxMemPoolEntryByAncestorFee,
// except operating on CTxMemPoolModifiedEntry.
// TODO: refactor to avoid duplication of this logic.
struct CompareModifiedEntry {
    bool operator()(const CTxMemPoolModifiedEntry &a,
                    const CTxMemPoolModifiedEntry &b) const {
        double f1 = double(b.nSizeWithAncestors *
                           a.nModFeesWithAncestors.GetSatoshis());
        double f2 = double(a.nSizeWithAncestors *
                           b.nModFeesWithAncestors.GetSatoshis());
        if (f1 == f2) {
            return CTxMemPool::CompareIteratorByHash()(a.iter, b.iter);
        }
        return f1 > f2;
    }
};

// A comparator that sorts transactions based on number of ancestors.
// This is sufficient to sort an ancestor package in an order that is valid
// to appear in a block.
struct CompareTxIterByAncestorCount {
    bool operator()(const CTxMemPool::txiter &a,
                    const CTxMemPool::txiter &b) const {
        if (a->GetCountWithAncestors() != b->GetCountWithAncestors())
            return a->GetCountWithAncestors() < b->GetCountWithAncestors();
        return CTxMemPool::CompareIteratorByHash()(a, b);
    }
};

typedef boost::multi_index_container<
    CTxMemPoolModifiedEntry,
    boost::multi_index::indexed_by<
        boost::multi_index::ordered_unique<modifiedentry_iter,
                                           CompareCTxMemPoolIter>,
        // sorted by modified ancestor fee rate
        boost::multi_index::ordered_non_unique<
            // Reuse same tag from CTxMemPool's similar index
            boost::multi_index::tag<ancestor_score>,
            boost::multi_index::identity<CTxMemPoolModifiedEntry>,
            CompareModifiedEntry>>>
    indexed_modified_transaction_set;

typedef indexed_modified_transaction_set::nth_index<0>::type::iterator
    modtxiter;
typedef indexed_modified_transaction_set::index<ancestor_score>::type::iterator
    modtxscoreiter;

struct update_for_parent_inclusion {
    update_for_parent_inclusion(CTxMemPool::txiter it) : iter(it) {}

    void operator()(CTxMemPoolModifiedEntry &e) {
        e.nModFeesWithAncestors -= iter->GetFee();
        e.nSizeWithAncestors -= iter->GetTxSize();
        e.nSigOpCountWithAncestors -= iter->GetSigOpCount();
    }

    CTxMemPool::txiter iter;
};

/** Generate a new block, without valid proof-of-work */
class LegacyBlockAssembler : public mining::BlockAssembler {
private:
    // The constructed block template
    std::unique_ptr<mining::CBlockTemplate> pblocktemplate;
    // A convenience pointer that always refers to the CBlock in pblocktemplate
    CBlock *pblock;

    // Configuration parameters for the block size
    CFeeRate blockMinFeeRate;

    // Information on the current status of the block
    uint64_t nBlockSize;
    uint64_t nBlockTx;
    uint64_t nBlockSigOps;
    CTxMemPool::setEntries inBlock;

    // Cache the current maximum generated block size
    uint64_t nMaxGeneratedBlockSize;

    // Chain context for the block
    int nHeight;
    int64_t nLockTimeCutoff;

    // Variables used for addPriorityTxs
    int lastFewTxs;
    bool blockFinished;

public:
    LegacyBlockAssembler(const Config &_config);
    /** Construct a new block template with coinbase to scriptPubKeyIn */
    std::unique_ptr<mining::CBlockTemplate> CreateNewBlock(const CScript& scriptPubKeyIn, CBlockIndex*& pindexPrev) override;

    /** Get the maximum generated block size for the current config and chain tip */
    uint64_t GetMaxGeneratedBlockSize() const override { return nMaxGeneratedBlockSize; }

private:
    // utility functions
    /** Clear the block's state and prepare for assembling a new block */
    void resetBlock();
    /** Add a tx to the block */
    void AddToBlock(CTxMemPool::txiter iter);

    // Methods for how to add transactions to a block.
    /** Add transactions based on tx "priority" */
    void addPriorityTxs();
    /** Add transactions based on feerate including unconfirmed ancestors
     * Increments nPackagesSelected / nDescendantsUpdated with corresponding
     * statistics from the package selection (for logging statistics). */
    void addPackageTxs(int &nPackagesSelected, int &nDescendantsUpdated);

    // helper function for addPriorityTxs
    /** Test if tx will still "fit" in the block */
    bool TestForBlock(CTxMemPool::txiter iter);
    /** Test if tx still has unconfirmed parents not yet in block */
    bool isStillDependent(CTxMemPool::txiter iter);

    // helper functions for addPackageTxs()
    /** Remove confirmed (inBlock) entries from given set */
    void onlyUnconfirmed(CTxMemPool::setEntries &testSet);
    /** Test if a new package would "fit" in the block */
    bool TestPackage(uint64_t packageSize, int64_t packageSigOpsCost);
    /** Perform checks on each transaction in a package:
     * locktime, serialized size (if necessary)
     * These checks should always succeed, and they're here
     * only as an extra check in case of suboptimal node configuration */
    bool TestPackageTransactions(const CTxMemPool::setEntries &package);
    /** Return true if given transaction from mapTx has already been evaluated,
     * or if the transaction's cached data in mapTx is incorrect. */
    bool SkipMapTxEntry(CTxMemPool::txiter it,
                        indexed_modified_transaction_set &mapModifiedTx,
                        CTxMemPool::setEntries &failedTx);
    /** Sort the package in an order that is valid to appear in a block */
    void SortForBlock(const CTxMemPool::setEntries &package,
                      CTxMemPool::txiter entry,
                      std::vector<CTxMemPool::txiter> &sortedEntries);
    /** Add descendants of given transactions to mapModifiedTx with ancestor
     * state updated assuming given transactions are inBlock. Returns number
     * of updated descendants. */
    int UpdatePackagesForAdded(const CTxMemPool::setEntries &alreadyAdded,
                               indexed_modified_transaction_set &mapModifiedTx);
};

/** Modify the extranonce in a block */
void IncrementExtraNonce(CBlock *pblock,
                         const CBlockIndex *pindexPrev,
                         unsigned int &nExtraNonce);
int64_t UpdateTime(CBlockHeader *pblock, const Config &config,
                   const CBlockIndex *pindexPrev);
#endif // BITCOIN_MINER_H
