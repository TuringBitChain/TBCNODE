// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open TBC software license, see the accompanying file
// LICENSE.

#include "rpc/transactionflow.h"

#include "clientversion.h"
#include "protocol.h"
#include "rpc/protocol.h"
#include "serialize.h"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

template <typename Callable>
void CheckRPCError(Callable &&call, int expectedCode,
                   const std::string &messagePrefix) {
    bool caught{false};
    try {
        call();
    } catch (const UniValue &error) {
        caught = true;
        BOOST_REQUIRE(error.isObject());
        BOOST_CHECK_EQUAL(find_value(error, "code").get_int(), expectedCode);
        const std::string message = find_value(error, "message").get_str();
        BOOST_CHECK_MESSAGE(
            message.compare(0, messagePrefix.size(), messagePrefix) == 0,
            "Expected error prefix '" << messagePrefix << "', got '" << message
                                      << "'");
    }
    BOOST_CHECK_MESSAGE(caught, "Expected an RPC error");
}

TxId SyntheticTxId(char finalHexDigit) {
    std::string hex(64, '0');
    hex.back() = finalHexDigit;
    return TxId{uint256S(hex)};
}

uint256 SyntheticBlockHash(char finalHexDigit) {
    std::string hex(64, '0');
    hex.back() = finalHexDigit;
    return uint256S(hex);
}

CTransactionRef MakeTransaction(const std::vector<COutPoint> &inputs,
                                const std::vector<Amount> &outputs,
                                uint32_t lockTime = 0) {
    CMutableTransaction transaction;
    transaction.nVersion = CTransaction::CURRENT_VERSION;
    transaction.nLockTime = lockTime;
    for (const COutPoint &input : inputs) {
        transaction.vin.emplace_back(input);
    }
    for (const Amount &output : outputs) {
        transaction.vout.emplace_back(output, CScript{});
    }
    return MakeTransactionRef(std::move(transaction));
}

CTransactionRef MakeTransactionOfSize(const std::vector<COutPoint> &inputs,
                                      const std::vector<Amount> &outputs,
                                      size_t desiredSize, uint32_t lockTime) {
    CMutableTransaction transaction;
    transaction.nVersion = CTransaction::CURRENT_VERSION;
    transaction.nLockTime = lockTime;
    for (const COutPoint &input : inputs) {
        transaction.vin.emplace_back(input);
    }
    for (const Amount &output : outputs) {
        transaction.vout.emplace_back(output, CScript{});
    }
    if (transaction.vout.empty()) {
        throw std::logic_error{"A sized transaction needs an output"};
    }

    // The script's CompactSize prefix changes width at its thresholds. Adjust
    // against the serialized size instead of hard-coding transaction overhead.
    for (size_t attempts = 0; attempts < 8; ++attempts) {
        const size_t currentSize =
            GetSerializeSize(transaction, SER_NETWORK, PROTOCOL_VERSION);
        if (currentSize == desiredSize) {
            return MakeTransactionRef(std::move(transaction));
        }
        CScript &padding = transaction.vout.back().scriptPubKey;
        if (currentSize < desiredSize) {
            padding.resize(padding.size() + desiredSize - currentSize);
        } else {
            const size_t excess = currentSize - desiredSize;
            if (excess > padding.size()) {
                break;
            }
            padding.resize(padding.size() - excess);
        }
    }
    throw std::logic_error{"Could not construct requested serialized size"};
}

transactionflow::LocatedTransaction
Located(CTransactionRef transaction, const uint256 &blockhash, int height,
        bool blockIndexAvailable = true, bool active = true) {
    transactionflow::LocatedTransaction located;
    located.tx = std::move(transaction);
    located.blockhash = blockhash;
    located.height = height;
    located.blockIndexAvailable = blockIndexAvailable;
    located.active = active;
    return located;
}

class FakeReader {
public:
    std::map<TxId, transactionflow::LocatedTransaction> transactions;
    std::map<TxId, size_t> calls;

    transactionflow::LocatedTransaction Read(const TxId &txid) {
        ++calls[txid];
        const auto entry = transactions.find(txid);
        if (entry == transactions.end()) {
            return {};
        }
        return entry->second;
    }
};

transactionflow::Snapshot MakeAmountSnapshot() {
    transactionflow::Snapshot snapshot;
    snapshot.txid = SyntheticTxId('1');
    snapshot.blockhash = SyntheticBlockHash('2');
    snapshot.blockheight = 5;
    snapshot.bestblockhash = SyntheticBlockHash('3');
    snapshot.bestblockheight = 7;
    snapshot.inputs = {
        {COutPoint{SyntheticTxId('4'), 2}, Amount{10000001}},
        {COutPoint{SyntheticTxId('5'), 7}, Amount{20000002}},
    };
    snapshot.outputs = {Amount{10000001}, Amount{19999001}};
    snapshot.targetBytes = 225;
    snapshot.parentBytes = 450;
    snapshot.parentCount = 2;
    return snapshot;
}

} // namespace

BOOST_AUTO_TEST_SUITE(transactionflow_tests)

