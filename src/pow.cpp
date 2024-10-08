// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2017 The Bitcoin developers
// Copyright (c) 2024 TBCNODE DEV GROUP
// Distributed under the Open TBC software license, see the accompanying file LICENSE.

#include "pow.h"

#include "arith_uint256.h"
#include "chain.h"
#include "chainparams.h"
#include "config.h"
#include "consensus/params.h"
#include "primitives/block.h"
#include "uint256.h"
#include "util.h"
#include "validation.h"

void SortingNetwork(const CBlockIndex *pindex,const int32_t selectSortingNum,std::map<uint32_t,const CBlockIndex*> &mapBlocks){
    assert(selectSortingNum%2);
    assert(pindex->nHeight >= selectSortingNum);
    
    std::map<uint32_t,int32_t>  mapIndex;
    auto*                       pOperateIndex   = pindex;
    int32_t                     bias            = selectSortingNum;
    int32_t                     num             = 0;

    for(; num < selectSortingNum; num++)
    {
        auto tmpNum     = pOperateIndex->nTime * bias;
        auto mIndexit   = mapIndex.find(tmpNum);
        if(mapIndex.end() == mIndexit)
        {
            mapIndex[tmpNum] = 0;
        }
        else
        {
            mapIndex[tmpNum]++;
        }
        mapBlocks[tmpNum + mapIndex[tmpNum]]    = pOperateIndex;
        pOperateIndex                           = pOperateIndex->pprev;

    }
}

static const CBlockIndex *GetSuitableBlock(const CBlockIndex *pindex, const int32_t freeSelectOddBlocks) {
    assert(freeSelectOddBlocks%2);
    assert(pindex->nHeight >= freeSelectOddBlocks);

    std::map<uint32_t,const CBlockIndex*>   mapBlocks;
    SortingNetwork(pindex,freeSelectOddBlocks,mapBlocks);

    auto it = mapBlocks.begin();
    std::advance(it,mapBlocks.size()/2);

    return it->second;
}

static uint64_t GetPromisedBlocks(const CBlockIndex *pindexPrev, 
            const int backNum, const Consensus::Params &params ){
    const uint32_t      nHeight         = pindexPrev->nHeight;
    uint32_t            nHeightFirst    = nHeight - backNum;
    const CBlockIndex * pindexFirst     = pindexPrev->GetAncestor(nHeightFirst);

    uint64_t time   = pindexPrev->nTime - pindexFirst->nTime;
    uint64_t nBlocks = time / params.nPowTargetSpacing;

    using T = decltype(time);

    return nBlocks;
}

static uint64_t GetNewBlockSpacing(const CBlockIndex *pindexPrev, 
            const uint64_t backNum, const Consensus::Params &params ){
    uint64_t nPromisedBlocks = GetPromisedBlocks(pindexPrev,backNum,params);
    uint64_t NewBlockSpacing;
    if ( nPromisedBlocks > backNum ){
        nPromisedBlocks = (nPromisedBlocks > backNum*2)? (backNum*2) : nPromisedBlocks; 
        NewBlockSpacing = params.nPowTargetSpacing*backNum/nPromisedBlocks;
    }
    else{
        NewBlockSpacing = params.nPowTargetSpacing;
    }
    return NewBlockSpacing;
}

/**
 * Compute the next required proof of work using the legacy Bitcoin difficulty
 * adjustment + Emergency Difficulty Adjustment (EDA).
 */
static uint32_t GetNextEDAWorkRequired(const CBlockIndex *pindexPrev,
                                       const CBlockHeader *pblock,
                                       const Config &config) {
    const Consensus::Params &params = config.GetChainParams().GetConsensus();

    // Only change once per difficulty adjustment interval
    uint32_t nHeight = pindexPrev->nHeight + 1;
    if (nHeight % params.DifficultyAdjustmentInterval() == 0) {
        // Go back by what we want to be 14 days worth of blocks
        assert(nHeight >= params.DifficultyAdjustmentInterval());
        uint32_t nHeightFirst = nHeight - params.DifficultyAdjustmentInterval();
        const CBlockIndex *pindexFirst = pindexPrev->GetAncestor(nHeightFirst);
        assert(pindexFirst);

        return CalculateNextWorkRequired(pindexPrev,
                                         pindexFirst->GetBlockTime(), config);
    }

    const uint32_t nProofOfWorkLimit =
        UintToArith256(params.powLimit).GetCompact();

    if (params.fPowAllowMinDifficultyBlocks) {
        // Special difficulty rule for testnet:
        // If the new block's timestamp is more than 2* 10 minutes then allow
        // mining of a min-difficulty block.
        if (pblock->GetBlockTime() >
            pindexPrev->GetBlockTime() + 2 * params.nPowTargetSpacing) {
            return nProofOfWorkLimit;
        }

        // Return the last non-special-min-difficulty-rules-block
        const CBlockIndex *pindex = pindexPrev;
        while (pindex->pprev &&
               pindex->nHeight % params.DifficultyAdjustmentInterval() != 0 &&
               pindex->nBits == nProofOfWorkLimit) {
            pindex = pindex->pprev;
        }

        return pindex->nBits;
    }

    // We can't go below the minimum, so bail early.
    uint32_t nBits = pindexPrev->nBits;
    if (nBits == nProofOfWorkLimit) {
        return nProofOfWorkLimit;
    }

    // If producing the last 6 blocks took less than 12h, we keep the same
    // difficulty.
    const CBlockIndex *pindex6 = pindexPrev->GetAncestor(nHeight - 7);
    assert(pindex6);
    int64_t mtp6blocks =
        pindexPrev->GetMedianTimePast() - pindex6->GetMedianTimePast();
    if (mtp6blocks < 12 * 3600) {
        return nBits;
    }

    // If producing the last 6 blocks took more than 12h, increase the
    // difficulty target by 1/4 (which reduces the difficulty by 20%).
    // This ensures that the chain does not get stuck in case we lose
    // hashrate abruptly.
    arith_uint256 nPow;
    nPow.SetCompact(nBits);
    nPow += (nPow >> 2);

    // Make sure we do not go below allowed values.
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    if (nPow > bnPowLimit) nPow = bnPowLimit;

    return nPow.GetCompact();
}

