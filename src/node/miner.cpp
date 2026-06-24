// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/miner.h>

#include <chain.h>                 // CBlockIndex, mapBlockIndex
#include <config.h>
#include <consensus/validation.h>  // CValidationState
#include <primitives/transaction.h>
#include <sync.h>
#include <validation.h>            // chainActive, cs_main, csBestBlock, cvBlockChange, ProcessNewBlock
#include <validationinterface.h>

#include <boost/thread.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

namespace node {

std::optional<interfaces::BlockRef> GetTip()
{
    LOCK(cs_main);
    const CBlockIndex* tip = chainActive.Tip();
    if (!tip) return std::nullopt;
    return interfaces::BlockRef{tip->GetBlockHash(), tip->nHeight};
}

std::optional<interfaces::BlockRef> WaitTipChanged(uint256 current_tip,
                                                   std::chrono::milliseconds timeout,
                                                   std::atomic<bool>& interrupt)
{
    const bool wait_forever = (timeout == std::chrono::milliseconds::max());
    boost::system_time deadline = boost::get_system_time();
    if (!wait_forever) deadline += boost::posix_time::milliseconds(timeout.count());

    {
        // csBestBlock is the mutex paired with cvBlockChange (see rpc/mining.cpp longpoll).
        // The tip is read locklessly inside the wait, mirroring that established pattern.
        boost::unique_lock<boost::mutex> lock(csBestBlock);
        while (!interrupt.load()) {
            const CBlockIndex* tip = chainActive.Tip();
            if (tip && tip->GetBlockHash() != current_tip) break;
            if (wait_forever) {
                cvBlockChange.wait(lock);
            } else if (!cvBlockChange.timed_wait(lock, deadline)) {
                break; // timeout
            }
        }
    }

    if (interrupt.load()) return std::nullopt;
    return GetTip();
}

namespace {
//! Captures the validation result for a submitted block (mirrors rpc/mining.cpp's catcher).
class SubmitBlockStateCatcher final : public CValidationInterface {
public:
    explicit SubmitBlockStateCatcher(const uint256& hash) : m_hash(hash) {}
    uint256 m_hash;
    bool m_found{false};
    CValidationState m_state;
protected:
    void BlockChecked(const CBlock& block, const CValidationState& state) override {
        if (block.GetHash() != m_hash) return;
        m_found = true;
        m_state = state;
    }
};
} // namespace

bool SubmitBlockHelper(const Config& config, const std::shared_ptr<const CBlock>& block,
                       std::string& reason, std::string& debug)
{
    {
        LOCK(cs_main);
        if (mapBlockIndex.count(block->GetHash())) { reason = "duplicate"; return false; }
    }

    SubmitBlockStateCatcher catcher(block->GetHash());
    RegisterValidationInterface(&catcher);
    bool new_block{false};
    const bool accepted = ProcessNewBlock(config, block, /*fForceProcessing=*/true, &new_block);
    UnregisterValidationInterface(&catcher);

    if (!new_block && accepted) { reason = "duplicate"; return false; }
    if (!catcher.m_found) { reason = "inconclusive"; return false; }
    if (!catcher.m_state.IsValid()) {
        reason = catcher.m_state.GetRejectReason();
        debug = catcher.m_state.GetDebugMessage();
        return false;
    }
    return accepted && new_block;
}

CoinbaseTx BuildCoinbaseTx(const CBlock& block, const Amount& reward_remaining)
{
    const CTransaction& cb = *block.vtx[0];
    CoinbaseTx out;
    out.version = cb.nVersion;
    out.sequence = cb.vin[0].nSequence;
    out.script_sig_prefix = cb.vin[0].scriptSig;
    out.block_reward_remaining = reward_remaining;
    out.required_outputs = {}; // TBC mandates no trailing coinbase outputs.
    out.lock_time = cb.nLockTime;
    return out;
}

} // namespace node