BOOST_AUTO_TEST_CASE(compute_amounts_is_exact_and_accepts_zero_fee) {
    const transactionflow::Amounts singleInput =
        transactionflow::ComputeAmounts({Amount{10000000}},
                                        {Amount{6000000}, Amount{3999000}});
    BOOST_CHECK_EQUAL(singleInput.totalInput, Amount{10000000});
    BOOST_CHECK_EQUAL(singleInput.totalOutput, Amount{9999000});
    BOOST_CHECK_EQUAL(singleInput.fee, Amount{1000});

    // This six-decimal vector also proves that one atomic unit is not rounded
    // through floating-point arithmetic.
    const transactionflow::Amounts values =
        transactionflow::ComputeAmounts({Amount{10000001}, Amount{20000002}},
                                        {Amount{10000001}, Amount{19999001}});
    BOOST_CHECK_EQUAL(values.totalInput, Amount{30000003});
    BOOST_CHECK_EQUAL(values.totalOutput, Amount{29999002});
    BOOST_CHECK_EQUAL(values.fee, Amount{1001});

    const transactionflow::Amounts zeroFee =
        transactionflow::ComputeAmounts({Amount{1}}, {Amount{1}});
    BOOST_CHECK_EQUAL(zeroFee.totalInput, Amount{1});
    BOOST_CHECK_EQUAL(zeroFee.totalOutput, Amount{1});
    BOOST_CHECK_EQUAL(zeroFee.fee, Amount{0});
}

BOOST_AUTO_TEST_CASE(compute_amounts_rejects_invalid_values_and_totals) {
    const int64_t overMaximum = MAX_MONEY.GetSatoshis() + 1;
    CheckRPCError([] { transactionflow::ComputeAmounts({}, {Amount{1}}); },
                  RPC_INTERNAL_ERROR, "Inconsistent transaction flow data");
    CheckRPCError([] { transactionflow::ComputeAmounts({Amount{1}}, {}); },
                  RPC_INTERNAL_ERROR, "Inconsistent transaction flow data");
    CheckRPCError(
        [] { transactionflow::ComputeAmounts({Amount{-1}}, {Amount{0}}); },
        RPC_INTERNAL_ERROR, "Inconsistent transaction flow data");
    CheckRPCError(
        [overMaximum] {
            transactionflow::ComputeAmounts({Amount{overMaximum}}, {Amount{0}});
        },
        RPC_INTERNAL_ERROR, "Inconsistent transaction flow data");
    CheckRPCError(
        [] {
            transactionflow::ComputeAmounts({MAX_MONEY, Amount{1}},
                                            {Amount{0}});
        },
        RPC_INTERNAL_ERROR, "Inconsistent transaction flow data");
    CheckRPCError(
        [] { transactionflow::ComputeAmounts({Amount{1}}, {Amount{2}}); },
        RPC_INTERNAL_ERROR, "Inconsistent transaction flow data");
}

BOOST_AUTO_TEST_CASE(resolve_prevout_checks_the_index_before_access) {
    const CTransactionRef parent =
        MakeTransaction({COutPoint{SyntheticTxId('1'), 0}},
                        {Amount{11}, Amount{22}, Amount{33}}, 1);

    BOOST_CHECK_EQUAL(transactionflow::ResolvePrevoutValue(*parent, 2),
                      Amount{33});
    CheckRPCError([&] { transactionflow::ResolvePrevoutValue(*parent, 3); },
                  RPC_INTERNAL_ERROR, "Inconsistent transaction flow data");
    CheckRPCError(
        [&] {
            transactionflow::ResolvePrevoutValue(
                *parent, std::numeric_limits<uint32_t>::max());
        },
        RPC_INTERNAL_ERROR, "Inconsistent transaction flow data");

    const CTransactionRef invalidParent =
        MakeTransaction({COutPoint{SyntheticTxId('2'), 0}}, {Amount{-1}}, 2);
    CheckRPCError(
        [&] { transactionflow::ResolvePrevoutValue(*invalidParent, 0); },
        RPC_INTERNAL_ERROR, "Inconsistent transaction flow data");
}

BOOST_AUTO_TEST_CASE(parse_transaction_id_is_strict_and_normalizes_case) {
    for (const UniValue &value :
         {UniValue{}, UniValue{true}, UniValue{1}, UniValue{UniValue::VARR},
          UniValue{UniValue::VOBJ}}) {
        CheckRPCError([&] { transactionflow::ParseTransactionId(value); },
                      RPC_TYPE_ERROR, "Expected type string");
    }

    for (const std::string &value :
         {std::string{}, std::string(63, '0'), std::string(65, '0')}) {
        CheckRPCError(
            [&] { transactionflow::ParseTransactionId(UniValue{value}); },
            RPC_INVALID_PARAMETER, "txid must be of length 64");
    }

    for (const std::string &value :
         {std::string(64, 'g'), std::string{"0x"} + std::string(62, '0'),
          std::string{" "} + std::string(63, '0'),
          std::string(63, '0') + std::string{" "}}) {
        CheckRPCError(
            [&] { transactionflow::ParseTransactionId(UniValue{value}); },
            RPC_INVALID_PARAMETER, "txid must be hexadecimal string");
    }

    const std::string uppercase(64, 'A');
    BOOST_CHECK_EQUAL(
        transactionflow::ParseTransactionId(UniValue{uppercase}).GetHex(),
        std::string(64, 'a'));
}

