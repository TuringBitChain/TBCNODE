// Distributed under the Open TBC software license, see the accompanying file LICENSE.

#include "config.h"
#include "mining/journal_change_set.h"
#include "rpc/blockchain.h"
#include "rpc/server.h"
#include "test/test_bitcoin.h"
#include "txmempool.h"
#include "validation.h"
#include "zmq/mempool_notifier_state.h"

#include <boost/test/unit_test.hpp>
#include <chrono>
#include <future>

namespace {
JSONRPCRequest EntryRequest(const uint256& txid)
{
    JSONRPCRequest request;
    request.params = UniValue{UniValue::VARR};
    request.params.push_back(txid.GetHex());
    return request;
}

bool StreamError(const UniValue& error)
{
    return error["code"].get_int() == RPC_MISC_ERROR &&
           error["message"].get_str().find("notification stream has failed") != std::string::npos;
}

bool MissingEntry(const UniValue& error)
{
    return error["code"].get_int() == RPC_INVALID_ADDRESS_OR_KEY &&
           error["message"].get_str() == "Transaction not in mempool";
}

void CheckPosition(const UniValue& result, int64_t epoch, int64_t seq)
{
    BOOST_REQUIRE(result["epoch"].isNum());
    BOOST_REQUIRE(result["seq"].isNum());
    BOOST_CHECK_EQUAL(result["epoch"].get_int64(), epoch);
    BOOST_CHECK_EQUAL(result["seq"].get_int64(), seq);
}

void CheckOmitted(const UniValue& result)
{
    BOOST_CHECK(!result.exists("epoch"));
    BOOST_CHECK(!result.exists("seq"));
}

struct MempoolRPCSetup : TestingSetup
{
    fs::path directory{fs::temp_directory_path() / fs::unique_path()};
    MempoolNotifierState state;

    MempoolRPCSetup() { fs::create_directory(directory); }
    ~MempoolRPCSetup()
    {
        mempool.SetChangeObserver({});
        mempool.Clear();
        state.Stop();
        boost::system::error_code error;
        fs::remove_all(directory, error);
    }

    void Start()
    {
        // Exercise JSON integers larger than 32 bits without changing state internals.
        fs::ofstream{directory / "epoch"} << (int64_t{1} << 40) << '\n';
        state.Start(directory / "epoch");
        mempool.SetChangeObserver([this](const uint256& txid, std::optional<MemPoolRemovalReason> reason) {
            if (reason) state.Removed(txid);
            else state.Accepted(txid);
        });
    }

    uint256 Add(uint32_t output, bool notify = true)
    {
        CMutableTransaction tx;
        tx.vin.emplace_back(COutPoint(uint256S("1234"), output));
        tx.vout.emplace_back(COIN, CScript() << OP_TRUE);
        TestMemPoolEntryHelper entry;
        mempool.AddUnchecked(tx.GetId(), entry.FromTx(tx), nullptr, nullptr, nullptr, notify);
        return tx.GetId();
    }

    UniValue Snapshot() { return GetZMQTipSnapshot(JSONRPCRequest{}, &state); }
    UniValue Entry(const uint256& txid) { return GetMempoolEntry(EntryRequest(txid), &state); }
};
}

BOOST_FIXTURE_TEST_SUITE(zmq_mempool_rpc_tests, MempoolRPCSetup)