uint32_t GetNextWorkRequired(const CBlockIndex *pindexPrev,
                             const CBlockHeader *pblock, const Config &config) {
    if(824188 == pindexPrev->nHeight){
        return 0x1d00ffff;
    }
    const Consensus::Params &params = config.GetChainParams().GetConsensus();

    // Genesis block
    if (pindexPrev == nullptr) {
        return UintToArith256(params.powLimit).GetCompact();
    }

    // Special rule for regtest: we never retarget.
    if (params.fPowNoRetargeting) {
        return pindexPrev->nBits;
    }

    if (IsDAAEnabled(config, pindexPrev)) {
        return GetNextCashWorkRequired(pindexPrev, pblock, config);
    }

    return GetNextEDAWorkRequired(pindexPrev, pblock, config);
}

uint32_t CalculateNextWorkRequired(const CBlockIndex *pindexPrev,
                                   int64_t nFirstBlockTime,
                                   const Config &config) {
    const Consensus::Params &params = config.GetChainParams().GetConsensus();

    if (params.fPowNoRetargeting) {
        return pindexPrev->nBits;
    }

    // Limit adjustment step
    int64_t nActualTimespan = pindexPrev->GetBlockTime() - nFirstBlockTime;
    if (nActualTimespan < params.nPowTargetTimespan / 4) {
        nActualTimespan = params.nPowTargetTimespan / 4;
    }

    if (nActualTimespan > params.nPowTargetTimespan * 4) {
        nActualTimespan = params.nPowTargetTimespan * 4;
    }

    // Retarget
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    arith_uint256 bnNew;
    bnNew.SetCompact(pindexPrev->nBits);
    bnNew *= nActualTimespan;
    bnNew /= params.nPowTargetTimespan;

    if (bnNew > bnPowLimit) bnNew = bnPowLimit;

    return bnNew.GetCompact();
}

bool CheckProofOfWork(uint256 hash, uint32_t nBits, const Config &config) {
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow ||
        bnTarget >
            UintToArith256(config.GetChainParams().GetConsensus().powLimit)) {
        return false;
    }

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget) {
        return false;
    }

    return true;
}

/**
 * Compute a target based on the work done between 2 blocks and the time
 * required to produce that work.
 */
static arith_uint256 ComputeTarget(const CBlockIndex *pindexFirst,
                                   const CBlockIndex *pindexLast,
                                   const int64_t nPowTargetSpacing) {
    assert(pindexLast->nHeight > pindexFirst->nHeight);

    /**
     * From the total work done and the time it took to produce that much work,
     * we can deduce how much work we expect to be produced in the targeted time
     * between blocks.
     */
    arith_uint256 work = pindexLast->nChainWork - pindexFirst->nChainWork;
    work *= nPowTargetSpacing;

    // In order to avoid difficulty cliffs, we bound the amplitude of the
    // adjustment we are going to do to a factor in [0.5, 2].
    int64_t nActualTimespan =
        int64_t(pindexLast->nTime) - int64_t(pindexFirst->nTime);

    if (nActualTimespan > 288 * nPowTargetSpacing) {
        nActualTimespan = 288 * nPowTargetSpacing;
    } else if (nActualTimespan < 72 * nPowTargetSpacing) {
        nActualTimespan = 72 * nPowTargetSpacing;
    }
    // or 
    // if (nActualTimespan < 72 * nPowTargetSpacing) {
    //     nActualTimespan = 72 * nPowTargetSpacing;
    // }
    work /= nActualTimespan;

    arith_uint256 result = (-work) / work ;

    /**
     * We need to compute T = (2^256 / W) - 1 but 2^256 doesn't fit in 256 bits.
     * By expressing 1 as W / W, we get (2^256 - W) / W, and we can compute
     * 2^256 - W as the complement of W.
     */
    // return (-work) / work;
    return result;
}