BOOST_AUTO_TEST_CASE(resource_limits_accept_boundaries_and_reject_excess) {
    BOOST_CHECK_NO_THROW(transactionflow::CheckLimit(
        transactionflow::MAX_INPUTS - 1, transactionflow::MAX_INPUTS,
        "input count"));
    BOOST_CHECK_NO_THROW(transactionflow::CheckLimit(
        transactionflow::MAX_INPUTS, transactionflow::MAX_INPUTS,
        "input count"));
    CheckRPCError(
        [] {
            transactionflow::CheckLimit(transactionflow::MAX_INPUTS + 1,
                                        transactionflow::MAX_INPUTS,
                                        "input count");
        },
        RPC_INVALID_PARAMETER,
        "Transaction flow query exceeds supported limits");

    BOOST_CHECK_NO_THROW(transactionflow::CheckLimit(
        transactionflow::MAX_OUTPUTS - 1, transactionflow::MAX_OUTPUTS,
        "output count"));
    BOOST_CHECK_NO_THROW(transactionflow::CheckLimit(
        transactionflow::MAX_OUTPUTS, transactionflow::MAX_OUTPUTS,
        "output count"));
    CheckRPCError(
        [] {
            transactionflow::CheckLimit(transactionflow::MAX_OUTPUTS + 1,
                                        transactionflow::MAX_OUTPUTS,
                                        "output count");
        },
        RPC_INVALID_PARAMETER,
        "Transaction flow query exceeds supported limits");

    BOOST_CHECK_NO_THROW(transactionflow::CheckLimit(
        transactionflow::MAX_TARGET_BYTES - 1,
        transactionflow::MAX_TARGET_BYTES, "target transaction bytes"));
    BOOST_CHECK_NO_THROW(transactionflow::CheckLimit(
        transactionflow::MAX_TARGET_BYTES, transactionflow::MAX_TARGET_BYTES,
        "target transaction bytes"));
    CheckRPCError(
        [] {
            transactionflow::CheckLimit(transactionflow::MAX_TARGET_BYTES + 1,
                                        transactionflow::MAX_TARGET_BYTES,
                                        "target transaction bytes");
        },
        RPC_INVALID_PARAMETER,
        "Transaction flow query exceeds supported limits");

    BOOST_CHECK_NO_THROW(transactionflow::CheckLimit(
        transactionflow::MAX_JSON_BYTES - 1, transactionflow::MAX_JSON_BYTES,
        "result JSON bytes"));
    BOOST_CHECK_NO_THROW(transactionflow::CheckLimit(
        transactionflow::MAX_JSON_BYTES, transactionflow::MAX_JSON_BYTES,
        "result JSON bytes"));
    CheckRPCError(
        [] {
            transactionflow::CheckLimit(transactionflow::MAX_JSON_BYTES + 1,
                                        transactionflow::MAX_JSON_BYTES,
                                        "result JSON bytes");
        },
        RPC_INVALID_PARAMETER,
        "Transaction flow query exceeds supported limits");

    transactionflow::Budget belowSingle;
    BOOST_CHECK_NO_THROW(
        belowSingle.AddParentBytes(transactionflow::MAX_PARENT_BYTES - 1));
    BOOST_CHECK_EQUAL(belowSingle.ParentBytes(),
                      transactionflow::MAX_PARENT_BYTES - 1);

    transactionflow::Budget exactSingle;
    BOOST_CHECK_NO_THROW(
        exactSingle.AddParentBytes(transactionflow::MAX_PARENT_BYTES));
    BOOST_CHECK_EQUAL(exactSingle.ParentBytes(),
                      transactionflow::MAX_PARENT_BYTES);

    transactionflow::Budget tooLargeSingle;
    CheckRPCError(
        [&] {
            tooLargeSingle.AddParentBytes(transactionflow::MAX_PARENT_BYTES +
                                          1);
        },
        RPC_INVALID_PARAMETER,
        "Transaction flow query exceeds supported limits");
    BOOST_CHECK_EQUAL(tooLargeSingle.ParentBytes(), 0U);

    transactionflow::Budget cumulative;
    for (size_t i = 0; i + 1 < transactionflow::MAX_TOTAL_PARENT_BYTES /
                                   transactionflow::MAX_PARENT_BYTES;
         ++i) {
        BOOST_CHECK_NO_THROW(
            cumulative.AddParentBytes(transactionflow::MAX_PARENT_BYTES));
    }
    BOOST_CHECK_NO_THROW(
        cumulative.AddParentBytes(transactionflow::MAX_PARENT_BYTES - 1));
    BOOST_CHECK_EQUAL(cumulative.ParentBytes(),
                      transactionflow::MAX_TOTAL_PARENT_BYTES - 1);
    BOOST_CHECK_NO_THROW(cumulative.AddParentBytes(1));
    BOOST_CHECK_EQUAL(cumulative.ParentBytes(),
                      transactionflow::MAX_TOTAL_PARENT_BYTES);
    CheckRPCError([&] { cumulative.AddParentBytes(1); }, RPC_INVALID_PARAMETER,
                  "Transaction flow query exceeds supported limits");
    BOOST_CHECK_EQUAL(cumulative.ParentBytes(),
                      transactionflow::MAX_TOTAL_PARENT_BYTES);
}

BOOST_AUTO_TEST_CASE(query_guard_is_nonblocking_and_releases_ownership) {
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    int contenderCode{0};
    std::string contenderMessage;
    {
        transactionflow::QueryGuard owner{busy};
        std::thread contender{[&] {
            try {
                transactionflow::QueryGuard second{busy};
            } catch (const UniValue &error) {
                contenderCode = find_value(error, "code").get_int();
                contenderMessage = find_value(error, "message").get_str();
            }
        }};
        contender.join();
        BOOST_CHECK_EQUAL(contenderCode, RPC_MISC_ERROR);
        BOOST_CHECK_EQUAL(contenderMessage,
                          "gettransactionflow is busy; retry later");

        // Failed construction never acquired ownership and must not clear the
        // first query's busy flag.
        CheckRPCError([&] { transactionflow::QueryGuard stillBusy{busy}; },
                      RPC_MISC_ERROR,
                      "gettransactionflow is busy; retry later");
    }

    const auto acquireAndRelease = [&] {
        transactionflow::QueryGuard guard{busy};
    };
    BOOST_CHECK_NO_THROW(acquireAndRelease());

    try {
        transactionflow::QueryGuard owner{busy};
        throw std::runtime_error{"synthetic failure"};
    } catch (const std::runtime_error &) {
    }
    BOOST_CHECK_NO_THROW(acquireAndRelease());
}