BOOST_AUTO_TEST_CASE(disabled_rpc_is_registered_and_preserves_entry_schema)
{
    JSONRPCRequest request;
    const auto* command{tableRPC["getzmqtipsnapshot"]};
    BOOST_REQUIRE(command);
    const auto result{command->call(GlobalConfig::GetConfig(), request)};
    BOOST_CHECK_EQUAL(result.size(), 2);
    CheckPosition(result, -1, -1);
    CheckPosition(Snapshot(), -1, -1);
    const auto txid{Add(0)};
    CheckOmitted(Entry(txid));
    BOOST_CHECK_EQUAL(Entry(txid).write(), GetMempoolEntry(EntryRequest(txid), nullptr).write());
    BOOST_CHECK_EXCEPTION(Entry(uint256S("ff")), UniValue, MissingEntry);
    request.params = UniValue{UniValue::VARR};
    request.params.push_back(1);
    BOOST_CHECK_THROW(command->call(GlobalConfig::GetConfig(), request), std::runtime_error);
    request.params.clear();
    request.fHelp = true;
    BOOST_CHECK_THROW(command->call(GlobalConfig::GetConfig(), request), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(entry_position_is_admission_and_restored_entries_have_no_position)
{
    Start();
    const auto epoch{state.GetSnapshot().epoch};
    CheckPosition(Snapshot(), epoch, 0);
    const auto first{Add(0)};
    const auto restored{Add(1, false)};
    const auto second{Add(2)};
    CheckPosition(Entry(first), epoch, 1);
    CheckPosition(Entry(second), epoch, 2);
    CheckOmitted(Entry(restored));
    const auto original{GetMempoolEntry(EntryRequest(first), nullptr)};
    const auto enriched{Entry(first)};
    BOOST_CHECK_EQUAL(enriched.size(), original.size() + 2);
    for (const auto& key : original.getKeys()) {
        BOOST_CHECK_EQUAL(enriched[key].write(), original[key].write());
    }
    CheckPosition(Snapshot(), epoch, 0);
    BOOST_REQUIRE(state.Publish(*state.GetAcceptance(first), [] { return true; }));
    BOOST_REQUIRE(state.Publish(*state.GetAcceptance(second), [] { return true; }));
    CheckPosition(Snapshot(), epoch, 2);
    CheckPosition(Entry(first), epoch, 1);
    mempool.RemoveRecursive(*mempool.Get(first), nullptr, MemPoolRemovalReason::BLOCK);
    BOOST_CHECK_EXCEPTION(Entry(first), UniValue, MissingEntry);
    BOOST_CHECK(Add(0) == first);
    CheckPosition(Entry(first), epoch, 4);
    CheckOmitted(Entry(restored));
    CheckPosition(Snapshot(), epoch, 2);
}

BOOST_AUTO_TEST_CASE(in_flight_send_does_not_advance_or_block_snapshot)
{
    Start();
    const auto txid{Add(0)};
    const auto position{*state.GetAcceptance(txid)};
    std::promise<void> entered, release;
    auto released{release.get_future()};
    auto publisher{std::async(std::launch::async, [&] {
        return state.Publish(position, [&] {
            entered.set_value();
            released.wait();
            return true;
        });
    })};
    entered.get_future().wait();
    auto query{std::async(std::launch::async, [&] {
        return std::make_pair(Snapshot(), Entry(txid));
    })};
    const auto status{query.wait_for(std::chrono::seconds{2})};
    // Always release the sender, even if a regression blocked the RPC query.
    release.set_value();
    BOOST_CHECK(publisher.get());
    const auto result{query.get()};
    BOOST_CHECK(status == std::future_status::ready);
    CheckPosition(result.first, position.epoch, 0);
    CheckPosition(result.second, position.epoch, 1);
    CheckPosition(Snapshot(), position.epoch, 1);
}

BOOST_AUTO_TEST_CASE(failed_stream_returns_rpc_errors_instead_of_disabled_or_stale_data)
{
    Start();
    const auto txid{Add(0)};
    BOOST_CHECK(!state.Publish(*state.GetAcceptance(txid), [] { return false; }));
    BOOST_CHECK_EXCEPTION(Snapshot(), UniValue, StreamError);
    BOOST_CHECK_EXCEPTION(Entry(txid), UniValue, StreamError);
    // Missing-entry errors retain their previous meaning even after failure.
    BOOST_CHECK_EXCEPTION(Entry(uint256S("ff")), UniValue, MissingEntry);
    state.Stop();
    CheckPosition(Snapshot(), -1, -1);
    CheckOmitted(Entry(txid));
}

BOOST_AUTO_TEST_SUITE_END()
