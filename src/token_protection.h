// Copyright (c) 2026 The TBC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "primitives/transaction.h"
#include "script/script.h"
#include "uint256.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

/**
 * Mempool token / PoolNFT 2.0 protection checks (mempool admission policy
 * only; does not affect block consensus).
 *
 * Triple protection:
 *   1. Amount fields in FT/LP/StableCoin tapes must be non-negative;
 *   2. Token amount conservation for transfer transactions, accounted per
 *      contract partial_hash;
 *   3. PoolNFT tape must not be tampered with, except for the amount field.
 */
namespace token_protection {

/**
 * Callback: fetch the full previous transaction by txid (injected by the
 * caller, typically mempool -> txindex/disk). Returns nullptr when the
 * transaction cannot be found.
 */
using PrevTxFetcher = std::function<CTransactionRef(const TxId&)>;

/** Check result */
enum class TokenCheckResult {
    OK,                    //!< Passed (or the transaction is not token related)
    NEGATIVE_AMOUNT,       //!< A tape amount field is negative
    AMOUNT_MISMATCH,       //!< Input/output token amounts of a transfer do not match
    POOLNFT_TAPE_TAMPERED, //!< Non-amount fields of a poolnft tape were tampered with
    PREV_TX_UNAVAILABLE,   //!< Full previous transaction unavailable; check cannot proceed
};

/** Reject reason string for a result (used with CValidationState) */
const char* TokenCheckResultToRejectReason(TokenCheckResult result);

/**
 * Whether the output locking script is an FT/LP/StableCoin tape output:
 * pattern `OP_FALSE OP_RETURN ... 4654617065` ("FTape" flag).
 */
bool IsFTapeOutput(const CScript& lockingScript);

/**
 * Whether the output locking script is a PoolNFT tape output:
 * pattern `OP_FALSE OP_RETURN ... 4e54617065` ("NTape" flag).
 */
bool IsNTapeOutput(const CScript& lockingScript);

/**
 * Parse the amount field from a tape output: take the first 48 bytes of the
 * first push data after `OP_FALSE OP_RETURN`, split into 6 int64 values
 * (8 bytes each, little-endian signed); the total token amount is the sum of
 * the 6 values. Returns std::nullopt if any amount is negative or the format
 * is invalid.
 */
std::optional<std::vector<int64_t>> ParseTapeAmounts(const CScript& tapeScript);

/**
 * Compute the token identity (partial hash) of a contract locking script:
 * strip the fixed-length trailing suffix `OP_RETURN <21B> <5B>` (29 bytes in
 * total; content may vary but the length is fixed) and SHA256 the remaining
 * bytes. Scripts shorter than 29 bytes are hashed in full.
 */
uint256 ComputeTokenPartialHash(const CScript& lockingScript);

/**
 * Token amount conservation check for transfer transactions:
 * build Input/OutputTokenAmountMap keyed by partial_hash and require, for
 * each output token: the token does not exist on the input side (treated as
 * mint) || input total == output total.
 * Also verifies that all output tape amounts are non-negative.
 */
TokenCheckResult CheckTokenAmountConservation(const CTransaction& tx,
                                              const PrevTxFetcher& prevTxFetcher);

/**
 * PoolNFT tape tamper check:
 * if vout[1] is an NTape with poolnft layout, fetch the old tape at vout[1]
 * of the previous transaction referenced by vin[0], and require the new and
 * old tapes to be identical except for the amount field (4th chunk, 24B).
 * Pool creation (no poolnft tape in the previous transaction) is treated as
 * mint and passes.
 */
TokenCheckResult CheckPoolNftTape(const CTransaction& tx,
                                  const PrevTxFetcher& prevTxFetcher);

/**
 * Entry point: run non-negative amount + amount conservation + poolnft tape
 * checks in order. Returns the first failure, or OK if all checks pass.
 */
TokenCheckResult CheckTokenTransaction(const CTransaction& tx,
                                       const PrevTxFetcher& prevTxFetcher);

} // namespace token_protection
