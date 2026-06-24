// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/mining.h>

#include <chain.h>
#include <config.h>
#include <consensus/merkle.h>      // ComputeMerkleRoot, ComputeMerkleBranch
#include <consensus/validation.h>  // CValidationState
#include <mining/assembler.h>      // CBlockTemplate
#include <mining/factory.h>        // mining::g_miningFactory
#include <node/context.h>
#include <node/miner.h>
#include <primitives/block.h>
#include <script/script.h>
#include <sync.h>
#include <validation.h>

#include <atomic>
#include <memory>
#include <vector>

using interfaces::BlockRef;
using interfaces::BlockTemplate;
using interfaces::Mining;

namespace node {
namespace {

class BlockTemplateImpl : public BlockTemplate
{
public:
    BlockTemplateImpl(const Config& config, std::unique_ptr<mining::CBlockTemplate> tmpl)
        : m_config(config), m_template(std::move(tmpl)) {}

    CBlockHeader getBlockHeader() override { return m_template->GetBlockRef()->GetBlockHeader(); }
    CBlock getBlock() override { return *m_template->GetBlockRef(); }

    std::vector<Amount> getTxFees() override
    {
        // Element 0 is the coinbase placeholder (-fees); drop it like the RPC does.
        auto fees = m_template->vTxFees;
        if (!fees.empty()) fees.erase(fees.begin());
        return fees;
    }
    std::vector<int64_t> getTxSigops() override
    {
        auto ops = m_template->vTxSigOpsCount;
        if (!ops.empty()) ops.erase(ops.begin());
        return ops;
    }

    CoinbaseTx getCoinbaseTx() override
    {
        const CBlock& block = *m_template->GetBlockRef();
        const Amount reward = block.vtx[0]->vout.empty() ? Amount(0)
                                                         : block.vtx[0]->vout[0].nValue;
        return BuildCoinbaseTx(block, reward);
    }

    std::vector<uint256> getCoinbaseMerklePath() override
    {
        const CBlock& block = *m_template->GetBlockRef();
        std::vector<uint256> leaves;
        leaves.reserve(block.vtx.size());
        for (const auto& tx : block.vtx) leaves.push_back(tx->GetHash());
        return ComputeMerkleBranch(leaves, 0);
    }

    bool submitSolution(uint32_t version, uint32_t timestamp, uint32_t nonce,
                        CTransactionRef coinbase) override
    {
        CBlockRef base = m_template->GetBlockRef();
        auto block = std::make_shared<CBlock>(base->GetBlockHeader());
        block->vtx = base->vtx;
        if (coinbase) block->vtx[0] = coinbase;
        block->nVersion = version;
        block->nTime = timestamp;
        block->nNonce = nonce;

        std::vector<uint256> leaves;
        leaves.reserve(block->vtx.size());
        for (const auto& tx : block->vtx) leaves.push_back(tx->GetHash());
        block->hashMerkleRoot = ComputeMerkleRoot(leaves);
        block->fChecked = false;

        std::string reason, debug;
        return SubmitBlockHelper(m_config, std::shared_ptr<const CBlock>(block), reason, debug);
    }

    std::unique_ptr<BlockTemplate> waitNext(BlockWaitOptions options) override
    {
        // M1: wait for a new tip, then build a fresh template. (Fee-threshold-only
        // wakeups are a later refinement; tip change is the primary trigger.)
        const uint256 prev = m_template->GetBlockRef()->hashPrevBlock;
        auto after = WaitTipChanged(prev, options.timeout, m_interrupt);
        if (!after) return nullptr;

        CBlockIndex* pindexPrev{nullptr};
        auto fresh = mining::g_miningFactory->GetAssembler()->CreateNewBlock(
            CScript() << OP_TRUE, pindexPrev);
        if (!fresh) return nullptr;
        return std::make_unique<BlockTemplateImpl>(m_config, std::move(fresh));
    }

    void interruptWait() override { m_interrupt = true; cvBlockChange.notify_all(); }

private:
    const Config& m_config;
    const std::unique_ptr<mining::CBlockTemplate> m_template;
    std::atomic<bool> m_interrupt{false};
};

class MinerImpl : public Mining
{
public:
    explicit MinerImpl(const NodeContext& node) : m_node(node) {}

    bool isTestChain() override { return Params().MineBlocksOnDemand(); }
    bool isInitialBlockDownload() override { return IsInitialBlockDownload(); }
    std::optional<BlockRef> getTip() override { return GetTip(); }

    std::optional<BlockRef> waitTipChanged(uint256 current_tip,
                                           std::chrono::milliseconds timeout) override
    {
        return WaitTipChanged(current_tip, timeout, m_interrupt);
    }

    std::unique_ptr<BlockTemplate> createNewBlock(const BlockCreateOptions& options,
                                                  bool /*cooldown*/) override
    {
        CScript spk = options.coinbase_output_script;
        CBlockIndex* pindexPrev{nullptr};
        auto tmpl = mining::g_miningFactory->GetAssembler()->CreateNewBlock(spk, pindexPrev);
        if (!tmpl) return nullptr;
        return std::make_unique<BlockTemplateImpl>(config(), std::move(tmpl));
    }

    void interrupt() override { m_interrupt = true; cvBlockChange.notify_all(); }

    bool checkBlock(const CBlock& block, const BlockCheckOptions& options,
                    std::string& reason, std::string& debug) override
    {
        LOCK(cs_main);
        CValidationState state;
        CBlockIndex* pindexPrev = chainActive.Tip();
        BlockValidationOptions validation_options(options.check_pow, options.check_merkle_root);
        const bool ok = TestBlockValidity(config(), state, block, pindexPrev, validation_options);
        if (!ok) { reason = state.GetRejectReason(); debug = state.GetDebugMessage(); }
        return ok;
    }

    bool submitBlock(const CBlock& block, std::string& reason, std::string& debug) override
    {
        auto shared = std::make_shared<const CBlock>(block);
        return SubmitBlockHelper(config(), shared, reason, debug);
    }

    const NodeContext* context() override { return &m_node; }

private:
    const Config& config() const { return *m_node.config; }
    const NodeContext& m_node;
    std::atomic<bool> m_interrupt{false};
};

} // namespace
} // namespace node

namespace interfaces {
std::unique_ptr<Mining> MakeMining(const node::NodeContext& node, bool /*wait_loaded*/)
{
    return std::make_unique<node::MinerImpl>(node);
}
} // namespace interfaces
