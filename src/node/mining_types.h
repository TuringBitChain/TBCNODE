// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_MINING_TYPES_H
#define BITCOIN_NODE_MINING_TYPES_H

#include <amount.h>                  // Amount, CFeeRate, MAX_MONEY
#include <primitives/transaction.h>  // CTxOut
#include <script/script.h>           // CScript, OP_TRUE
#include <uint256.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace node {

//! Block template creation options. Override node defaults; cannot exceed node limits.
struct BlockCreateOptions {
    //! Set false to omit mempool transactions.
    bool use_mempool{true};
    //! RESERVED (TBC): not a per-call option. TBC's JournalingBlockAssembler reads -blockmintxfee
    //! / -printpriority from Config/gArgs itself (no MergeMiningOptions layer; cf. spec §1A #3),
    //! so these are currently unwired. Slated to be dropped in the M3 schema audit.
    std::optional<CFeeRate> block_min_fee_rate{};
    std::optional<bool> print_modified_fee{};
    //! Script to put in the coinbase. Default is anyone-can-spend (tests only).
    CScript coinbase_output_script{CScript() << OP_TRUE};
    //! Whether to call TestBlockValidity() at the end of CreateNewBlock (tests/benches).
    bool test_block_validity{true};
    // TBC spec-§5 audit: dropped block_reserved_weight / block_max_weight (no weight model;
    // max block size comes from Config) and coinbase_output_max_additional_sigops
    // (TBC sigops are per-MB / Genesis-based, not a fixed coinbase budget).
};

struct BlockWaitOptions {
    //! How long to wait before returning nothing. Default: forever.
    std::chrono::milliseconds timeout{std::chrono::milliseconds::max()};
    //! Only return a new template if fees rise by at least this, or the tip changes.
    Amount fee_threshold{MAX_MONEY};
};

struct BlockCheckOptions {
    bool check_merkle_root{true};
    bool check_pow{true};
};

//! Coinbase fields set by the miner code. Clients add their own outputs and may expand scriptSig.
struct CoinbaseTx {
    uint32_t version;
    uint32_t sequence;
    //! Prefix for the scriptSig (BIP34 height push). Clients append extranonce data.
    CScript script_sig_prefix;
    //! Block subsidy plus fees, minus any non-zero required_outputs.
    Amount block_reward_remaining;
    //! Mandatory trailing coinbase outputs. Currently empty in TBC.
    std::vector<CTxOut> required_outputs;
    uint32_t lock_time;
    // TBC spec-§5 audit: dropped `witness` (no segwit/witness in TBC).
};

} // namespace node

#endif // BITCOIN_NODE_MINING_TYPES_H
