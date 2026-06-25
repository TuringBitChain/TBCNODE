// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_MINER_H
#define BITCOIN_NODE_MINER_H

#include <amount.h>
#include <interfaces/types.h>
#include <node/mining_types.h>
#include <primitives/block.h>
#include <uint256.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>

class Config;
class CChainParams;

namespace node {

//! True iff the given chain is a test chain (anything but mainnet), mirroring
//! upstream CChainParams::IsTestChain. Used by interfaces::Mining::isTestChain.
bool IsTestChain(const CChainParams& params);

//! How many blocks the best header leads the active tip by, or nullopt when the tip is not
//! behind. The cooldown gate for createNewBlock() (mirrors ChainstateManager::BlocksAheadOfTip).
std::optional<int> BlocksAheadOfTip(int best_header_height, int tip_height);

//! Block while the best header still leads the tip (post-IBD catch-up), returning true when the
//! tip has caught up / the cooldown window expires, or false on interrupt. Mirrors upstream
//! node::CooldownIfHeadersAhead.
bool CooldownIfHeadersAhead(std::atomic<bool>& interrupt);

//! Current chain tip as a BlockRef, or nullopt if no tip.
std::optional<interfaces::BlockRef> GetTip();

//! Block until the tip differs from current_tip, or interrupt, or timeout.
//! Mirrors the rpc/mining.cpp longpoll wait (csBestBlock + cvBlockChange).
std::optional<interfaces::BlockRef> WaitTipChanged(uint256 current_tip,
                                                   std::chrono::milliseconds timeout,
                                                   std::atomic<bool>& interrupt);

//! Submit a fully assembled block via ProcessNewBlock, capturing the BIP22 result.
//! Returns true iff accepted as a new block; sets reason/debug otherwise.
bool SubmitBlockHelper(const Config& config, const std::shared_ptr<const CBlock>& block,
                       std::string& reason, std::string& debug);

//! Extract the coinbase-template fields from a freshly created block.
CoinbaseTx BuildCoinbaseTx(const CBlock& block, const Amount& reward_remaining);

} // namespace node

#endif // BITCOIN_NODE_MINER_H
