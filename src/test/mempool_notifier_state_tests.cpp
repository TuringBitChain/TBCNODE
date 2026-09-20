// Distributed under the Open TBC software license, see the accompanying file LICENSE.

#include "zmq/mempool_notifier_state.h"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <future>
#include <limits>
#include <thread>
#include <vector>

namespace
{
struct StateSetup
{
    fs::path directory{fs::temp_directory_path() / fs::unique_path()};
    fs::path epochFile{directory / "txinmempool.epoch"};

    StateSetup() { fs::create_directory(directory); }
    ~StateSetup()
    {
        boost::system::error_code error;
        fs::remove_all(directory, error);
    }

    void WriteCounter(const std::string& value)
    {
        fs::ofstream output(epochFile);
        output << value;
    }
};

uint256 TxId(uint64_t value)
{
    uint256 txid;
    WriteLE64(txid.begin(), value);
    return txid;
}
}

BOOST_FIXTURE_TEST_SUITE(mempool_notifier_state_tests, StateSetup)

BOOST_AUTO_TEST_CASE(disabled_state_has_no_side_effects)
{
    MempoolNotifierState state;
    BOOST_CHECK(state.GetStatus() == MempoolNotifierState::Status::DISABLED);
    BOOST_CHECK_EQUAL(state.GetSnapshot().epoch, -1);
    BOOST_CHECK_EQUAL(state.GetSnapshot().seq, -1);
    BOOST_CHECK(!state.Accepted(TxId(1)));
    BOOST_CHECK(!state.Removed(TxId(1)));
    BOOST_CHECK(!state.GetAcceptance(TxId(1)));
    bool called{false};
    BOOST_CHECK(!state.Publish({1, 1}, [&] { called = true; return true; }));
    BOOST_CHECK(!called);
    BOOST_CHECK(!fs::exists(epochFile));
}

BOOST_AUTO_TEST_CASE(accept_remove_and_reaccept_have_distinct_positions)
{
    MempoolNotifierState state;
    state.Start(epochFile);
    const auto initial{state.GetSnapshot()};
    BOOST_CHECK_GT(initial.epoch, 0);
    BOOST_CHECK_EQUAL(initial.seq, 0);

    const auto accepted{state.Accepted(TxId(1))};
    BOOST_REQUIRE(accepted);
    BOOST_CHECK_EQUAL(accepted->seq, 1);
    BOOST_CHECK_EQUAL(accepted->epoch, initial.epoch);
    BOOST_CHECK_EQUAL(state.GetAcceptance(TxId(1))->seq, accepted->seq);
    BOOST_CHECK_EQUAL(state.GetSnapshot().seq, 0);
    BOOST_CHECK_THROW(state.Accepted(TxId(1)), std::logic_error);
    BOOST_CHECK(state.Publish(*accepted, [] { return true; }));
    BOOST_CHECK_EQUAL(state.GetSnapshot().seq, 1);

    const auto removed{state.Removed(TxId(1))};
    BOOST_REQUIRE(removed);
    BOOST_CHECK_EQUAL(removed->seq, 2);
    BOOST_CHECK(!state.GetAcceptance(TxId(1)));
    const auto reaccepted{state.Accepted(TxId(1))};
    BOOST_REQUIRE(reaccepted);
    BOOST_CHECK_EQUAL(reaccepted->seq, 3);
    BOOST_CHECK_EQUAL(reaccepted->epoch, accepted->epoch);
    BOOST_CHECK(state.Publish(*removed, [] { return true; }));
    // Publishing an old removal must not erase a newer acceptance.
    BOOST_CHECK_EQUAL(state.GetAcceptance(TxId(1))->seq, 3);
    BOOST_CHECK(state.Publish(*reaccepted, [] { return true; }));
    BOOST_CHECK_EQUAL(state.GetSnapshot().seq, 3);
}

BOOST_AUTO_TEST_CASE(restored_entry_can_leave_without_an_acceptance)
{
    MempoolNotifierState state;
    state.Start(epochFile);
    BOOST_CHECK(!state.GetAcceptance(TxId(1)));
    BOOST_CHECK_EQUAL(state.GetSnapshot().seq, 0);
    const auto removed{state.Removed(TxId(1))};
    BOOST_REQUIRE(removed);
    BOOST_CHECK_EQUAL(removed->seq, 1);
    BOOST_CHECK(state.Publish(*removed, [] { return true; }));
    const auto accepted{state.Accepted(TxId(1))};
    BOOST_REQUIRE(accepted);
    BOOST_CHECK_EQUAL(accepted->seq, 2);
}

