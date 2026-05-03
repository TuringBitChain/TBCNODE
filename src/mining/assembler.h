// Copyright (c) 2019 Bitcoin Association.
// Distributed under the Open TBC software license, see the accompanying file LICENSE.

#pragma once

#include "primitives/block.h"

class Config;
class CBlockIndex;

namespace mining
{

/**
 * The CBlockTemplate is used during the assembly of a new block.
 */
class CBlockTemplate {
private:
    CBlockRef mBlock { std::make_shared<CBlock>() };

public:
    CBlockTemplate() = default;
    CBlockTemplate(const CBlockRef& block) : mBlock{block} {}
    CBlockRef GetBlockRef() const { return mBlock; }

    std::vector<Amount> vTxFees;
    std::vector<int64_t> vTxSigOpsCount;
};


/**
 * The Block Assembler assembles a new block. It collects transactions from the mempool, prioritizes them, and ensures
 * that all required ancestors are present.
 */
class BlockAssembler {
public:
    BlockAssembler(const Config& config);
    virtual ~BlockAssembler() = default;

    /** Construct a new block template with coinbase to scriptPubKeyIn */
    virtual std::unique_ptr<CBlockTemplate> CreateNewBlock(const CScript& scriptPubKeyIn, CBlockIndex*& pindexPrev) = 0;

    /** Get the maximum generated block size for the current config and chain tip */
    virtual uint64_t GetMaxGeneratedBlockSize() const = 0;

    /** Get (and reset) whether we might produce an updated template */
    virtual bool GetTemplateUpdated() { return false; }

protected:
    uint64_t ComputeMaxGeneratedBlockSize(const CBlockIndex* pindexPrev) const;

    // Fill in header fields for a new block template
    void FillBlockHeader(CBlockRef& block, const CBlockIndex* pindex, const CScript& scriptPubKeyIn) const;

    // Keep reference to the global config
    const Config& mConfig;

    // Block accounting
    Amount mBlockFees {0};
};

using BlockAssemblerRef = std::shared_ptr<BlockAssembler>;

}

// Free helpers shared by RPC + assembler. Phase 3 (v3.3.0) relocated these from the now-deleted
// mining/legacy.{h,cpp} into assembler.{h,cpp} since they are non-LegacyBlockAssembler-specific
// block-construction utilities.
class CBlockHeader;
class CBlock;

/** Refresh nTime / nBits on a candidate block header from the chain tip. Returns delta in seconds. */
int64_t UpdateTime(CBlockHeader *pblock, const Config &config,
                   const CBlockIndex *pindexPrev);

/** Bump the coinbase extra-nonce field and recompute the coinbase + merkle root. */
void IncrementExtraNonce(CBlock *pblock,
                         const CBlockIndex *pindexPrev,
                         unsigned int &nExtraNonce);