BOOST_AUTO_TEST_CASE(budget_uses_an_exact_monotonic_deadline) {
    using Clock = transactionflow::Budget::Clock;
    Clock::time_point now{};
    transactionflow::Budget budget{[&] { return now; }};

    now += transactionflow::QUERY_TIMEOUT - std::chrono::milliseconds{1};
    BOOST_CHECK_NO_THROW(budget.Check());
    now += std::chrono::milliseconds{1};
    CheckRPCError([&] { budget.Check(); }, RPC_MISC_ERROR,
                  "gettransactionflow deadline exceeded; retry later");
}

BOOST_AUTO_TEST_CASE(
    collect_snapshot_groups_parents_and_preserves_input_order) {
    const CTransactionRef parent =
        MakeTransaction({COutPoint{SyntheticTxId('1'), 0}},
                        {Amount{10000001}, Amount{20000002}}, 11);
    const CTransactionRef target = MakeTransaction(
        {COutPoint{parent->GetId(), 1}, COutPoint{parent->GetId(), 0}},
        {Amount{29999002}}, 12);

    FakeReader fake;
    fake.transactions.emplace(target->GetId(),
                              Located(target, SyntheticBlockHash('a'), 5));
    fake.transactions.emplace(parent->GetId(),
                              Located(parent, SyntheticBlockHash('b'), 4));
    transactionflow::Budget budget;
    const transactionflow::Snapshot snapshot = transactionflow::CollectSnapshot(
        target->GetId(), SyntheticBlockHash('c'), 7,
        [&](const TxId &txid) { return fake.Read(txid); }, budget);

    BOOST_REQUIRE_EQUAL(snapshot.inputs.size(), 2U);
    BOOST_CHECK(snapshot.inputs[0].prevout == target->vin[0].prevout);
    BOOST_CHECK(snapshot.inputs[1].prevout == target->vin[1].prevout);
    BOOST_CHECK_EQUAL(snapshot.inputs[0].value, Amount{20000002});
    BOOST_CHECK_EQUAL(snapshot.inputs[1].value, Amount{10000001});
    BOOST_REQUIRE_EQUAL(snapshot.outputs.size(), 1U);
    BOOST_CHECK_EQUAL(snapshot.outputs[0], Amount{29999002});
    BOOST_CHECK_EQUAL(snapshot.parentCount, 1U);
    BOOST_CHECK_EQUAL(fake.calls[target->GetId()], 1U);
    BOOST_CHECK_EQUAL(fake.calls[parent->GetId()], 1U);
    BOOST_CHECK_EQUAL(snapshot.targetBytes,
                      GetSerializeSize(*target, SER_NETWORK, PROTOCOL_VERSION));
    BOOST_CHECK_EQUAL(snapshot.parentBytes,
                      GetSerializeSize(*parent, SER_NETWORK, PROTOCOL_VERSION));
}

BOOST_AUTO_TEST_CASE(
    collect_snapshot_rejects_duplicate_or_missing_parent_data) {
    const CTransactionRef parent =
        MakeTransaction({COutPoint{SyntheticTxId('1'), 0}}, {Amount{10}}, 21);
    const COutPoint repeated{parent->GetId(), 0};
    const CTransactionRef duplicateTarget =
        MakeTransaction({repeated, repeated}, {Amount{10}}, 22);

    FakeReader duplicateReader;
    duplicateReader.transactions.emplace(
        duplicateTarget->GetId(),
        Located(duplicateTarget, SyntheticBlockHash('a'), 5));
    duplicateReader.transactions.emplace(
        parent->GetId(), Located(parent, SyntheticBlockHash('b'), 4));
    transactionflow::Budget duplicateBudget;
    CheckRPCError(
        [&] {
            transactionflow::CollectSnapshot(
                duplicateTarget->GetId(), SyntheticBlockHash('c'), 7,
                [&](const TxId &txid) { return duplicateReader.Read(txid); },
                duplicateBudget);
        },
        RPC_INTERNAL_ERROR, "Inconsistent transaction flow data");

    // groups is ordered by std::map<TxId, ...>. An all-f TXID sorts last, so a
    // known parent is collected before the second group fails.
    const TxId missingParent{uint256S(std::string(64, 'f'))};
    BOOST_REQUIRE(parent->GetId() < missingParent);
    const CTransactionRef missingTarget = MakeTransaction(
        {COutPoint{parent->GetId(), 0}, COutPoint{missingParent, 0}},
        {Amount{10}}, 23);
    FakeReader missingReader;
    missingReader.transactions.emplace(
        missingTarget->GetId(),
        Located(missingTarget, SyntheticBlockHash('e'), 5));
    missingReader.transactions.emplace(
        parent->GetId(), Located(parent, SyntheticBlockHash('f'), 4));
    transactionflow::Budget missingBudget;
    CheckRPCError(
        [&] {
            transactionflow::CollectSnapshot(
                missingTarget->GetId(), SyntheticBlockHash('9'), 7,
                [&](const TxId &txid) { return missingReader.Read(txid); },
                missingBudget);
        },
        RPC_DATABASE_ERROR,
        "Previous transaction data unavailable or inconsistent");
    BOOST_CHECK_EQUAL(missingReader.calls[parent->GetId()], 1U);
    BOOST_CHECK_EQUAL(missingReader.calls[missingParent], 1U);
    BOOST_CHECK_EQUAL(missingBudget.ParentBytes(),
                      GetSerializeSize(*parent, SER_NETWORK, PROTOCOL_VERSION));
}

