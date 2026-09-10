// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open TBC software license, see the accompanying file
// LICENSE.

#include "rpc/transactionflow.h"

#include "chain.h"
#include "dbwrapper.h"
#include "logging.h"
#include "rpc/protocol.h"
#include "rpc/server.h"
#include "validation.h"

#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace transactionflow {
namespace {

    [[noreturn]] void Inconsistent(const char *detail) {
        throw JSONRPCError(RPC_INTERNAL_ERROR,
                           std::string("Inconsistent transaction flow data: ") +
                               detail);
    }

    Amount SumAmounts(const std::vector<Amount> &values) {
        if (values.empty()) {
            Inconsistent("empty input or output collection");
        }
        Amount total{0};
        for (const Amount &value : values) {
            // Amount addition does not check overflow, so validate operands
            // before adding them.
            if (!MoneyRange(value) || value > MAX_MONEY - total) {
                Inconsistent("amount or total outside money range");
            }
            total += value;
        }
        return total;
    }

    [[noreturn]] void PreviousUnavailable() {
        throw JSONRPCError(
            RPC_DATABASE_ERROR,
            "Previous transaction data unavailable or inconsistent");
    }

    void CheckBlockIndex(const LocatedTransaction &located) {
        if (!located.blockIndexAvailable) {
            throw JSONRPCError(RPC_DATABASE_ERROR,
                               "Local block index data unavailable");
        }
    }

} // namespace

QueryGuard::QueryGuard(std::atomic_flag &busy) : m_busy(busy) {
    if (m_busy.test_and_set(std::memory_order_acquire)) {
        throw JSONRPCError(RPC_MISC_ERROR,
                           "gettransactionflow is busy; retry later");
    }
}

QueryGuard::~QueryGuard() {
    m_busy.clear(std::memory_order_release);
}

Budget::Budget(Now now) : m_now(std::move(now)), m_started(m_now()) {}

void Budget::Check() const {
    // This cooperative budget cannot interrupt a lock wait or an in-progress
    // disk read.
    if (m_now() - m_started >= QUERY_TIMEOUT) {
        throw JSONRPCError(RPC_MISC_ERROR,
                           "gettransactionflow deadline exceeded; retry later");
    }
}

void CheckLimit(size_t value, size_t maximum, const char *description) {
    if (value > maximum) {
        throw JSONRPCError(
            RPC_INVALID_PARAMETER,
            std::string("Transaction flow query exceeds supported limits: ") +
                description);
    }
}

void Budget::AddParentBytes(size_t bytes) {
    CheckLimit(bytes, MAX_PARENT_BYTES, "parent transaction bytes");
    // m_parentBytes always remains within its limit. Check the remaining
    // capacity before addition to avoid unsigned wraparound.
    CheckLimit(bytes, MAX_TOTAL_PARENT_BYTES - m_parentBytes,
               "total parent transaction bytes");
    m_parentBytes += bytes;
}

TxId ParseTransactionId(const UniValue &value) {
    RPCTypeCheckArgument(value, UniValue::VSTR);
    if (value.get_str().size() != 64) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "txid must be of length 64");
    }
    // Bound the length before calling the existing parser so an error cannot
    // echo an arbitrarily long input.
    return TxId(ParseHashV(value, "txid"));
}

Amounts ComputeAmounts(const std::vector<Amount> &inputs,
                       const std::vector<Amount> &outputs) {
    const Amount totalInput = SumAmounts(inputs);
    const Amount totalOutput = SumAmounts(outputs);
    if (totalOutput > totalInput) {
        Inconsistent("total output exceeds total input");
    }
    return {totalInput, totalOutput, totalInput - totalOutput};
}

Amount ResolvePrevoutValue(const CTransaction &parent, uint32_t n) {
    if (n >= parent.vout.size()) {
        Inconsistent("previous output index out of range");
    }
    const Amount value = parent.vout[n].nValue;
    if (!MoneyRange(value)) {
        Inconsistent("previous output amount outside money range");
    }
    return value;
}

