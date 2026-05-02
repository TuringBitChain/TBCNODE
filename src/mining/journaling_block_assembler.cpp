// Copyright (c) 2019 Bitcoin Association.
// Distributed under the Open TBC software license, see the accompanying file LICENSE.
#include <mining/journaling_block_assembler.h>

#include <chainparams.h>
#include <config.h>
#include <consensus/validation.h>
#include <logging.h>
#include <mining/journal_builder.h>
#include <timedata.h>
#include <txmempool.h>
#include <util.h>
#include <validation.h>
#include <validation/topo_sort.h>  // v2.6.2 修法 Y: TopoSort 兜底防 reorg ancestor race

#include <limits>

using mining::CBlockTemplate;
using mining::JournalingBlockAssembler;

namespace
{
    // Getters for config values
    uint64_t GetMaxTxnBatch()
    {
        return static_cast<uint64_t>(gArgs.GetArg("-jbamaxtxnbatch", JournalingBlockAssembler::DEFAULT_MAX_SLOT_TRANSACTIONS));
    }
    bool GetFillAfterNewBlock()
    {
        return gArgs.GetBoolArg("-jbafillafternewblock", JournalingBlockAssembler::DEFAULT_NEW_BLOCK_FILL);
    }
}

// Construction
JournalingBlockAssembler::JournalingBlockAssembler(const Config& config)
: BlockAssembler{config}, mMaxSlotTransactions{GetMaxTxnBatch()}, mNewBlockFill{GetFillAfterNewBlock()}
{
    // Create a new starting block
    newBlock();
    // Initialise our starting position
    mJournalPos = CJournal::ReadLock{mJournal}.begin();

    // Launch our main worker thread
    future_ = std::async(std::launch::async,
                         &JournalingBlockAssembler::threadBlockUpdate, this);
}

// Destruction
JournalingBlockAssembler::~JournalingBlockAssembler()
{
    promise_.set_value(); // Tell worker to finish
    future_.wait();       // Wait for worker to finish
}


// (Re)read our configuration parameters (for unit testing)
void JournalingBlockAssembler::ReadConfigParameters()
{
    // Get config values
    mMaxSlotTransactions = GetMaxTxnBatch();
    mNewBlockFill = GetFillAfterNewBlock();
}


