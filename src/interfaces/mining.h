// Copyright (c) 2024-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INTERFACES_MINING_H
#define BITCOIN_INTERFACES_MINING_H

#include <amount.h>
#include <interfaces/types.h>
#include <node/mining_types.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace node {
struct NodeContext;
} // namespace node

namespace interfaces {

//! Block template interface
class BlockTemplate
{
public:
    virtual ~BlockTemplate() = default;

    virtual CBlockHeader getBlockHeader() = 0;
    virtual CBlock getBlock() = 0;

    virtual std::vector<Amount> getTxFees() = 0;
    virtual std::vector<int64_t> getTxSigops() = 0;

    virtual node::CoinbaseTx getCoinbaseTx() = 0;
    //! Merkle path to the coinbase, ordered from the deepest.
    virtual std::vector<uint256> getCoinbaseMerklePath() = 0;

    //! Construct and broadcast the block. Modifies the template in place.
    virtual bool submitSolution(uint32_t version, uint32_t timestamp, uint32_t nonce, CTransactionRef coinbase) = 0;

    //! Wait for higher fees, a new tip, or timeout. nullptr on timeout.
    virtual std::unique_ptr<BlockTemplate> waitNext(node::BlockWaitOptions options = {}) = 0;

    virtual void interruptWait() = 0;
};

//! Interface giving clients (RPC, Stratum v2 Template Provider) ability to create block templates.
class Mining
{
public:
    virtual ~Mining() = default;

    virtual bool isTestChain() = 0;
    virtual bool isInitialBlockDownload() = 0;
    virtual std::optional<BlockRef> getTip() = 0;

    //! Wait for the tip to differ from current_tip (or for it to connect during init).
    virtual std::optional<BlockRef> waitTipChanged(uint256 current_tip, std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) = 0;

    //! Construct a new block template. cooldown waits for tip/IBD (disable on single-miner regtest).
    virtual std::unique_ptr<BlockTemplate> createNewBlock(const node::BlockCreateOptions& options = {}, bool cooldown = true) = 0;

    virtual void interrupt() = 0;

    //! Check a block's validity (PoW check skippable for externally generated templates).
    virtual bool checkBlock(const CBlock& block, const node::BlockCheckOptions& options, std::string& reason, std::string& debug) = 0;

    //! Process a fully assembled block (like submitblock). true if accepted as new.
    virtual bool submitBlock(const CBlock& block, std::string& reason, std::string& debug) = 0;

    //! Internal node context. Useful for RPC/testing; not accessible across processes.
    virtual const node::NodeContext* context() { return nullptr; }
};

//! Return implementation of Mining interface.
std::unique_ptr<Mining> MakeMining(const node::NodeContext& node, bool wait_loaded = true);

} // namespace interfaces

#endif // BITCOIN_INTERFACES_MINING_H