Snapshot CollectSnapshot(const TxId &txid, const uint256 &bestblockhash,
                         int bestblockheight, const Reader &read,
                         Budget &budget) {
    budget.Check();
    const LocatedTransaction target = read(txid);
    budget.Check();
    if (!target.tx) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                           "Transaction unavailable from local node");
    }
    if (target.tx->GetId() != txid) {
        Inconsistent("target transaction id mismatch");
    }
    if (target.tx->IsCoinBase()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           "Coinbase transactions are not supported");
    }
    if (target.blockhash.IsNull()) {
        throw JSONRPCError(
            RPC_INVALID_PARAMETER,
            "Located transaction data has no active-chain confirmation");
    }
    CheckBlockIndex(target);
    if (!target.active) {
        throw JSONRPCError(
            RPC_INVALID_PARAMETER,
            "Located transaction data has no active-chain confirmation");
    }
    if (bestblockhash.IsNull() || target.height < 0 ||
        target.height > bestblockheight) {
        Inconsistent("invalid chain snapshot heights");
    }

    const auto &tx = *target.tx;
    if (tx.vin.empty() || tx.vout.empty()) {
        Inconsistent("empty input or output collection");
    }
    CheckLimit(tx.vin.size(), MAX_INPUTS, "input count");
    CheckLimit(tx.vout.size(), MAX_OUTPUTS, "output count");
    const size_t targetBytes =
        GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
    CheckLimit(targetBytes, MAX_TARGET_BYTES, "target transaction bytes");
    budget.Check();

    Snapshot snapshot;
    snapshot.txid = tx.GetId();
    snapshot.blockhash = target.blockhash;
    snapshot.blockheight = target.height;
    snapshot.bestblockhash = bestblockhash;
    snapshot.bestblockheight = bestblockheight;
    snapshot.targetBytes = targetBytes;
    snapshot.inputs.resize(tx.vin.size());
    snapshot.outputs.reserve(tx.vout.size());
    for (const auto &output : tx.vout) {
        snapshot.outputs.push_back(output.nValue);
    }

    // Grouping only deduplicates parent reads. Input values and result order
    // still follow the original vin indices.
    std::map<TxId, std::vector<size_t>> groups;
    std::set<COutPoint> seen;
    for (size_t i = 0; i < tx.vin.size(); ++i) {
        const auto &prevout = tx.vin[i].prevout;
        if (prevout.IsNull() || !seen.insert(prevout).second) {
            Inconsistent("null or duplicate previous output");
        }
        snapshot.inputs[i].prevout = prevout;
        groups[prevout.GetTxId()].push_back(i);
    }
    for (const auto &group : groups) {
        budget.Check();
        // Retain one full parent per iteration and never reuse the previous
        // lookup's block hash.
        const LocatedTransaction parent = read(group.first);
        budget.Check();
        if (!parent.tx || parent.blockhash.IsNull()) {
            PreviousUnavailable();
        }
        CheckBlockIndex(parent);
        if (!parent.active || parent.height < 0 ||
            parent.height > target.height ||
            parent.tx->GetId() != group.first) {
            PreviousUnavailable();
        }
        budget.AddParentBytes(
            GetSerializeSize(*parent.tx, SER_NETWORK, PROTOCOL_VERSION));
        for (const size_t i : group.second) {
            snapshot.inputs[i].value =
                ResolvePrevoutValue(*parent.tx, tx.vin[i].prevout.GetN());
        }
        budget.Check();
    }
    snapshot.parentCount = groups.size();
    snapshot.parentBytes = budget.ParentBytes();
    return snapshot;
}

