// Copyright (c) 2026 The TBC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "token_protection.h"

#include "crypto/common.h"
#include "crypto/sha256.h"

#include <limits>
#include <map>

namespace token_protection {

namespace {

//! Token identity (contract partial_hash) -> amount sum
using TokenAmountMap = std::map<uint256, int64_t>;

//! Accumulate non-negative values; returns false on overflow.
bool SafeAddNonNegative(int64_t& acc, int64_t value) {
    if (acc > std::numeric_limits<int64_t>::max() - value) {
        return false;
    }
    acc += value;
    return true;
}

/**
 * If sourceTx.vout[tapeIndex] is an FTape output, parse its amounts and
 * accumulate them into the map keyed by token identity (partial hash of the
 * contract output right before the tape). A tape output immediately follows
 * its contract output, so a tape at index 0 has no contract to attribute to
 * and is excluded from conservation accounting (non-negative amounts are
 * still verified).
 */
TokenCheckResult AccumulateTapeAmount(const CTransaction& sourceTx,
                                      size_t tapeIndex, TokenAmountMap& map) {
    const CScript& tapeScript = sourceTx.vout[tapeIndex].scriptPubKey;
    if (!IsFTapeOutput(tapeScript)) {
        return TokenCheckResult::OK;
    }
    const auto amounts = ParseTapeAmounts(tapeScript);
    if (!amounts) {
        return TokenCheckResult::NEGATIVE_AMOUNT;
    }
    if (tapeIndex == 0) {
        return TokenCheckResult::OK;
    }
    const uint256 tokenId =
        ComputeTokenPartialHash(sourceTx.vout[tapeIndex - 1].scriptPubKey);
    int64_t& sum = map[tokenId];
    for (const int64_t amount : *amounts) {
        if (!SafeAddNonNegative(sum, amount)) {
            return TokenCheckResult::AMOUNT_MISMATCH;
        }
    }
    return TokenCheckResult::OK;
}

//! Total size of the tape amount field: 6 slots of 8-byte signed
//! little-endian integers.
constexpr size_t TAPE_AMOUNT_FIELD_SIZE = 48;
constexpr size_t TAPE_AMOUNT_SLOT_SIZE = 8;
constexpr size_t TAPE_AMOUNT_SLOT_COUNT =
    TAPE_AMOUNT_FIELD_SIZE / TAPE_AMOUNT_SLOT_SIZE;

/**
 * Extract the first push data after `OP_FALSE OP_RETURN` (the tape amount
 * field). Returns false if the script does not match the pattern or the
 * first element is not a data push.
 */
bool GetTapeDataAfterOpReturn(const CScript& script,
                              std::vector<uint8_t>& data) {
    CScript::const_iterator pc = script.begin();
    opcodetype opcode{OP_INVALIDOPCODE};
    std::vector<uint8_t> vch;
    if (!script.GetOp(pc, opcode, vch) || opcode != OP_FALSE) {
        return false;
    }
    if (!script.GetOp(pc, opcode, vch) || opcode != OP_RETURN) {
        return false;
    }
    if (!script.GetOp(pc, opcode, data) || opcode > OP_PUSHDATA4) {
        return false;
    }
    return true;
}

//! FT/LP/StableCoin tape flag: "FTape" (hex 4654617065)
const std::vector<uint8_t> FTAPE_FLAG = {0x46, 0x54, 0x61, 0x70, 0x65};
//! poolnft tape flag: "NTape" (hex 4e54617065)
const std::vector<uint8_t> NTAPE_FLAG = {0x4e, 0x54, 0x61, 0x70, 0x65};

/**
 * poolnft tape layout (matches getPoolNftTape in the tbc-contract SDK):
 *   OP_FALSE OP_RETURN <64B ft_lp/ft_a partialhash>
 *   <24B amount: ft_lp_amount/ft_a_amount/tbc_amount, 8 bytes LE each>
 *   <32B ft_a_contractTxid> <serviceFeeRate> <lpPlan> <withLock> <withLockTime>
 *   "NTape"
 * The amount is the 4th chunk (the pool contract script slices the same
 * 3x8B amounts at offset 0x44).
 */
constexpr size_t POOLNFT_AMOUNT_CHUNK_INDEX = 3;
constexpr size_t POOLNFT_AMOUNT_SIZE = 24;

struct ScriptChunk {
    opcodetype opcode{OP_INVALIDOPCODE};
    std::vector<uint8_t> data;
};

//! Parse the whole script into an (opcode, data) sequence; returns false for
//! malformed scripts such as truncated pushes.
bool ParseScriptChunks(const CScript& script, std::vector<ScriptChunk>& chunks) {
    chunks.clear();
    CScript::const_iterator pc = script.begin();
    while (pc < script.end()) {
        ScriptChunk chunk;
        if (!script.GetOp(pc, chunk.opcode, chunk.data)) {
            return false;
        }
        chunks.push_back(std::move(chunk));
    }
    return true;
}

//! Match tape outputs of the form `OP_FALSE OP_RETURN ... <flag>`.
bool MatchesTapeFlagPattern(const CScript& lockingScript,
                            const std::vector<uint8_t>& flag) {
    std::vector<ScriptChunk> chunks;
    if (!ParseScriptChunks(lockingScript, chunks) || chunks.size() < 3) {
        return false;
    }
    return chunks[0].opcode == OP_FALSE && chunks[1].opcode == OP_RETURN &&
           chunks.back().data == flag;
}

//! Match tape outputs of the form `OP_FALSE OP_RETURN ... "NTape"`.
bool MatchesNTapePattern(const CScript& lockingScript) {
    return MatchesTapeFlagPattern(lockingScript, NTAPE_FLAG);
}

//! Match FT/LP/StableCoin tape outputs of the form
//! `OP_FALSE OP_RETURN ... "FTape"`.
bool MatchesFTapePattern(const CScript& lockingScript) {
    return MatchesTapeFlagPattern(lockingScript, FTAPE_FLAG);
}

/**
 * Determine whether a script has the poolnft tape layout (as opposed to
 * another tape sharing the same flag). A poolnft tape has at least 6 chunks:
 * OP_FALSE OP_RETURN <64B hash> <24B amount> <32B txid> "NTape"; later
 * versions carry extra fields (fee rate etc.) after the txid.
 */
bool IsPoolNftTapeLayout(const CScript& lockingScript) {
    std::vector<ScriptChunk> chunks;
    if (!ParseScriptChunks(lockingScript, chunks) || chunks.size() < 6) {
        return false;
    }
    if (chunks[0].opcode != OP_FALSE || chunks[1].opcode != OP_RETURN ||
        chunks.back().data != NTAPE_FLAG) {
        return false;
    }
    // chunk[2]=ft_lp||ft_a partialhash(64B), chunk[3]=3x uint64 amount(24B)
    return chunks[2].data.size() == 64 &&
           chunks[3].data.size() == POOLNFT_AMOUNT_SIZE;
}

//! New and old poolnft tapes must match chunk by chunk, except for the
//! amount field (4th chunk).
bool PoolNftTapesMatchExceptAmount(const CScript& newTape,
                                   const CScript& oldTape) {
    std::vector<ScriptChunk> newChunks;
    std::vector<ScriptChunk> oldChunks;
    if (!ParseScriptChunks(newTape, newChunks) ||
        !ParseScriptChunks(oldTape, oldChunks)) {
        return false;
    }
    if (newChunks.size() != oldChunks.size() ||
        newChunks.size() <= POOLNFT_AMOUNT_CHUNK_INDEX) {
        return false;
    }
    for (size_t i = 0; i < newChunks.size(); ++i) {
        if (newChunks[i].opcode != oldChunks[i].opcode) {
            return false;
        }
        if (i == POOLNFT_AMOUNT_CHUNK_INDEX) {
            // The amount field may change, but both sides must be 24 bytes
            // (3 uint64 values).
            if (newChunks[i].data.size() != POOLNFT_AMOUNT_SIZE ||
                oldChunks[i].data.size() != POOLNFT_AMOUNT_SIZE) {
                return false;
            }
            continue;
        }
        if (newChunks[i].data != oldChunks[i].data) {
            return false;
        }
    }
    return true;
}

} // namespace

const char* TokenCheckResultToRejectReason(TokenCheckResult result) {
    switch (result) {
        case TokenCheckResult::OK:
            return "";
        case TokenCheckResult::NEGATIVE_AMOUNT:
        case TokenCheckResult::AMOUNT_MISMATCH:
            return "bad-txn-token-amount-mismatch";
        case TokenCheckResult::POOLNFT_TAPE_TAMPERED:
            return "bad-txn-poolnft-tape-tampered";
        case TokenCheckResult::PREV_TX_UNAVAILABLE:
            return "bad-txn-token-prev-tx-unavailable";
    }
    return "";
}

bool IsFTapeOutput(const CScript& lockingScript) {
    return MatchesFTapePattern(lockingScript);
}

bool IsNTapeOutput(const CScript& lockingScript) {
    return MatchesNTapePattern(lockingScript);
}

std::optional<std::vector<int64_t>> ParseTapeAmounts(const CScript& tapeScript) {
    std::vector<uint8_t> data;
    if (!GetTapeDataAfterOpReturn(tapeScript, data)) {
        return std::nullopt;
    }
    if (data.size() < TAPE_AMOUNT_FIELD_SIZE) {
        return std::nullopt;
    }

    std::vector<int64_t> amounts;
    amounts.reserve(TAPE_AMOUNT_SLOT_COUNT);
    for (size_t offset = 0; offset < TAPE_AMOUNT_FIELD_SIZE;
         offset += TAPE_AMOUNT_SLOT_SIZE) {
        const auto amount =
            static_cast<int64_t>(ReadLE64(data.data() + offset));
        if (amount < 0) {
            return std::nullopt;
        }
        amounts.push_back(amount);
    }
    return amounts;
}

/**
 * Fixed-length suffix at the end of a contract script:
 *   OP_RETURN(1) + push21(1+21) + push5(1+5) = 29 bytes.
 * The suffix content (e.g. code hash / "2Code") may vary, but its serialized
 * length is fixed.
 */
constexpr size_t CONTRACT_SCRIPT_SUFFIX_LEN = 29;

uint256 ComputeTokenPartialHash(const CScript& lockingScript) {
    const size_t digestLen = lockingScript.size() > CONTRACT_SCRIPT_SUFFIX_LEN
                                 ? lockingScript.size() - CONTRACT_SCRIPT_SUFFIX_LEN
                                 : lockingScript.size();
    uint256 hash;
    CSHA256()
        .Write(lockingScript.data(), digestLen)
        .Finalize(hash.begin());
    return hash;
}

TokenCheckResult CheckTokenAmountConservation(const CTransaction& tx,
                                              const PrevTxFetcher& prevTxFetcher) {
    // Output side: parse all FTape outputs, verify amounts are non-negative
    // and build the OutputTokenAmountMap.
    TokenAmountMap outputMap;
    for (size_t i = 0; i < tx.vout.size(); ++i) {
        const auto result = AccumulateTapeAmount(tx, i, outputMap);
        if (result != TokenCheckResult::OK) {
            return result;
        }
    }
    // No token involved in the outputs, so no conservation constraint; skip
    // input-side parsing.
    if (outputMap.empty()) {
        return TokenCheckResult::OK;
    }
    // Input side: an input spends the contract output at {pre_txid, n}, and
    // its tape lives at vout[n + 1] of the previous transaction. Tapes are
    // OP_FALSE OP_RETURN outputs that never enter the UTXO set, so the full
    // previous transaction is required (mempool -> txindex/disk, injected
    // via prevTxFetcher).
    TokenAmountMap inputMap;
    for (const CTxIn& txin : tx.vin) {
        const CTransactionRef prevTx =
            prevTxFetcher(txin.prevout.GetTxId());
        if (!prevTx) {
            // Reject when the previous transaction cannot be fetched, so the
            // check cannot be bypassed by confirming the parent first and
            // spending it afterwards.
            return TokenCheckResult::PREV_TX_UNAVAILABLE;
        }
        const size_t tapeIndex =
            static_cast<size_t>(txin.prevout.GetN()) + 1;
        if (tapeIndex >= prevTx->vout.size()) {
            continue;
        }
        const auto result = AccumulateTapeAmount(*prevTx, tapeIndex, inputMap);
        if (result != TokenCheckResult::OK) {
            return result;
        }
    }

    // Conservation rule: for each output token, either the token does not
    // exist on the input side (treated as mint, which covers minttoken /
    // initpool / adding LP) || input total == output total.
    for (const auto& [tokenId, outputAmount] : outputMap) {
        const auto it = inputMap.find(tokenId);
        if (it != inputMap.end() && it->second != outputAmount) {
            return TokenCheckResult::AMOUNT_MISMATCH;
        }
    }
    return TokenCheckResult::OK;
}

TokenCheckResult CheckPoolNftTape(const CTransaction& tx,
                                  const PrevTxFetcher& prevTxFetcher) {
    // Only run the check when vout[1] is an NTape with poolnft layout.
    // Other tapes may share the "NTape" flag but have a different chunk
    // layout; they must be excluded to avoid false rejections.
    if (tx.vout.size() < 2 ||
        !IsPoolNftTapeLayout(tx.vout[1].scriptPubKey)) {
        return TokenCheckResult::OK;
    }
    if (tx.vin.empty()) {
        return TokenCheckResult::POOLNFT_TAPE_TAMPERED;
    }

    // Per the spec: take the transaction referenced by vin[0]'s
    // {pre_txid, 0} and read the old tape at {pre_txid, 1} (poolnft
    // positions are fixed: vout[0] contract, vout[1] tape).
    const TxId& prevTxId = tx.vin[0].prevout.GetTxId();
    const CTransactionRef prevTx = prevTxFetcher ? prevTxFetcher(prevTxId)
                                                 : nullptr;
    if (!prevTx) {
        return TokenCheckResult::PREV_TX_UNAVAILABLE;
    }

    // createPoolNFT: the previous transaction is usually a funding
    // transaction that may lack vout[1], or whose vout[1] is change or not a
    // poolnft tape -- treat it as minting a new pool and skip the tamper
    // comparison.
    if (prevTx->vout.size() < 2 ||
        !IsPoolNftTapeLayout(prevTx->vout[1].scriptPubKey)) {
        return TokenCheckResult::OK;
    }

    const CScript& newTape = tx.vout[1].scriptPubKey;
    const CScript& oldTape = prevTx->vout[1].scriptPubKey;
    if (!PoolNftTapesMatchExceptAmount(newTape, oldTape)) {
        return TokenCheckResult::POOLNFT_TAPE_TAMPERED;
    }
    return TokenCheckResult::OK;
}

TokenCheckResult CheckTokenTransaction(const CTransaction& tx,
                                       const PrevTxFetcher& prevTxFetcher) {
    const auto conservation = CheckTokenAmountConservation(tx, prevTxFetcher);
    if (conservation != TokenCheckResult::OK) {
        return conservation;
    }
    return CheckPoolNftTape(tx, prevTxFetcher);
}

} // namespace token_protection