// Construct a new block template with coinbase to scriptPubKeyIn
std::unique_ptr<CBlockTemplate> JournalingBlockAssembler::CreateNewBlock(const CScript& scriptPubKeyIn, CBlockIndex*& pindexPrev)
{
    CBlockRef block { std::make_shared<CBlock>() };

    // Get tip we're builing on
    LOCK(cs_main);
    CBlockIndex* pindexPrevNew { chainActive.Tip() };

    // v2.6.2 修法 Y：TopoSort 兜底
    //   ★ 问题：reorg 异步 PTV 让子先于父 commit 进 mempool，子 m_parents 字段空（脏），
    //   journal 按 insertion_index 排子在父前 → block.vtx 子在前父在后 →
    //   ConnectBlock 按 vtx 顺序处理子时父 outpoint 不在 chainstate UTXO →
    //   "bad-txns-inputs-missingorspent" → block 自家 reject。
    //   ★ BSV/上游不修（mainnet 算力分散统计自愈）；TBC 测试链算力集中需修。
    //   ★ 实施：CreateNewBlock 一刻对 vtx[1..] 拓扑排序。coinbase vtx[0] 必须保留首位。
    //   ★ 锁层级（review v7 HIGH 修订）：TopoSort + mTxFees/mTxSigOpsCount 重写
    //     必须在 mMtx 内做完，否则跟后台 updateBlock thread race。
    {
        std::unique_lock<std::mutex> lock { mMtx };

        // Get our best block even if the background thread hasn't run for a while
        updateBlock(pindexPrevNew, mNewBlockFill? std::numeric_limits<uint64_t>::max() : mMaxSlotTransactions.load());
        // Copy our current transactions into the block
        block->vtx = mBlockTxns;
        // 注：mBlockTxns[0] 是 newBlock() 用 emplace_back() 创的 dummy nullptr
        //   CTransactionRef，FillBlockHeader 之后会把它替换成真 coinbase。Fix Y
        //   TopoSort 不能 deref vtx[0]（nullptr），所以循环跳 i=0。

        // ★ TopoSort 兜底（在 mMtx 内，跟 mTxFees/mTxSigOpsCount 同保护）
        if (block->vtx.size() > 2) {
            try {
                // 拆出 coinbase 跟其他 tx；vtx[0] 是 newBlock() emplace_back() 的
                //   dummy nullptr placeholder，FillBlockHeader 之后会替换成真 coinbase。
                //   TopoSort 只处理 vtx[1..]，跳过 vtx[0] 防 nullptr deref。
                CTransactionRef coinbase = block->vtx[0];
                std::vector<CTransactionRef> non_coinbase(
                    block->vtx.begin() + 1, block->vtx.end());

                // TopoSort（Kahn 算法，validation/topo_sort.h）— 只对非 coinbase 跑。
                //   non_coinbase 里若仍有 nullptr（防御），TopoSort 内部 GetId 会崩；
                //   但生产路径 mBlockTxns[1..] 都是 addTransaction 加的真 tx，无 nullptr。
                auto sorted = tbc::validation::TopoSort(std::move(non_coinbase));

                // 建立 txid → 老 index 映射（用于重排 fees / sigops）。
                //   i 从 1 开始跳 dummy nullptr coinbase（vtx[0]）。
                std::unordered_map<TxId, size_t, std::hash<TxId>> old_idx_of;
                old_idx_of.reserve(block->vtx.size());
                for (size_t i = 1; i < block->vtx.size(); i++) {
                    if (!block->vtx[i]) continue;  // 防御：mBlockTxns 里若混入 nullptr
                    old_idx_of[block->vtx[i]->GetId()] = i;
                }

                // 重组 vtx + 按新顺序重排 mTxFees / mTxSigOpsCount
                std::vector<CTransactionRef> new_vtx;
                new_vtx.reserve(block->vtx.size());
                new_vtx.push_back(std::move(coinbase));

                std::vector<Amount> new_fees(block->vtx.size());
                std::vector<int64_t> new_sigops(block->vtx.size());
                new_fees[0] = mTxFees.size() > 0 ? mTxFees[0] : Amount(0);
                new_sigops[0] = mTxSigOpsCount.size() > 0 ? mTxSigOpsCount[0] : 0;
                for (size_t i = 0; i < sorted.size(); i++) {
                    new_vtx.push_back(sorted[i]);
                    size_t old_i = old_idx_of[sorted[i]->GetId()];
                    new_fees[i + 1] = (old_i < mTxFees.size()) ? mTxFees[old_i] : Amount(0);
                    new_sigops[i + 1] = (old_i < mTxSigOpsCount.size()) ? mTxSigOpsCount[old_i] : 0;
                }
                block->vtx = std::move(new_vtx);
                mTxFees = std::move(new_fees);
                mTxSigOpsCount = std::move(new_sigops);
            } catch (const tbc::validation::BatchTopoSortError& e) {
                // 极罕见：mempool 含环或重复 txid（不该发生）。保留原顺序让
                // TestBlockValidity 报真错。
                LogPrintf("WARNING JournalingBlockAssembler: TopoSort failed: %s "
                          "(falling back to journal order, block may be rejected)\n",
                          e.what());
            }
        }
    }  // ★ 释放 mMtx

    // Fill in the block header fields
    FillBlockHeader(block, pindexPrevNew, scriptPubKeyIn);

    // If required, check block validity
    if(mConfig.GetTestBlockCandidateValidity())
    {
        CValidationState state {};
        BlockValidationOptions validationOptions { false, false, true };
        if(!TestBlockValidity(mConfig, state, *block, pindexPrevNew, validationOptions))
        {
            throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s",
                                               __func__, FormatStateMessage(state)));
        }
    }

    uint64_t serializeSize { GetSerializeSize(*block, SER_NETWORK, PROTOCOL_VERSION) };
    LogPrintf("JournalingBlockAssembler::CreateNewBlock(): total size: %u txs: %u fees: %ld sigops %d\n",
        serializeSize, block->vtx.size() - 1, mBlockFees, mBlockSigOps);

    bool isGenesisEnabled = IsGenesisEnabled(mConfig, chainActive.Height() + 1);
    bool sigOpCountError;

    // Build template
    std::unique_ptr<CBlockTemplate> blockTemplate { std::make_unique<CBlockTemplate>(block) };
    blockTemplate->vTxFees = mTxFees;
    blockTemplate->vTxSigOpsCount = mTxSigOpsCount;
    blockTemplate->vTxFees[0] = -1 * mBlockFees;

    int64_t txSigOpCount = static_cast<int64_t>(GetSigOpCountWithoutP2SH(*block->vtx[0], isGenesisEnabled, sigOpCountError));
    // This can happen if supplied coinbase scriptPubKeyIn contains multisig with too many public keys
    if (sigOpCountError)
    {
        blockTemplate = nullptr;
    }
    else
    {
        blockTemplate->vTxSigOpsCount[0] = txSigOpCount;
    }
    // Can now update callers pindexPrev
    pindexPrev = pindexPrevNew;
    mRecentlyUpdated = false;

    return blockTemplate;
}