BOOST_AUTO_TEST_CASE(collect_snapshot_rejects_invalid_target_structure) {
    const uint256 bestHash = SyntheticBlockHash('a');

    FakeReader unavailableReader;
    transactionflow::Budget unavailableBudget;
    CheckRPCError(
        [&] {
            transactionflow::CollectSnapshot(
                SyntheticTxId('1'), bestHash, 7,
                [&](const TxId &txid) { return unavailableReader.Read(txid); },
                unavailableBudget);
        },
        RPC_INVALID_ADDRESS_OR_KEY, "Transaction unavailable from local node");

    CMutableTransaction coinbaseMutable;
    coinbaseMutable.nVersion = CTransaction::CURRENT_VERSION;
    coinbaseMutable.vin.emplace_back();
    coinbaseMutable.vout.emplace_back(Amount{1}, CScript{});
    const CTransactionRef coinbase = MakeTransactionRef(coinbaseMutable);
    FakeReader coinbaseReader;
    coinbaseReader.transactions.emplace(
        coinbase->GetId(), Located(coinbase, SyntheticBlockHash('b'), 5));
    transactionflow::Budget coinbaseBudget;
    CheckRPCError(
        [&] {
            transactionflow::CollectSnapshot(
                coinbase->GetId(), bestHash, 7,
                [&](const TxId &txid) { return coinbaseReader.Read(txid); },
                coinbaseBudget);
        },
        RPC_INVALID_PARAMETER, "Coinbase transactions are not supported");

    const CTransactionRef emptyInputs = MakeTransaction({}, {Amount{1}}, 31);
    FakeReader emptyInputReader;
    emptyInputReader.transactions.emplace(
        emptyInputs->GetId(), Located(emptyInputs, SyntheticBlockHash('c'), 5));
    transactionflow::Budget emptyInputBudget;
    CheckRPCError(
        [&] {
            transactionflow::CollectSnapshot(
                emptyInputs->GetId(), bestHash, 7,
                [&](const TxId &txid) { return emptyInputReader.Read(txid); },
                emptyInputBudget);
        },
        RPC_INTERNAL_ERROR, "Inconsistent transaction flow data");

    const CTransactionRef emptyOutputs =
        MakeTransaction({COutPoint{SyntheticTxId('2'), 0}}, {}, 32);
    FakeReader emptyOutputReader;
    emptyOutputReader.transactions.emplace(
        emptyOutputs->GetId(),
        Located(emptyOutputs, SyntheticBlockHash('d'), 5));
    transactionflow::Budget emptyOutputBudget;
    CheckRPCError(
        [&] {
            transactionflow::CollectSnapshot(
                emptyOutputs->GetId(), bestHash, 7,
                [&](const TxId &txid) { return emptyOutputReader.Read(txid); },
                emptyOutputBudget);
        },
        RPC_INTERNAL_ERROR, "Inconsistent transaction flow data");
}

BOOST_AUTO_TEST_CASE(collect_snapshot_enforces_location_and_count_limits) {
    const CTransactionRef parent =
        MakeTransaction({COutPoint{SyntheticTxId('1'), 0}}, {Amount{1}}, 41);
    const CTransactionRef target =
        MakeTransaction({COutPoint{parent->GetId(), 0}}, {Amount{1}}, 42);
    const uint256 targetBlock = SyntheticBlockHash('a');
    const uint256 parentBlock = SyntheticBlockHash('b');
    const uint256 bestBlock = SyntheticBlockHash('c');

    auto checkTarget = [&](transactionflow::LocatedTransaction located,
                           int code, const std::string &prefix) {
        FakeReader fake;
        fake.transactions.emplace(target->GetId(), std::move(located));
        transactionflow::Budget budget;
        CheckRPCError(
            [&] {
                transactionflow::CollectSnapshot(
                    target->GetId(), bestBlock, 7,
                    [&](const TxId &txid) { return fake.Read(txid); }, budget);
            },
            code, prefix);
    };
    checkTarget(Located(parent, targetBlock, 5), RPC_INTERNAL_ERROR,
                "Inconsistent transaction flow data");
    checkTarget(Located(target, uint256{}, 5), RPC_INVALID_PARAMETER,
                "Located transaction data has no active-chain confirmation");
    checkTarget(Located(target, targetBlock, 5, false), RPC_DATABASE_ERROR,
                "Local block index data unavailable");
    checkTarget(Located(target, targetBlock, 5, true, false),
                RPC_INVALID_PARAMETER,
                "Located transaction data has no active-chain confirmation");
    checkTarget(Located(target, targetBlock, 8), RPC_INTERNAL_ERROR,
                "Inconsistent transaction flow data");

    auto checkParent = [&](transactionflow::LocatedTransaction located,
                           int code, const std::string &prefix) {
        FakeReader fake;
        fake.transactions.emplace(target->GetId(),
                                  Located(target, targetBlock, 5));
        fake.transactions.emplace(parent->GetId(), std::move(located));
        transactionflow::Budget budget;
        CheckRPCError(
            [&] {
                transactionflow::CollectSnapshot(
                    target->GetId(), bestBlock, 7,
                    [&](const TxId &txid) { return fake.Read(txid); }, budget);
            },
            code, prefix);
    };
    checkParent(Located(parent, uint256{}, 4), RPC_DATABASE_ERROR,
                "Previous transaction data unavailable or inconsistent");
    checkParent(Located(parent, parentBlock, 4, false), RPC_DATABASE_ERROR,
                "Local block index data unavailable");
    checkParent(Located(parent, parentBlock, 4, true, false),
                RPC_DATABASE_ERROR,
                "Previous transaction data unavailable or inconsistent");
    checkParent(Located(parent, parentBlock, 6), RPC_DATABASE_ERROR,
                "Previous transaction data unavailable or inconsistent");
    checkParent(Located(target, parentBlock, 4), RPC_DATABASE_ERROR,
                "Previous transaction data unavailable or inconsistent");

    std::vector<COutPoint> tooManyInputs;
    tooManyInputs.reserve(transactionflow::MAX_INPUTS + 1);
    for (size_t i = 0; i <= transactionflow::MAX_INPUTS; ++i) {
        tooManyInputs.emplace_back(SyntheticTxId('9'), uint32_t(i));
    }
    const CTransactionRef excessiveInputs =
        MakeTransaction(tooManyInputs, {Amount{1}}, 43);
    FakeReader inputReader;
    inputReader.transactions.emplace(excessiveInputs->GetId(),
                                     Located(excessiveInputs, targetBlock, 5));
    transactionflow::Budget inputBudget;
    CheckRPCError(
        [&] {
            transactionflow::CollectSnapshot(
                excessiveInputs->GetId(), bestBlock, 7,
                [&](const TxId &txid) { return inputReader.Read(txid); },
                inputBudget);
        },
        RPC_INVALID_PARAMETER,
        "Transaction flow query exceeds supported limits");

    std::vector<Amount> tooManyOutputs(transactionflow::MAX_OUTPUTS + 1,
                                       Amount{0});
    const CTransactionRef excessiveOutputs =
        MakeTransaction({COutPoint{parent->GetId(), 0}}, tooManyOutputs, 44);
    FakeReader outputReader;
    outputReader.transactions.emplace(
        excessiveOutputs->GetId(), Located(excessiveOutputs, targetBlock, 5));
    transactionflow::Budget outputBudget;
    CheckRPCError(
        [&] {
            transactionflow::CollectSnapshot(
                excessiveOutputs->GetId(), bestBlock, 7,
                [&](const TxId &txid) { return outputReader.Read(txid); },
                outputBudget);
        },
        RPC_INVALID_PARAMETER,
        "Transaction flow query exceeds supported limits");
}

