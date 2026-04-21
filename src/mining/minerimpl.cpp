#include "mining.h"
#include "assembler.h"
#include "context.h"
#include "factory.h"
#include "consensus/merkle.h"
#include "consensus/validation.h"
#include "validation.h"
#include "util/check.h"

class BlockTemplateImpl : public BlockTemplate
{
public:
    explicit BlockTemplateImpl(std::unique_ptr<mining::CBlockTemplate> block_template, const Config& config)
        : m_block_template(std::move(block_template)), m_config(config)
    {
        assert(m_block_template);
    }

    CBlockHeader getBlockHeader() override
    {
        return *m_block_template->GetBlockRef();
    }

    std::shared_ptr<CBlock> getBlockRef() override
    {
        return m_block_template->GetBlockRef();
    }

    CBlock getBlock() override
    {
        return *m_block_template->GetBlockRef();
    }

    std::vector<Amount> getTxFees() override
    {
        return m_block_template->vTxFees;
    }

    std::vector<int64_t> getTxSigops() override
    {
        return m_block_template->vTxSigOpsCount;
    }

    CTransactionRef getCoinbaseTx() override
    {
        return m_block_template->GetBlockRef()->vtx[0];
    }

    int getWitnessCommitmentIndex() override
    {
        return GetWitnessCommitmentIndex(m_block_template->GetBlockRef());
    }

    std::vector<uint256> getCoinbaseMerklePath() override
    {
        return BlockMerkleBranch(*m_block_template->GetBlockRef(), 0);
    }

    bool submitSolution(uint32_t version, uint32_t timestamp, uint32_t nonce, CTransactionRef coinbase) override
    {
        auto block = m_block_template->GetBlockRef();
        if (block->vtx.empty()) {
            block->vtx.push_back(std::move(coinbase));
        } else {
            block->vtx[0] = std::move(coinbase);
        }
        block->nVersion = version;
        block->nTime = timestamp;
        block->nNonce = nonce;
        block->hashMerkleRoot = BlockMerkleRoot(*block);
        return ProcessNewBlock(m_config, block, true, nullptr);
    }

private:
    const std::unique_ptr<mining::CBlockTemplate> m_block_template;
    const Config& m_config;
};

class MinerImpl : public Mining
{
public:
    explicit MinerImpl(NodeContext& node, const Config& config) : m_node(node), m_config(config) {}

    std::optional<BlockRef> getTip() override
    {
        LOCK(::cs_main);
        CBlockIndex* tip{chainActive.Tip()};
        if (!tip) return std::nullopt;
        return BlockRef{tip->GetBlockHash(), tip->nHeight};
    }

    std::optional<BlockRef> waitTipChanged(uint256 current_tip, MillisecondsDouble timeout) override
    {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        {
            WAIT_LOCKMt(g_best_block_mutex, lock);
            while (std::chrono::steady_clock::now() < deadline) {
                auto check_time = std::chrono::steady_clock::now() + std::min(timeout, MillisecondsDouble(1000));
                g_best_block_cv.wait_until(lock, check_time);
                if (uint256() != g_best_block && g_best_block != current_tip) {
                    break;
                }
            }
        }
        LOCK(::cs_main);
        CBlockIndex* tip{chainActive.Tip()};
        if (!tip) return std::nullopt;
        return BlockRef{tip->GetBlockHash(), tip->nHeight};
    }

    bool waitFeesChanged(MillisecondsDouble timeout, uint256 tip, Amount fee_delta, Amount& fees_before, bool& tip_changed) override
    {
        Assume(getTip());
        unsigned int last_mempool_update{context()->mempool->GetTransactionsUpdated()};

        auto deadline = std::chrono::steady_clock::now() + timeout;
        {
            while (std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::min(timeout, MillisecondsDouble(100)));
                if (getTip()->hash != tip) {
                    tip_changed = true;
                    return false;
                }

                // TODO: when cluster mempool is available, actually calculate
                // fees for the next block. This is currently too expensive.
                if (context()->mempool->GetTransactionsUpdated() > last_mempool_update) return true;
            }
        }
        return false;
    }

    std::unique_ptr<BlockTemplate> createNewBlock(const CScript& script_pub_key) override
    {
        if(!mining::g_miningFactory) {
            return nullptr;
        }
        CBlockIndex* pindexPrev = nullptr;
        auto pblocktemplate = mining::g_miningFactory->GetAssembler()->CreateNewBlock(script_pub_key, pindexPrev);
        if (!pblocktemplate) {
            return nullptr;
        }
        return std::make_unique<BlockTemplateImpl>(std::move(pblocktemplate), m_config);
    }

    bool checkBlock(const CBlock& block, const BlockCheckOptions& options, std::string& reason, std::string& debug) override
    {
        LOCK(::cs_main);
        CBlockIndex* tip{chainActive.Tip()};
        if (block.hashPrevBlock != tip->GetBlockHash()) {
            reason = "bad-prevblk";
            return false;
        }
        CValidationState state;
        BlockValidationOptions validationOptions{options.check_pow, options.check_merkle_root};
        bool res = TestBlockValidity(m_config, state, block, tip, validationOptions);
        reason = state.GetRejectReason();
        debug = state.GetDebugMessage();
        return res;
    }

    NodeContext* context() override { return &m_node; }
    NodeContext& m_node;
    const Config& m_config;
};

std::unique_ptr<Mining> MakeMining(NodeContext& node, const Config& config)
{
    return std::make_unique<MinerImpl>(node, config);
}