UniValue BuildJSON(const Snapshot &snapshot, const Budget &budget) {
    budget.Check();
    CheckLimit(snapshot.inputs.size(), MAX_INPUTS, "input count");
    CheckLimit(snapshot.outputs.size(), MAX_OUTPUTS, "output count");
    if (snapshot.blockheight < 0 ||
        snapshot.bestblockheight < snapshot.blockheight ||
        snapshot.blockhash.IsNull() || snapshot.bestblockhash.IsNull()) {
        Inconsistent("invalid chain snapshot");
    }
    std::vector<Amount> inputValues;
    inputValues.reserve(snapshot.inputs.size());
    for (const auto &input : snapshot.inputs) {
        inputValues.push_back(input.value);
    }
    const Amounts totals = ComputeAmounts(inputValues, snapshot.outputs);

    UniValue result{UniValue::VOBJ};
    result.pushKV("txid", snapshot.txid.GetHex());
    result.pushKV("blockhash", snapshot.blockhash.GetHex());
    result.pushKV("blockheight", snapshot.blockheight);
    result.pushKV("confirmations",
                  int64_t(snapshot.bestblockheight) - snapshot.blockheight + 1);
    result.pushKV("bestblockhash", snapshot.bestblockhash.GetHex());
    result.pushKV("bestblockheight", snapshot.bestblockheight);
    result.pushKV("input_count", uint64_t(snapshot.inputs.size()));
    result.pushKV("output_count", uint64_t(snapshot.outputs.size()));
    UniValue inputs{UniValue::VARR};
    for (size_t i = 0; i < snapshot.inputs.size(); ++i) {
        const auto &input = snapshot.inputs[i];
        UniValue item{UniValue::VOBJ};
        item.pushKV("n", uint64_t(i));
        item.pushKV("prev_txid", input.prevout.GetTxId().GetHex());
        item.pushKV("prev_vout", uint64_t(input.prevout.GetN()));
        item.pushKV("value", ValueFromAmount(input.value));
        inputs.push_back(item);
    }
    result.pushKV("inputs", inputs);
    UniValue outputs{UniValue::VARR};
    for (size_t i = 0; i < snapshot.outputs.size(); ++i) {
        UniValue item{UniValue::VOBJ};
        item.pushKV("n", uint64_t(i));
        item.pushKV("value", ValueFromAmount(snapshot.outputs[i]));
        outputs.push_back(item);
    }
    result.pushKV("outputs", outputs);
    result.pushKV("total_input", ValueFromAmount(totals.totalInput));
    result.pushKV("total_output", ValueFromAmount(totals.totalOutput));
    result.pushKV("fee", ValueFromAmount(totals.fee));
    budget.Check();
    CheckLimit(result.write().size(), MAX_JSON_BYTES, "result JSON bytes");
    budget.Check();
    return result;
}

} // namespace transactionflow

namespace {

// Call only while holding cs_main. Snapshots do not retain CBlockIndex
// pointers.
transactionflow::LocatedTransaction
ReadTransactionFlowData(const Config &config, const TxId &txid) {
    AssertLockHeld(cs_main);
    transactionflow::LocatedTransaction located;
    bool genesisEnabled{false};
    if (!GetTransaction(config, txid, located.tx, false, located.blockhash,
                        genesisEnabled)) {
        // The backend collapses several failures into false. Report a missing
        // block index only when deserialization completed, the ID matches, and
        // the missing index can be proven directly. Do not guess other causes.
        if (located.tx && located.tx->GetId() == txid &&
            !located.blockhash.IsNull()) {
            const auto entry = mapBlockIndex.find(located.blockhash);
            if (entry == mapBlockIndex.end() || !entry->second) {
                throw JSONRPCError(RPC_DATABASE_ERROR,
                                   "Local block index data unavailable");
            }
        }
        located.tx.reset();
        return located;
    }
    if (!located.blockhash.IsNull()) {
        const auto entry = mapBlockIndex.find(located.blockhash);
        if (entry != mapBlockIndex.end() && entry->second) {
            located.blockIndexAvailable = true;
            located.height = entry->second->nHeight;
            located.active = chainActive.Contains(entry->second);
        }
    }
    return located;
}

} // namespace