BOOST_AUTO_TEST_CASE(collect_snapshot_enforces_actual_target_byte_boundary) {
    const CTransactionRef parent =
        MakeTransaction({COutPoint{SyntheticTxId('1'), 0}}, {Amount{1}}, 61);

    const auto runBoundary = [&](size_t desiredSize, uint32_t lockTime,
                                 bool accepted) {
        const CTransactionRef target =
            MakeTransactionOfSize({COutPoint{parent->GetId(), 0}}, {Amount{1}},
                                  desiredSize, lockTime);
        BOOST_REQUIRE_EQUAL(
            GetSerializeSize(*target, SER_NETWORK, PROTOCOL_VERSION),
            desiredSize);

        FakeReader fake;
        fake.transactions.emplace(target->GetId(),
                                  Located(target, SyntheticBlockHash('a'), 5));
        fake.transactions.emplace(parent->GetId(),
                                  Located(parent, SyntheticBlockHash('b'), 4));
        transactionflow::Budget budget;
        if (accepted) {
            const transactionflow::Snapshot snapshot =
                transactionflow::CollectSnapshot(
                    target->GetId(), SyntheticBlockHash('c'), 7,
                    [&](const TxId &txid) { return fake.Read(txid); }, budget);
            BOOST_CHECK_EQUAL(snapshot.targetBytes, desiredSize);
            BOOST_CHECK_EQUAL(fake.calls[parent->GetId()], 1U);
        } else {
            CheckRPCError(
                [&] {
                    transactionflow::CollectSnapshot(
                        target->GetId(), SyntheticBlockHash('c'), 7,
                        [&](const TxId &txid) { return fake.Read(txid); },
                        budget);
                },
                RPC_INVALID_PARAMETER,
                "Transaction flow query exceeds supported limits");
            BOOST_CHECK_EQUAL(fake.calls[parent->GetId()], 0U);
        }
    };

    runBoundary(transactionflow::MAX_TARGET_BYTES - 1, 62, true);
    runBoundary(transactionflow::MAX_TARGET_BYTES, 63, true);
    runBoundary(transactionflow::MAX_TARGET_BYTES + 1, 64, false);
}

BOOST_AUTO_TEST_CASE(collect_snapshot_enforces_actual_parent_byte_boundaries) {
    const auto runBoundary = [&](size_t desiredSize, uint32_t lockTime,
                                 bool accepted) {
        const CTransactionRef parent =
            MakeTransactionOfSize({COutPoint{SyntheticTxId('1'), 0}},
                                  {Amount{1}}, desiredSize, lockTime);
        BOOST_REQUIRE_EQUAL(
            GetSerializeSize(*parent, SER_NETWORK, PROTOCOL_VERSION),
            desiredSize);
        const CTransactionRef target = MakeTransaction(
            {COutPoint{parent->GetId(), 0}}, {Amount{1}}, lockTime + 10);

        FakeReader fake;
        fake.transactions.emplace(target->GetId(),
                                  Located(target, SyntheticBlockHash('a'), 5));
        fake.transactions.emplace(parent->GetId(),
                                  Located(parent, SyntheticBlockHash('b'), 4));
        transactionflow::Budget budget;
        if (accepted) {
            const transactionflow::Snapshot snapshot =
                transactionflow::CollectSnapshot(
                    target->GetId(), SyntheticBlockHash('c'), 7,
                    [&](const TxId &txid) { return fake.Read(txid); }, budget);
            BOOST_CHECK_EQUAL(snapshot.parentBytes, desiredSize);
        } else {
            CheckRPCError(
                [&] {
                    transactionflow::CollectSnapshot(
                        target->GetId(), SyntheticBlockHash('c'), 7,
                        [&](const TxId &txid) { return fake.Read(txid); },
                        budget);
                },
                RPC_INVALID_PARAMETER,
                "Transaction flow query exceeds supported limits");
            BOOST_CHECK_EQUAL(budget.ParentBytes(), 0U);
        }
        BOOST_CHECK_EQUAL(fake.calls[parent->GetId()], 1U);
    };

    runBoundary(transactionflow::MAX_PARENT_BYTES - 1, 71, true);
    runBoundary(transactionflow::MAX_PARENT_BYTES, 72, true);
    runBoundary(transactionflow::MAX_PARENT_BYTES + 1, 73, false);
}