// Get the maximum generated block size for the current config and chain tip
uint64_t JournalingBlockAssembler::GetMaxGeneratedBlockSize() const
{
    LOCK(cs_main);
    return ComputeMaxGeneratedBlockSize(chainActive.Tip());
}

// Thread entry point for block update processing
void JournalingBlockAssembler::threadBlockUpdate() noexcept
{
    try
    {
        LogPrint(BCLog::JOURNAL, "JournalingBlockAssembler thread starting\n");
        const auto future{promise_.get_future()};
        while(true)
        {
            // Run every few seconds or until stopping
            const auto status = future.wait_for(mRunFrequency);
            if(status == std::future_status::timeout)
            {
                // Update block template
                std::unique_lock<std::mutex> lock { mMtx };
                updateBlock(chainActive.Tip(), mMaxSlotTransactions);
            }
            else if(status == std::future_status::ready)
                break;
        }

        LogPrint(BCLog::JOURNAL, "JournalingBlockAssembler thread stopping\n");
    }
    catch(...)
    {
        LogPrint(BCLog::JOURNAL, "Unexpected exception in JournalingBlockAssembler thread\n");
    }
}

// Update our block template with some new transactions - Caller holds mutex
void JournalingBlockAssembler::updateBlock(const CBlockIndex* pindex, uint64_t maxTxns)
{
    uint64_t txnNum {0};

    try
    {
        // Update chain state
        if(pindex)
        {
            int height { pindex->nHeight + 1 };
            mLockTimeCutoff = (StandardNonFinalVerifyFlags(IsGenesisEnabled(mConfig, height)) & LOCKTIME_MEDIAN_TIME_PAST) ?
                pindex->GetMedianTimePast() : GetAdjustedTime();
        }

        // Lock journal to prevent changes while we iterate over it
        CJournal::ReadLock journalLock { mJournal };

        // Does our journal or iterator need replacing?
        while(!mJournal->getCurrent() || !mJournalPos.valid())
        {
            // Release old lock, update journal/block, take new lock
            journalLock = CJournal::ReadLock {};
            newBlock();
            journalLock = CJournal::ReadLock { mJournal };

            // Reset our position to the start of the journal
            mJournalPos = journalLock.begin();
        }

        // Reposition our journal index incase we were previously at the end and now
        // some new additions have arrived.
        mJournalPos.reset();

        // Read and process transactions from the journal until either we've done as many
        // as we allow this go or we reach the end of the journal.
        bool finished { mJournalPos == journalLock.end() };
        while(!finished)
        {
            // Try to add another txn to the block
            if(addTransaction(pindex))
            {
                ++txnNum;

                // We're finished if we've reached the end of the journal, or we've added
                // as many transactions this iteration as we're allowed.
                finished = (mJournalPos == journalLock.end() || txnNum >= maxTxns);
            }
            else
            {
                // We're also finished once we can't add any more transactions.
                finished = true;
            }
        }
    }
    catch(std::exception& e)
    {
        LogPrint(BCLog::JOURNAL, "JournalingBlockAssembler caught: %s\n", e.what());
    }

    if(txnNum > 0)
    {
        LogPrint(BCLog::JOURNAL, "JournalingBlockAssembler processed %llu transactions from the journal\n", txnNum);
    }
}

