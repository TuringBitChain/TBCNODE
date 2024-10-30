#include "mining.h"
#include "context.h"
#include "validation.h"
#include "util/check.h"

class MinerImpl : public Mining
{
public:
    explicit MinerImpl(NodeContext& node) : m_node(node) {}

    // bool isTestChain() override
    // {
    //     return chainman().GetParams().IsTestChain();
    // }

    // bool isInitialBlockDownload() override
    // {
    //     return chainman().IsInitialBlockDownload();
    // }

    std::optional<uint256> getTipHash() override
    {
        LOCK(::cs_main);
        CBlockIndex* tip{chainActive.Tip()};
        if (!tip) return {};
        return tip->GetBlockHash();
    }

    std::pair<uint256, int> waitTipChanged(MillisecondsDouble timeout) override
    {
        uint256 previous_hash{WITH_LOCK(::cs_main, return chainActive.Tip()->GetBlockHash();)};

        auto deadline = std::chrono::steady_clock::now() + timeout;
        {
            WAIT_LOCKMt(g_best_block_mutex, lock);
            while (/*!chainman().m_interrupt &&*/ std::chrono::steady_clock::now() < deadline) {
                auto check_time = std::chrono::steady_clock::now() + std::min(timeout, MillisecondsDouble(1000));
                g_best_block_cv.wait_until(lock, check_time);
                if (g_best_block != previous_hash) break;
                // Obtaining the height here using chainActive.Tip()->nHeight
                // would result in a deadlock, because UpdateTip requires holding cs_main.
            }
        }
        LOCK(::cs_main);
        return std::make_pair(chainActive.Tip()->GetBlockHash(), chainActive.Tip()->nHeight);
    }

    bool waitFeesChanged(MillisecondsDouble timeout, uint256 tip, Amount fee_delta, Amount& fees_before, bool& tip_changed) override
    {
        Assume(getTipHash());
        unsigned int last_mempool_update{context()->mempool->GetTransactionsUpdated()};

        auto deadline = std::chrono::steady_clock::now() + timeout;
        {
            while (/*!chainman().m_interrupt &&*/ std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::min(timeout, MillisecondsDouble(100)));
                if (getTipHash().value() != tip) {
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

    // bool processNewBlock(const std::shared_ptr<const CBlock>& block, bool* new_block) override
    // {
    //     return chainman().ProcessNewBlock(block, /*force_processing=*/true, /*min_pow_checked=*/true, /*new_block=*/new_block);
    // }

    // unsigned int getTransactionsUpdated() override
    // {
    //     return context()->mempool->GetTransactionsUpdated();
    // }

    // bool testBlockValidity(const CBlock& block, bool check_merkle_root, BlockValidationState& state) override
    // {
    //     LOCK(cs_main);
    //     CBlockIndex* tip{chainActive.Tip()};
    //     // Fail if the tip updated before the lock was taken
    //     if (block.hashPrevBlock != tip->GetBlockHash()) {
    //         state.Error("Block does not connect to current chain tip.");
    //         return false;
    //     }

    //     return TestBlockValidity(state, chainman().GetParams(), chainman().ActiveChainstate(), block, tip, /*fCheckPOW=*/false, check_merkle_root);
    // }

    // std::unique_ptr<BlockTemplate> createNewBlock(const CScript& script_pub_key, const BlockCreateOptions& options) override
    // {
    //     BlockAssembler::Options assemble_options{options};
    //     ApplyArgsManOptions(*Assert(m_node.args), assemble_options);
    //     return std::make_unique<BlockTemplateImpl>(BlockAssembler{chainman().ActiveChainstate(), context()->mempool.get(), assemble_options}.CreateNewBlock(script_pub_key), m_node);
    // }

    NodeContext* context() override { return &m_node; }
    //ChainstateManager& chainman() { return *Assert(m_node.chainman); }
    NodeContext& m_node;
};