BOOST_AUTO_TEST_CASE(restart_changes_epoch_and_resets_sequence)
{
    int64_t oldEpoch;
    {
        MempoolNotifierState state;
        state.Start(epochFile);
        const auto accepted{state.Accepted(TxId(1))};
        BOOST_REQUIRE(accepted);
        oldEpoch = accepted->epoch;
        BOOST_CHECK(state.Publish(*accepted, [] { return true; }));
        // Simulate termination without Stop: the counter was saved at Start.
    }
    // A leftover temporary file must not replace the committed counter.
    fs::path temporary{epochFile};
    temporary += ".tmp";
    { fs::ofstream output(temporary); output << "0\n"; }
    MempoolNotifierState restarted;
    restarted.Start(epochFile);
    BOOST_CHECK_GT(restarted.GetSnapshot().epoch, oldEpoch);
    BOOST_CHECK_EQUAL(restarted.GetSnapshot().seq, 0);
    BOOST_CHECK(!restarted.GetAcceptance(TxId(1)));
    BOOST_CHECK_EQUAL(restarted.Accepted(TxId(1))->seq, 1);
    BOOST_CHECK_THROW(restarted.Start(epochFile), std::logic_error);
}

BOOST_AUTO_TEST_CASE(invalid_epoch_storage_prevents_startup)
{
    for (const auto& value : {"", "-1\n", "abc\n", "7 trailing\n",
                              "9223372036854775808\n"})
    {
        WriteCounter(value);
        MempoolNotifierState state;
        BOOST_CHECK_THROW(state.Start(epochFile), std::runtime_error);
        BOOST_CHECK(state.GetStatus() == MempoolNotifierState::Status::DISABLED);
        BOOST_CHECK_EQUAL(state.GetSnapshot().epoch, -1);
    }
    WriteCounter(std::to_string(std::numeric_limits<int64_t>::max()));
    MempoolNotifierState exhausted;
    BOOST_CHECK_THROW(exhausted.Start(epochFile), std::overflow_error);
    MempoolNotifierState missingDirectory;
    BOOST_CHECK_THROW(missingDirectory.Start(directory / "missing" / "epoch"),
                      std::runtime_error);
    BOOST_CHECK(!missingDirectory.Accepted(TxId(1)));
}

BOOST_AUTO_TEST_CASE(failed_epoch_write_preserves_previous_counter)
{
    WriteCounter("7\n");
    fs::path temporary{epochFile};
    temporary += ".tmp";
    fs::create_directory(temporary);
    MempoolNotifierState state;
    BOOST_CHECK_THROW(state.Start(epochFile), std::runtime_error);
    BOOST_CHECK_EQUAL(state.GetSnapshot().epoch, -1);
    BOOST_CHECK(!state.Accepted(TxId(1)));
    fs::remove(temporary);
    state.Start(epochFile);
    BOOST_CHECK_EQUAL(state.GetSnapshot().epoch, 8);
    BOOST_CHECK_EQUAL(state.GetSnapshot().seq, 0);
}

BOOST_AUTO_TEST_CASE(publication_rejects_invalid_positions_before_sending)
{
    MempoolNotifierState state;
    state.Start(epochFile);
    const auto first{state.Accepted(TxId(1)).value()};
    const auto second{state.Accepted(TxId(2)).value()};
    int calls{0};
    const auto sender = [&] { ++calls; return true; };
    BOOST_CHECK_THROW(state.Publish(second, sender), std::logic_error);
    BOOST_CHECK_THROW(state.Publish({first.epoch + 1, 1}, sender), std::logic_error);
    BOOST_CHECK_THROW(state.Publish({first.epoch, 0}, sender), std::logic_error);
    BOOST_CHECK_THROW(state.Publish({first.epoch, -1}, sender), std::logic_error);
    BOOST_CHECK_THROW(state.Publish({first.epoch, 3}, sender), std::logic_error);
    BOOST_CHECK_EQUAL(calls, 0);
    BOOST_CHECK(state.Publish(first, sender));
    BOOST_CHECK_THROW(state.Publish(first, sender), std::logic_error);
    BOOST_CHECK(state.Publish(second, sender));
    BOOST_CHECK_EQUAL(calls, 2);
    BOOST_CHECK_EQUAL(state.GetSnapshot().seq, 2);
}

BOOST_AUTO_TEST_CASE(send_failure_is_terminal)
{
    for (bool throws : {false, true})
    {
        MempoolNotifierState state;
        state.Start(epochFile);
        const auto first{state.Accepted(TxId(1)).value()};
        const auto second{state.Accepted(TxId(2)).value()};
        BOOST_CHECK(state.Publish(first, [] { return true; }));
        if (throws)
        {
            BOOST_CHECK_THROW(state.Publish(second, []() -> bool {
                throw std::runtime_error("Transport failure");
            }), std::runtime_error);
        }
        else
        {
            BOOST_CHECK(!state.Publish(second, [] { return false; }));
        }
        BOOST_CHECK(state.GetStatus() == MempoolNotifierState::Status::FAILED);
        BOOST_CHECK_THROW(state.GetSnapshot(), std::runtime_error);
        BOOST_CHECK_THROW(state.GetAcceptance(TxId(1)), std::runtime_error);
        BOOST_CHECK_THROW(state.Accepted(TxId(3)), std::runtime_error);
        BOOST_CHECK_THROW(state.Removed(TxId(1)), std::runtime_error);
        bool called{false};
        BOOST_CHECK_THROW(state.Publish(second, [&] { called = true; return true; }),
                          std::runtime_error);
        BOOST_CHECK(!called);
        BOOST_CHECK_THROW(state.Start(epochFile), std::logic_error);
        state.Stop();
        BOOST_CHECK_EQUAL(state.GetSnapshot().seq, -1);
    }
}