// Get (and reset) whether we might produce an updated template
bool JournalingBlockAssembler::GetTemplateUpdated()
{
    // Get and reset latch
    return mRecentlyUpdated.exchange(false);
}

// Create a new block for us to start working on - Caller holds mutex
void JournalingBlockAssembler::newBlock()
{
    LogPrint(BCLog::JOURNAL, "JournalingBlockAssembler replacing journal/iterator/block\n");

    // Get new current journal
    mJournal = mempool.getJournalBuilder().getCurrentJournal();

    // Reset transaction list
    mBlockTxns.clear();
    mTxFees.clear();
    mTxSigOpsCount.clear();

    // Reset other accounting information
    mBlockFees = Amount{0};
    mBlockSigOps = COINBASE_SIG_OPS;
    mBlockSize = COINBASE_SIZE;

    // Add dummy coinbase as first transaction
    mBlockTxns.emplace_back();
    mTxFees.emplace_back(Amount{-1});
    mTxSigOpsCount.emplace_back(-1);

    // Set updated flag
    mRecentlyUpdated = true;
}

// Test whether we can add another transaction to the next block, and if
// so do it - Caller holds mutex
bool JournalingBlockAssembler::addTransaction(const CBlockIndex* pindex)
{
    const CJournalEntry& entry { mJournalPos.at() };
    const CTransactionRef& txn { entry.getTxn() };

    // Check for block being full
    uint64_t maxBlockSize { ComputeMaxGeneratedBlockSize(pindex) };
    uint64_t txnSize { txn->GetTotalSize() };
    uint64_t blockSizeWithTx { mBlockSize + txnSize };
    if(blockSizeWithTx >= maxBlockSize)
    {
        return false;
    }

    uint64_t txnSigOps{ static_cast<uint64_t>(entry.getSigOpsCount()) };
    uint64_t blockSigOpsWithTx{ mBlockSigOps + txnSigOps };

    // After Genesis we don't count sigops anymore
    if (!IsGenesisEnabled(mConfig, pindex->nHeight + 1))
    {
        // Check sig ops count
        uint64_t maxBlockSigOps = mConfig.GetMaxBlockSigOpsConsensusBeforeGenesis(blockSizeWithTx);
     
        if (blockSigOpsWithTx >= maxBlockSigOps)
        {
            return false;
        }
    }

    // Must check that lock times are still valid
    if(pindex)
    {
        CValidationState state {};
        if(!ContextualCheckTransaction(mConfig, *txn, state, pindex->nHeight + 1, mLockTimeCutoff, false))
        {
            return false;
        }
    }

    // Append next txn to the block template
    mBlockTxns.emplace_back(txn);
    mTxFees.emplace_back(entry.getFee());
    mTxSigOpsCount.emplace_back(entry.getSigOpsCount());

    // Update block accounting details
    mBlockSize = blockSizeWithTx;
    mBlockSigOps = blockSigOpsWithTx;
    mBlockFees += entry.getFee();

    // Set updated flag
    mRecentlyUpdated = true;

    // Move to the next item in the journal
    ++mJournalPos;

    return true;
}