BOOST_AUTO_TEST_CASE(collect_snapshot_stops_after_cumulative_parent_limit) {
    std::vector<CTransactionRef> parents;
    std::vector<TxId> parentIds;
    std::vector<COutPoint> inputs;
    FakeReader fake;
    for (uint32_t i = 0; i < 6; ++i) {
        CTransactionRef parent = MakeTransactionOfSize(
            {COutPoint{SyntheticTxId('1'), i}}, {Amount{1}},
            transactionflow::MAX_PARENT_BYTES, 80 + i);
        BOOST_REQUIRE_EQUAL(
            GetSerializeSize(*parent, SER_NETWORK, PROTOCOL_VERSION),
            transactionflow::MAX_PARENT_BYTES);
        parentIds.push_back(parent->GetId());
        inputs.emplace_back(parent->GetId(), 0);
        fake.transactions.emplace(parent->GetId(),
                                  Located(parent, SyntheticBlockHash('b'), 4));
        parents.push_back(std::move(parent));
    }
    std::sort(parentIds.begin(), parentIds.end());
    BOOST_REQUIRE(std::unique(parentIds.begin(), parentIds.end()) ==
                  parentIds.end());

    const CTransactionRef target = MakeTransaction(inputs, {Amount{6}}, 90);
    fake.transactions.emplace(target->GetId(),
                              Located(target, SyntheticBlockHash('a'), 5));
    transactionflow::Budget budget;
    CheckRPCError(
        [&] {
            transactionflow::CollectSnapshot(
                target->GetId(), SyntheticBlockHash('c'), 7,
                [&](const TxId &txid) { return fake.Read(txid); }, budget);
        },
        RPC_INVALID_PARAMETER,
        "Transaction flow query exceeds supported limits");

    BOOST_CHECK_EQUAL(budget.ParentBytes(),
                      transactionflow::MAX_TOTAL_PARENT_BYTES);
    for (size_t i = 0; i < 5; ++i) {
        BOOST_CHECK_EQUAL(fake.calls[parentIds[i]], 1U);
    }
    // The fifth full parent exceeds the post-read cumulative budget. The sixth
    // parent, which sorts later, must never be read.
    BOOST_CHECK_EQUAL(fake.calls[parentIds[5]], 0U);
}

BOOST_AUTO_TEST_CASE(collect_snapshot_checks_budget_around_reader_calls) {
    using Clock = transactionflow::Budget::Clock;
    Clock::time_point now{};
    size_t reads{0};
    transactionflow::Budget expiredBeforeRead{[&] { return now; }};
    now += transactionflow::QUERY_TIMEOUT;
    CheckRPCError(
        [&] {
            transactionflow::CollectSnapshot(
                SyntheticTxId('1'), SyntheticBlockHash('a'), 7,
                [&](const TxId &) {
                    ++reads;
                    return transactionflow::LocatedTransaction{};
                },
                expiredBeforeRead);
        },
        RPC_MISC_ERROR, "gettransactionflow deadline exceeded; retry later");
    BOOST_CHECK_EQUAL(reads, 0U);

    now = Clock::time_point{};
    const CTransactionRef target =
        MakeTransaction({COutPoint{SyntheticTxId('2'), 0}}, {Amount{1}}, 51);
    transactionflow::Budget expiresDuringRead{[&] { return now; }};
    CheckRPCError(
        [&] {
            transactionflow::CollectSnapshot(
                target->GetId(), SyntheticBlockHash('b'), 7,
                [&](const TxId &) {
                    ++reads;
                    now += transactionflow::QUERY_TIMEOUT;
                    return Located(target, SyntheticBlockHash('c'), 5);
                },
                expiresDuringRead);
        },
        RPC_MISC_ERROR, "gettransactionflow deadline exceeded; retry later");
    BOOST_CHECK_EQUAL(reads, 1U);

    now = Clock::time_point{};
    reads = 0;
    const CTransactionRef parent =
        MakeTransaction({COutPoint{SyntheticTxId('3'), 0}}, {Amount{1}}, 52);
    const CTransactionRef targetWithParent =
        MakeTransaction({COutPoint{parent->GetId(), 0}}, {Amount{1}}, 53);
    transactionflow::Budget expiresDuringParentRead{[&] { return now; }};
    CheckRPCError(
        [&] {
            transactionflow::CollectSnapshot(
                targetWithParent->GetId(), SyntheticBlockHash('d'), 7,
                [&](const TxId &txid) {
                    ++reads;
                    if (txid == targetWithParent->GetId()) {
                        return Located(targetWithParent,
                                       SyntheticBlockHash('e'), 5);
                    }
                    now += transactionflow::QUERY_TIMEOUT;
                    return Located(parent, SyntheticBlockHash('f'), 4);
                },
                expiresDuringParentRead);
        },
        RPC_MISC_ERROR, "gettransactionflow deadline exceeded; retry later");
    BOOST_CHECK_EQUAL(reads, 2U);
}