BOOST_AUTO_TEST_CASE(producer_failure_during_send_cannot_be_hidden)
{
    MempoolNotifierState state;
    state.Start(epochFile);
    const auto first{state.Accepted(TxId(1)).value()};
    BOOST_CHECK(!state.Publish(first, [&] {
        state.Fail();
        return true;
    }));
    BOOST_CHECK(state.GetStatus() == MempoolNotifierState::Status::FAILED);
    BOOST_CHECK_THROW(state.GetSnapshot(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(concurrent_events_have_unique_contiguous_sequences)
{
    MempoolNotifierState state;
    state.Start(epochFile);
    constexpr size_t threads{8};
    constexpr size_t transactions{64};
    std::vector<MempoolNotifierState::Position> positions(threads * transactions * 2);
    std::vector<std::thread> workers;
    std::atomic<bool> entriesMatch{true};
    for (size_t worker = 0; worker < threads; ++worker)
    {
        workers.emplace_back([&, worker] {
            for (size_t i = 0; i < transactions; ++i)
            {
                const size_t index{worker * transactions + i};
                const auto txid{TxId(index)};
                const auto accepted{state.Accepted(txid).value()};
                positions[index * 2] = accepted;
                const auto entry{state.GetAcceptance(txid)};
                if (!entry || entry->seq != accepted.seq || entry->epoch != accepted.epoch)
                    entriesMatch = false;
                positions[index * 2 + 1] = state.Removed(txid).value();
                if (state.GetAcceptance(txid)) entriesMatch = false;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    BOOST_CHECK(entriesMatch);
    BOOST_CHECK_EQUAL(state.GetSnapshot().seq, 0);
    std::sort(positions.begin(), positions.end(), [](const auto& a, const auto& b) {
        return a.seq < b.seq;
    });
    for (size_t i = 0; i < positions.size(); ++i)
    {
        BOOST_CHECK_EQUAL(positions[i].seq, i + 1);
        BOOST_CHECK(state.Publish(positions[i], [] { return true; }));
    }
    BOOST_CHECK_EQUAL(state.GetSnapshot().seq, positions.size());
}

BOOST_AUTO_TEST_CASE(in_flight_send_does_not_block_state_queries_or_producers)
{
    MempoolNotifierState state;
    state.Start(epochFile);
    const auto first{state.Accepted(TxId(1)).value()};
    std::promise<void> sending;
    std::promise<void> finish;
    auto finishFuture{finish.get_future()};
    auto publisher{std::async(std::launch::async, [&] {
        return state.Publish(first, [&] {
            sending.set_value();
            finishFuture.wait();
            return true;
        });
    })};
    sending.get_future().wait();
    const auto snapshot{state.GetSnapshot()};
    const auto second{state.Accepted(TxId(2))};
    finish.set_value();
    BOOST_CHECK(publisher.get());
    BOOST_CHECK_EQUAL(snapshot.seq, 0);
    BOOST_REQUIRE(second);
    BOOST_CHECK_EQUAL(second->seq, 2);
    BOOST_CHECK_EQUAL(state.GetSnapshot().seq, 1);
    BOOST_CHECK(state.Publish(*second, [] { return true; }));
}

BOOST_AUTO_TEST_CASE(stop_waits_for_send_and_abandons_pending_events)
{
    MempoolNotifierState state;
    state.Start(epochFile);
    const auto first{state.Accepted(TxId(1)).value()};
    const auto second{state.Accepted(TxId(2)).value()};
    std::promise<void> sending;
    std::promise<void> finish;
    auto finishFuture{finish.get_future()};
    auto publisher{std::async(std::launch::async, [&] {
        return state.Publish(first, [&] {
            sending.set_value();
            finishFuture.wait();
            return true;
        });
    })};
    sending.get_future().wait();
    auto stopping{std::async(std::launch::async, [&] { state.Stop(); })};
    finish.set_value();
    BOOST_CHECK(publisher.get());
    stopping.get();
    BOOST_CHECK(state.GetStatus() == MempoolNotifierState::Status::STOPPED);
    BOOST_CHECK_EQUAL(state.GetSnapshot().epoch, -1);
    BOOST_CHECK_EQUAL(state.GetSnapshot().seq, -1);
    BOOST_CHECK(!state.GetAcceptance(TxId(1)));
    BOOST_CHECK(!state.Accepted(TxId(3)));
    bool called{false};
    BOOST_CHECK(!state.Publish(second, [&] { called = true; return true; }));
    BOOST_CHECK(!called);
    BOOST_CHECK_THROW(state.Start(epochFile), std::logic_error);
    state.Stop();
}

BOOST_AUTO_TEST_SUITE_END()
