// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open TBC software license, see the accompanying file
// LICENSE.

#ifndef BITCOIN_RPC_TRANSACTIONFLOW_H
#define BITCOIN_RPC_TRANSACTIONFLOW_H

#include "primitives/transaction.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <univalue.h>
#include <vector>

class Config;
class JSONRPCRequest;

// These types are internal to the RPC. Value objects also let tests exercise
// boundary cases without depending on the transaction database.
namespace transactionflow {

constexpr size_t MAX_INPUTS = 100;
constexpr size_t MAX_OUTPUTS = 1000;
constexpr size_t MAX_TARGET_BYTES = 1024 * 1024;
constexpr size_t MAX_PARENT_BYTES = 4 * 1024 * 1024;
constexpr size_t MAX_TOTAL_PARENT_BYTES = 16 * 1024 * 1024;
constexpr size_t MAX_JSON_BYTES = 256 * 1024;
constexpr std::chrono::milliseconds QUERY_TIMEOUT{2000};

// All business requests compete for one slot. A failed construction does not
// own the slot, and destruction only releases a successfully acquired slot.
class QueryGuard {
public:
    explicit QueryGuard(std::atomic_flag &busy);
    ~QueryGuard();
    QueryGuard(const QueryGuard &) = delete;
    QueryGuard &operator=(const QueryGuard &) = delete;

private:
    std::atomic_flag &m_busy;
};

class Budget {
public:
    using Clock = std::chrono::steady_clock;
    using Now = std::function<Clock::time_point()>;
    explicit Budget(Now now = Clock::now);
    void Check() const;
    void AddParentBytes(size_t bytes);
    size_t ParentBytes() const { return m_parentBytes; }

private:
    Now m_now;
    Clock::time_point m_started;
    size_t m_parentBytes{0};
};

void CheckLimit(size_t value, size_t maximum, const char *description);
TxId ParseTransactionId(const UniValue &value);

struct Amounts {
    Amount totalInput;
    Amount totalOutput;
    Amount fee;
};
Amounts ComputeAmounts(const std::vector<Amount> &inputs,
                       const std::vector<Amount> &outputs);
Amount ResolvePrevoutValue(const CTransaction &parent, uint32_t n);

struct Input {
    COutPoint prevout;
    Amount value;
};

struct Snapshot {
    TxId txid;
    uint256 blockhash;
    int blockheight{0};
    uint256 bestblockhash;
    int bestblockheight{0};
    std::vector<Input> inputs;
    std::vector<Amount> outputs;
    size_t targetBytes{0};
    size_t parentBytes{0};
    size_t parentCount{0};
};

// Result of one lookup. An empty transaction means unavailable; an empty block
// hash may mean that the transaction was found in the memory pool.
struct LocatedTransaction {
    CTransactionRef tx;
    uint256 blockhash;
    int height{-1};
    bool blockIndexAvailable{false};
    bool active{false};
};
using Reader = std::function<LocatedTransaction(const TxId &)>;

// A real-node caller must hold cs_main until all reads and snapshot copies are
// complete. Reader is an internal test seam, not an RPC argument or fault
// injection switch.
Snapshot CollectSnapshot(const TxId &txid, const uint256 &bestblockhash,
                         int bestblockheight, const Reader &read,
                         Budget &budget);
UniValue BuildJSON(const Snapshot &snapshot, const Budget &budget);

} // namespace transactionflow

UniValue gettransactionflow(const Config &config,
                            const JSONRPCRequest &request);

#endif // BITCOIN_RPC_TRANSACTIONFLOW_H