BOOST_AUTO_TEST_CASE(build_json_has_exact_ordered_numeric_contract) {
    const transactionflow::Snapshot snapshot = MakeAmountSnapshot();
    const transactionflow::Budget budget;
    const UniValue result = transactionflow::BuildJSON(snapshot, budget);

    BOOST_REQUIRE(result.isObject());
    BOOST_CHECK_EQUAL(result.size(), 13U);
    BOOST_CHECK_EQUAL(find_value(result, "txid").get_str(),
                      snapshot.txid.GetHex());
    BOOST_CHECK_EQUAL(find_value(result, "blockhash").get_str(),
                      snapshot.blockhash.GetHex());
    BOOST_CHECK_EQUAL(find_value(result, "blockheight").get_int(), 5);
    BOOST_CHECK_EQUAL(find_value(result, "confirmations").get_int64(), 3);
    BOOST_CHECK_EQUAL(find_value(result, "bestblockhash").get_str(),
                      snapshot.bestblockhash.GetHex());
    BOOST_CHECK_EQUAL(find_value(result, "bestblockheight").get_int(), 7);
    BOOST_CHECK_EQUAL(find_value(result, "input_count").get_int64(), 2);
    BOOST_CHECK_EQUAL(find_value(result, "output_count").get_int64(), 2);

    const UniValue &inputs = find_value(result, "inputs");
    BOOST_REQUIRE(inputs.isArray());
    BOOST_REQUIRE_EQUAL(inputs.size(), 2U);
    BOOST_CHECK_EQUAL(find_value(inputs[0], "n").get_int64(), 0);
    BOOST_CHECK_EQUAL(find_value(inputs[0], "prev_txid").get_str(),
                      snapshot.inputs[0].prevout.GetTxId().GetHex());
    BOOST_CHECK_EQUAL(find_value(inputs[0], "prev_vout").get_int64(), 2);
    BOOST_CHECK(find_value(inputs[0], "value").isNum());
    BOOST_CHECK_EQUAL(find_value(inputs[0], "value").getValStr(), "10.000001");
    BOOST_CHECK_EQUAL(find_value(inputs[1], "n").get_int64(), 1);
    BOOST_CHECK_EQUAL(find_value(inputs[1], "prev_vout").get_int64(), 7);
    BOOST_CHECK_EQUAL(find_value(inputs[1], "value").getValStr(), "20.000002");

    const UniValue &outputs = find_value(result, "outputs");
    BOOST_REQUIRE(outputs.isArray());
    BOOST_REQUIRE_EQUAL(outputs.size(), 2U);
    BOOST_CHECK_EQUAL(find_value(outputs[0], "n").get_int64(), 0);
    BOOST_CHECK(find_value(outputs[0], "value").isNum());
    BOOST_CHECK_EQUAL(find_value(outputs[0], "value").getValStr(), "10.000001");
    BOOST_CHECK_EQUAL(find_value(outputs[1], "n").get_int64(), 1);
    BOOST_CHECK_EQUAL(find_value(outputs[1], "value").getValStr(), "19.999001");

    BOOST_CHECK(find_value(result, "total_input").isNum());
    BOOST_CHECK(find_value(result, "total_output").isNum());
    BOOST_CHECK(find_value(result, "fee").isNum());
    BOOST_CHECK_EQUAL(find_value(result, "total_input").getValStr(),
                      "30.000003");
    BOOST_CHECK_EQUAL(find_value(result, "total_output").getValStr(),
                      "29.999002");
    BOOST_CHECK_EQUAL(find_value(result, "fee").getValStr(), "0.001001");
}

BOOST_AUTO_TEST_CASE(build_json_rejects_invalid_snapshot_metadata) {
    transactionflow::Snapshot snapshot = MakeAmountSnapshot();
    const transactionflow::Budget budget;

    snapshot.blockhash.SetNull();
    CheckRPCError([&] { transactionflow::BuildJSON(snapshot, budget); },
                  RPC_INTERNAL_ERROR, "Inconsistent transaction flow data");
    snapshot = MakeAmountSnapshot();
    snapshot.bestblockheight = snapshot.blockheight - 1;
    CheckRPCError([&] { transactionflow::BuildJSON(snapshot, budget); },
                  RPC_INTERNAL_ERROR, "Inconsistent transaction flow data");
}

BOOST_AUTO_TEST_CASE(build_json_accepts_maximum_shape_within_response_limit) {
    transactionflow::Snapshot snapshot = MakeAmountSnapshot();
    snapshot.inputs.clear();
    snapshot.outputs.clear();
    snapshot.inputs.reserve(transactionflow::MAX_INPUTS);
    snapshot.outputs.reserve(transactionflow::MAX_OUTPUTS);
    for (size_t i = 0; i < transactionflow::MAX_INPUTS; ++i) {
        snapshot.inputs.push_back(
            {COutPoint{SyntheticTxId('4'), uint32_t(i)}, Amount{10000}});
    }
    for (size_t i = 0; i < transactionflow::MAX_OUTPUTS; ++i) {
        snapshot.outputs.push_back(Amount{1000});
    }

    const transactionflow::Budget budget;
    const UniValue result = transactionflow::BuildJSON(snapshot, budget);
    BOOST_REQUIRE_EQUAL(find_value(result, "inputs").size(),
                        transactionflow::MAX_INPUTS);
    BOOST_REQUIRE_EQUAL(find_value(result, "outputs").size(),
                        transactionflow::MAX_OUTPUTS);
    BOOST_CHECK(find_value(result, "total_input").isNum());
    BOOST_CHECK(find_value(result, "total_output").isNum());
    BOOST_CHECK(find_value(result, "fee").isNum());
    BOOST_CHECK_EQUAL(find_value(result, "fee").getValStr(), "0.000000");
    BOOST_CHECK_LT(result.write().size(), transactionflow::MAX_JSON_BYTES);
}

BOOST_AUTO_TEST_SUITE_END()