/**
 * To reduce the impact of timestamp manipulation, we select the block we are
 * basing our computation on via a median of 3.
 */
static const CBlockIndex *GetSuitableBlock(const CBlockIndex *pindex) {
    assert(pindex->nHeight >= 3);

    /**
     * In order to avoid a block with a very skewed timestamp having too much
     * influence, we select the median of the 3 top most blocks as a starting
     * point.
     */
    const CBlockIndex *blocks[3];
    blocks[2] = pindex;
    blocks[1] = pindex->pprev;
    blocks[0] = blocks[1]->pprev;

    // Sorting network.
    if (blocks[0]->nTime > blocks[2]->nTime) {
        std::swap(blocks[0], blocks[2]);
    }

    if (blocks[0]->nTime > blocks[1]->nTime) {
        std::swap(blocks[0], blocks[1]);
    }

    if (blocks[1]->nTime > blocks[2]->nTime) {
        std::swap(blocks[1], blocks[2]);
    }

    // We should have our candidate in the middle now.
    return blocks[1];
}

/**
 * Compute the next required proof of work using a weighted average of the
 * estimated hashrate per block.
 *
 * Using a weighted average ensure that the timestamp parameter cancels out in
 * most of the calculation - except for the timestamp of the first and last
 * block. Because timestamps are the least trustworthy information we have as
 * input, this ensures the algorithm is more resistant to malicious inputs.
 */
uint32_t GetNextCashWorkRequired(const CBlockIndex *pindexPrev,
                                 const CBlockHeader *pblock,
                                 const Config &config) {
    const Consensus::Params &params = config.GetChainParams().GetConsensus();

    // This cannot handle the genesis block and early blocks in general.
    assert(pindexPrev);

    // Special difficulty rule for testnet:
    // If the new block's timestamp is more than 2* 10 minutes then allow
    // mining of a min-difficulty block.
    if (params.fPowAllowMinDifficultyBlocks &&
        (pblock->GetBlockTime() >
         pindexPrev->GetBlockTime() + 2 * params.nPowTargetSpacing)) {
        return UintToArith256(params.powLimit).GetCompact();
    }

    // Compute the difficulty based on the full adjustment interval.
    const uint32_t nHeight = pindexPrev->nHeight;
    assert(nHeight >= params.DifficultyAdjustmentInterval());

    // Get the last suitable block of the difficulty interval.
    const CBlockIndex *pindexLast = GetSuitableBlock(pindexPrev);
    assert(pindexLast);

    // Get the first suitable block of the difficulty interval.
    uint32_t nHeightFirst = nHeight - 144;
    const CBlockIndex *pindexFirst =
        GetSuitableBlock(pindexPrev->GetAncestor(nHeightFirst));
    assert(pindexFirst);

    const arith_uint256 powLimit = UintToArith256(params.powLimit);
    uint64_t NewBlockSpacing;

    if ( pindexPrev->nHeight >= 824189 ){
        NewBlockSpacing = GetNewBlockSpacing(pindexPrev,8064,params);

        arith_uint256 prevTarget;
        arith_uint256 prevTargetUpLimit;
        arith_uint256 prevTargetDnLimit;
        prevTarget.SetCompact(pindexPrev->nBits);
        prevTargetUpLimit = prevTarget + (prevTarget>>4);
        if (prevTargetUpLimit >= powLimit) {
            prevTargetUpLimit = powLimit;
        }
        prevTargetDnLimit = prevTarget - (prevTarget>>4);

        // Compute the target based on time and work done during the interval.
        arith_uint256 nextTarget =
            ComputeTarget(pindexFirst, pindexLast, NewBlockSpacing );

        if ( nextTarget > prevTargetUpLimit ){
            nextTarget = prevTargetUpLimit;
        }
        else {
            const CBlockIndex *pindex12 = pindexPrev->GetAncestor(pindexPrev->nHeight - 12);
            assert(pindex12);
            int64_t mtp12blocks = pindexPrev->GetMedianTimePast() - pindex12->GetMedianTimePast();

            if (mtp12blocks > 6 * 3600) { 
                nextTarget = prevTargetUpLimit;
            }
            else {
                if ( nextTarget < prevTargetDnLimit ){
                    nextTarget = prevTargetDnLimit;
                }
            }

        }
        return nextTarget.GetCompact();

    }
    else{
    NewBlockSpacing = params.nPowTargetSpacing;

    // Compute the target based on time and work done during the interval.
    const arith_uint256 nextTarget =
        ComputeTarget(pindexFirst, pindexLast, NewBlockSpacing );

    const arith_uint256 powLimit = UintToArith256(params.powLimit);
    if (nextTarget > powLimit) {
        return powLimit.GetCompact();
    }

    return nextTarget.GetCompact();

    }
}