UniValue gettransactionflow(const Config &config,
                            const JSONRPCRequest &request) {
    if (request.fHelp || request.params.size() != 1) {
        throw std::runtime_error(
            "gettransactionflow \"txid\"\n"
            "\nReturn the value flow of a confirmed, non-Coinbase transaction "
            "in the active chain.\n"
            "This command requires -txindex=1. Amounts are JSON numbers "
            "denominated in TBC and formatted with six decimal places.\n"
            "Reads historical previous outputs, including spent outputs; "
            "does not require a wallet or change chain/mempool state.\n"
            "Limits: 100 inputs, 1000 outputs, 1 MiB target, 4 MiB per parent, "
            "16 MiB total parents, 256 KiB result; one concurrent query.\n"
            "The 2000 ms cooperative deadline and post-read size checks do not "
            "bound initial deserialization or interrupt blocking I/O/lock "
            "waits.\n"
            "\nArguments:\n"
            "1. \"txid\"    (string, required) The transaction id as exactly "
            "64 hexadecimal characters; no 0x prefix or whitespace\n"
            "\nResult:\n"
            "{\n"
            "  \"txid\": \"hex\",              (string) The transaction id\n"
            "  \"blockhash\": \"hex\",         (string) The containing block "
            "hash\n"
            "  \"blockheight\": n,             (numeric) The containing block "
            "height\n"
            "  \"confirmations\": n,           (numeric) The confirmation "
            "count\n"
            "  \"bestblockhash\": \"hex\",     (string) The snapshot tip hash\n"
            "  \"bestblockheight\": n,         (numeric) The snapshot tip "
            "height\n"
            "  \"input_count\": n,             (numeric) The number of inputs\n"
            "  \"output_count\": n,            (numeric) The number of "
            "outputs\n"
            "  \"inputs\": [                   (array) Inputs in transaction "
            "order\n"
            "    {\n"
            "      \"n\": n,                   (numeric) Input index\n"
            "      \"prev_txid\": \"hex\",      (string) Referenced "
            "transaction id\n"
            "      \"prev_vout\": n,           (numeric) Referenced output "
            "index\n"
            "      \"value\": x.xxxxxx         (numeric) Referenced value in "
            "TBC\n"
            "    }\n"
            "  ],\n"
            "  \"outputs\": [                  (array) Outputs in transaction "
            "order\n"
            "    {\n"
            "      \"n\": n,                   (numeric) Output index\n"
            "      \"value\": x.xxxxxx         (numeric) Output value in TBC\n"
            "    }\n"
            "  ],\n"
            "  \"total_input\": x.xxxxxx,      (numeric) Total input value in "
            "TBC\n"
            "  \"total_output\": x.xxxxxx,     (numeric) Total output value in "
            "TBC\n"
            "  \"fee\": x.xxxxxx              (numeric) Transaction fee in "
            "TBC\n"
            "}\n"
            "\nExamples:\n" +
            HelpExampleCli("gettransactionflow",
                           "\"bc214b100b993aee4d2c8d9068dbac06b11f6ab26022b38c9"
                           "40af6386f8384ae\"") +
            HelpExampleRpc("gettransactionflow",
                           "\"bc214b100b993aee4d2c8d9068dbac06b11f6ab26022b38c9"
                           "40af6386f8384ae\""));
    }

    const TxId txid = transactionflow::ParseTransactionId(request.params[0]);
    static std::atomic_flag busy = ATOMIC_FLAG_INIT;
    const transactionflow::QueryGuard guard{busy};
    transactionflow::Budget budget;
    const auto started = transactionflow::Budget::Clock::now();
    transactionflow::Snapshot snapshot;
    int64_t lockMicros{0};
    {
        LOCK(cs_main);
        const auto lockStarted = transactionflow::Budget::Clock::now();
        budget.Check();
        if (!fTxIndex) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "gettransactionflow requires -txindex=1");
        }
        const auto *tip = chainActive.Tip();
        if (!tip) {
            throw JSONRPCError(RPC_DATABASE_ERROR,
                               "Local block index data unavailable");
        }
        try {
            snapshot = transactionflow::CollectSnapshot(
                txid, tip->GetBlockHash(), tip->nHeight,
                [&config](const TxId &id) {
                    return ReadTransactionFlowData(config, id);
                },
                budget);
        } catch (const dbwrapper_error &) {
            throw JSONRPCError(RPC_DATABASE_ERROR,
                               "Local transaction database unavailable");
        }
        lockMicros = std::chrono::duration_cast<std::chrono::microseconds>(
                         transactionflow::Budget::Clock::now() - lockStarted)
                         .count();
    }
    // cs_main is released here. Amount calculation, JSON construction, and the
    // framework response all happen without the chain lock.
    UniValue result = transactionflow::BuildJSON(snapshot, budget);
    const auto totalMicros =
        std::chrono::duration_cast<std::chrono::microseconds>(
            transactionflow::Budget::Clock::now() - started)
            .count();
    LogPrint(BCLog::RPC,
             "gettransactionflow stats inputs=%u parents=%u target_bytes=%u "
             "parent_bytes=%u lock_us=%d total_us=%d\n",
             snapshot.inputs.size(), snapshot.parentCount, snapshot.targetBytes,
             snapshot.parentBytes, lockMicros, totalMicros);
    return result;
}
