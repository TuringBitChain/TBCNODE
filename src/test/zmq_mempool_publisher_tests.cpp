// Distributed under the Open TBC software license, see the accompanying file LICENSE.

#include "zmq/zmqnotificationinterface.h"
#include "zmq/zmqpublishnotifier.h"
#include "chain.h"
#include "mining/journal_change_set.h"
#include "rpc/server.h"
#include "streams.h"
#include "test/test_bitcoin.h"
#include "util.h"
#include "utilstrencodings.h"

#include <boost/test/unit_test.hpp>
#include <univalue.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace
{
template <typename T>
struct TestPublisher : T
{
    std::string BoundAddress() const
    {
        char endpoint[256];
        size_t size{sizeof(endpoint)};
        BOOST_REQUIRE_EQUAL(zmq_getsockopt(this->psocket, ZMQ_LAST_ENDPOINT, endpoint, &size), 0);
        return endpoint;
    }
};

struct PublisherSetup : BasicTestingSetup
{
    fs::path directory{fs::temp_directory_path() / fs::unique_path()};
    void* context{zmq_ctx_new()};
    void* subscriber{nullptr};
    std::vector<std::unique_ptr<CZMQAbstractPublishNotifier>> publishers;

    PublisherSetup()
    {
        BOOST_REQUIRE(context);
        fs::create_directory(directory);
    }

    ~PublisherSetup()
    {
        if (subscriber) zmq_close(subscriber);
        for (auto& publisher : publishers) publisher->Shutdown();
        publishers.clear();
        zmq_ctx_term(context);
        boost::system::error_code error;
        fs::remove_all(directory, error);
    }

    template <typename T>
    TestPublisher<T>& MakePublisher(const std::string& address)
    {
        auto publisher{std::make_unique<TestPublisher<T>>()};
        auto& result{*publisher};
        publisher->SetAddress(address);
        publishers.push_back(std::move(publisher));
        BOOST_REQUIRE(result.Initialize(context));
        return result;
    }

    std::vector<std::string> Receive()
    {
        std::vector<std::string> frames;
        while (true)
        {
            zmq_msg_t message;
            BOOST_REQUIRE_EQUAL(zmq_msg_init(&message), 0);
            const int received{zmq_msg_recv(&message, subscriber, 0)};
            if (received < 0)
            {
                zmq_msg_close(&message);
                BOOST_FAIL("Timed out receiving ZMQ multipart message");
            }
            frames.emplace_back(static_cast<const char*>(zmq_msg_data(&message)),
                                zmq_msg_size(&message));
            const bool more{zmq_msg_more(&message) != 0};
            zmq_msg_close(&message);
            if (!more) return frames;
        }
    }

    template <typename T>
    void Connect(TestPublisher<T>& publisher)
    {
        subscriber = zmq_socket(context, ZMQ_SUB);
        BOOST_REQUIRE(subscriber);
        const int timeout{2000};
        const int linger{0};
        BOOST_REQUIRE_EQUAL(zmq_setsockopt(subscriber, ZMQ_RCVTIMEO, &timeout, sizeof(timeout)), 0);
        BOOST_REQUIRE_EQUAL(zmq_setsockopt(subscriber, ZMQ_LINGER, &linger, sizeof(linger)), 0);
        BOOST_REQUIRE_EQUAL(zmq_setsockopt(subscriber, ZMQ_SUBSCRIBE, "", 0), 0);
        BOOST_REQUIRE_EQUAL(zmq_connect(subscriber, publisher.BoundAddress().c_str()), 0);

        // PUB has no subscription acknowledgement. Probe until the subscriber
        // is ready, without consuming any txinmempool event positions.
        const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{5}};
        while (std::chrono::steady_clock::now() < deadline)
        {
            BOOST_REQUIRE(publisher.SendZMQMessage("probe", "", 0));
            zmq_pollitem_t item{subscriber, 0, ZMQ_POLLIN, 0};
            BOOST_REQUIRE_GE(zmq_poll(&item, 1, 100), 0);
            if (item.revents & ZMQ_POLLIN)
            {
                BOOST_REQUIRE_EQUAL(Receive()[0], "probe");
                return;
            }
        }
        BOOST_FAIL("ZMQ subscription did not become ready");
    }

    std::vector<std::string> Event()
    {
        auto frames{Receive()};
        while (frames[0] == "probe") frames = Receive();
        BOOST_REQUIRE_EQUAL(frames.size(), 3);
        return frames;
    }

    UniValue CheckEvent(const uint256& txid, int64_t epoch, int64_t sequence,
                       const std::string& reason = "")
    {
        const auto frames{Event()};
        BOOST_CHECK_EQUAL(frames[0], "txinmempool");
        BOOST_REQUIRE_EQUAL(frames[1].size(), 32);
        BOOST_CHECK_EQUAL(HexStr(frames[1].begin(), frames[1].end()), txid.GetHex());
        UniValue json;
        BOOST_REQUIRE(json.read(frames[2]));
        BOOST_REQUIRE(json.isObject());
        BOOST_CHECK_EQUAL(json.size(), reason.empty() ? 3 : 4);
        BOOST_CHECK_EQUAL(json["epoch"].get_int64(), epoch);
        BOOST_CHECK_EQUAL(json["seq"].get_int64(), sequence);
        BOOST_CHECK_EQUAL(json["status"].get_str(), reason.empty() ? "ACCEPTED" : "DISCARDED");
        if (!reason.empty()) BOOST_CHECK_EQUAL(json["reason"].get_str(), reason);
        return json;
    }

    void CheckFrames(const std::string& address)
    {
        auto& publisher{MakePublisher<CZMQPublishTxInMempoolNotifier>(address)};
        Connect(publisher);
        const auto txid{uint256S("0123456789abcdef00000000000000000000000000000000123456789abcdef001")};
        const int64_t epoch{int64_t{1} << 40};
        int64_t sequence{(int64_t{1} << 40) + 1};
        BOOST_REQUIRE(publisher.SendMempoolMessage(txid, {epoch, sequence}));
        CheckEvent(txid, epoch, sequence++);
        const std::vector<std::pair<MemPoolRemovalReason, std::string>> reasons{
            {MemPoolRemovalReason::EXPIRY, "expired"},
            {MemPoolRemovalReason::BLOCK, "included-in-block"},
            {MemPoolRemovalReason::CONFLICT, "collision-in-block-tx"},
            {MemPoolRemovalReason::REORG, "reorg"}};
        for (const auto& reason : reasons)
        {
            BOOST_REQUIRE(publisher.SendMempoolMessage(txid, {epoch, sequence}, reason.first));
            CheckEvent(txid, epoch, sequence++, reason.second);
        }
    }
};

struct MempoolArgGuard
{
    const std::string key{"-zmqpubtxinmempool"};
    const bool present{gArgs.IsArgSet(key)};
    const std::string previous{gArgs.GetArg(key, "")};
    ~MempoolArgGuard()
    {
        gArgs.ClearArg(key);
        if (present) gArgs.ForceSetArg(key, previous);
    }
};
}

BOOST_FIXTURE_TEST_SUITE(zmq_mempool_publisher_tests, PublisherSetup)

BOOST_AUTO_TEST_CASE(tcp_frames_and_reasons)
{
    CheckFrames("tcp://127.0.0.1:*");
}

#ifndef WIN32
BOOST_AUTO_TEST_CASE(ipc_frames_and_reasons)
{
    CheckFrames("ipc://" + (directory / "publisher.sock").string());
}
#endif

BOOST_AUTO_TEST_CASE(no_subscriber_still_advances_publication_without_replay)
{
    auto& publisher{MakePublisher<CZMQPublishTxInMempoolNotifier>("tcp://127.0.0.1:*")};
    MempoolNotifierState state;
    state.Start(directory / "epoch");
    const uint256 txid{uint256S("1234")};
    const auto accepted{state.Accepted(txid).value()};
    BOOST_CHECK(state.Publish(accepted, [&] { return publisher.SendMempoolMessage(txid, accepted); }));
    BOOST_CHECK_EQUAL(state.GetSnapshot().seq, 1);
    Connect(publisher);
    const auto removed{state.Removed(txid).value()};
    BOOST_CHECK(state.Publish(removed, [&] {
        return publisher.SendMempoolMessage(txid, removed, MemPoolRemovalReason::BLOCK);
    }));
    CheckEvent(txid, removed.epoch, 2, "included-in-block");
    BOOST_CHECK_EQUAL(state.GetSnapshot().seq, 2);
}

BOOST_AUTO_TEST_CASE(shared_legacy_topics_keep_their_frames_and_counters)
{
    const std::string address{"tcp://127.0.0.1:*"};
    auto& hashTx{MakePublisher<CZMQPublishHashTransactionNotifier>(address)};
    auto& rawTx{MakePublisher<CZMQPublishRawTransactionNotifier>(address)};
    auto& hashBlock{MakePublisher<CZMQPublishHashBlockNotifier>(address)};
    auto& publisher{MakePublisher<CZMQPublishTxInMempoolNotifier>(address)};
    Connect(publisher);
    const CTransaction transaction{CMutableTransaction{}};
    const uint256 blockHash{uint256S("abcd")};
    CBlockIndex block;
    block.phashBlock = &blockHash;
    BOOST_REQUIRE(hashTx.NotifyTransaction(transaction));
    auto frames{Event()};
    BOOST_CHECK_EQUAL(frames[0], "hashtx");
    BOOST_CHECK_EQUAL(HexStr(frames[1].begin(), frames[1].end()), transaction.GetId().GetHex());
    BOOST_REQUIRE_EQUAL(frames[2].size(), 4);
    BOOST_CHECK_EQUAL(ReadLE32(reinterpret_cast<const uint8_t*>(frames[2].data())), 0);
    BOOST_REQUIRE(publisher.SendMempoolMessage(transaction.GetId(), {7, 1}));
    CheckEvent(transaction.GetId(), 7, 1);
    BOOST_REQUIRE(rawTx.NotifyTransaction(transaction));
    frames = Event();
    BOOST_CHECK_EQUAL(frames[0], "rawtx");
    CDataStream serialized(SER_NETWORK, PROTOCOL_VERSION | RPCSerializationFlags());
    serialized << transaction;
    BOOST_CHECK_EQUAL(frames[1], std::string(serialized.begin(), serialized.end()));
    BOOST_REQUIRE_EQUAL(frames[2].size(), 4);
    BOOST_CHECK_EQUAL(ReadLE32(reinterpret_cast<const uint8_t*>(frames[2].data())), 0);
    BOOST_REQUIRE(hashBlock.NotifyBlock(&block));
    frames = Event();
    BOOST_CHECK_EQUAL(frames[0], "hashblock");
    BOOST_CHECK_EQUAL(HexStr(frames[1].begin(), frames[1].end()), blockHash.GetHex());
    BOOST_REQUIRE_EQUAL(frames[2].size(), 4);
    BOOST_CHECK_EQUAL(ReadLE32(reinterpret_cast<const uint8_t*>(frames[2].data())), 0);
    BOOST_REQUIRE(hashTx.NotifyTransaction(transaction));
    frames = Event();
    BOOST_REQUIRE_EQUAL(frames[2].size(), 4);
    BOOST_CHECK_EQUAL(ReadLE32(reinterpret_cast<const uint8_t*>(frames[2].data())), 1);
}

BOOST_AUTO_TEST_CASE(concurrent_shared_socket_messages_do_not_interleave)
{
    const std::string address{"tcp://127.0.0.1:*"};
    auto& legacy{MakePublisher<CZMQPublishHashTransactionNotifier>(address)};
    auto& publisher{MakePublisher<CZMQPublishTxInMempoolNotifier>(address)};
    Connect(publisher);
    const CTransaction transaction{CMutableTransaction{}};
    std::atomic<bool> sent{true};
    constexpr int count{100};
    std::thread oldMessages{[&] {
        for (int i = 0; i < count; ++i)
            if (!legacy.NotifyTransaction(transaction)) sent = false;
    }};
    std::thread newMessages{[&] {
        for (int i = 0; i < count; ++i)
            if (!publisher.SendMempoolMessage(transaction.GetId(), {7, i + 1})) sent = false;
    }};
    oldMessages.join();
    newMessages.join();
    BOOST_REQUIRE(sent);
    int oldCount{0}, newCount{0};
    for (int i = 0; i < count * 2; ++i)
    {
        const auto frames{Event()};
        BOOST_CHECK_EQUAL(HexStr(frames[1].begin(), frames[1].end()), transaction.GetId().GetHex());
        if (frames[0] == "hashtx")
        {
            BOOST_REQUIRE_EQUAL(frames[2].size(), 4);
            BOOST_CHECK_EQUAL(ReadLE32(reinterpret_cast<const uint8_t*>(frames[2].data())), oldCount++);
        }
        else
        {
            BOOST_REQUIRE_EQUAL(frames[0], "txinmempool");
            UniValue json;
            BOOST_REQUIRE(json.read(frames[2]));
            BOOST_CHECK_EQUAL(json["seq"].get_int64(), ++newCount);
        }
    }
    BOOST_CHECK_EQUAL(oldCount, count);
    BOOST_CHECK_EQUAL(newCount, count);
}

BOOST_AUTO_TEST_CASE(invalid_metadata_does_not_send_or_consume_legacy_sequence)
{
    auto& publisher{MakePublisher<CZMQPublishTxInMempoolNotifier>("tcp://127.0.0.1:*")};
    Connect(publisher);
    const uint256 txid{uint256S("01")};
    BOOST_CHECK_THROW(publisher.SendMempoolMessage(txid, {-1, 1}), std::invalid_argument);
    BOOST_CHECK_THROW(publisher.SendMempoolMessage(txid, {1, 0}), std::invalid_argument);
    BOOST_CHECK_THROW(publisher.SendMempoolMessage(txid, {1, 1}, MemPoolRemovalReason::UNKNOWN), std::invalid_argument);
    BOOST_CHECK_THROW(publisher.SendMempoolMessage(txid, {1, 1}, MemPoolRemovalReason::REPLACED), std::invalid_argument);
    BOOST_CHECK_THROW(publisher.SendMempoolMessage(txid, {1, 1}, MemPoolRemovalReason::SIZELIMIT), std::invalid_argument);
    BOOST_REQUIRE(publisher.SendMempoolMessage(txid, {1, 1}));
    CheckEvent(txid, 1, 1);
}

BOOST_AUTO_TEST_CASE(failed_bind_and_shared_shutdown_are_safe)
{
    CZMQPublishTxInMempoolNotifier failed;
    failed.SetAddress("invalid://address");
    BOOST_CHECK(!failed.Initialize(context));
    failed.Shutdown();
    failed.Shutdown();
    const std::string address{"tcp://127.0.0.1:*"};
    auto& first{MakePublisher<CZMQPublishHashTransactionNotifier>(address)};
    auto& second{MakePublisher<CZMQPublishTxInMempoolNotifier>(address)};
    first.Shutdown();
    Connect(second);
    const uint256 txid{uint256S("01")};
    BOOST_REQUIRE(second.SendMempoolMessage(txid, {1, 1}));
    CheckEvent(txid, 1, 1);
}

BOOST_AUTO_TEST_CASE(interface_publishes_allocated_positions_without_subscribers)
{
    MempoolArgGuard restore;
    gArgs.ForceSetArg(restore.key, "tcp://127.0.0.1:*");
    std::unique_ptr<CZMQNotificationInterface> interface{CZMQNotificationInterface::Create()};
    BOOST_REQUIRE(interface);
    auto& state{interface->GetMempoolState()};
    BOOST_CHECK_EQUAL(state.GetSnapshot().seq, -1);
    state.Start(directory / "epoch");
    const uint256 txid{uint256S("12")};
    const auto accepted{state.Accepted(txid).value()};
    BOOST_CHECK(interface->PublishMempoolTransaction(txid, accepted));
    BOOST_CHECK_EQUAL(state.GetSnapshot().seq, 1);
    const auto removed{state.Removed(txid).value()};
    BOOST_CHECK(interface->PublishMempoolTransaction(txid, removed, MemPoolRemovalReason::EXPIRY));
    BOOST_CHECK_EQUAL(state.GetSnapshot().seq, 2);
}

BOOST_AUTO_TEST_CASE(pool_events_are_ordered_and_restoration_is_silent)
{
    MempoolArgGuard restore;
    const std::string address{"tcp://127.0.0.1:*"};
    auto& probe{MakePublisher<CZMQPublishHashTransactionNotifier>(address)};
    Connect(probe);
    CTxMemPool pool;
    gArgs.ForceSetArg(restore.key, address);
    std::unique_ptr<CZMQNotificationInterface> interface{CZMQNotificationInterface::Create()};
    BOOST_REQUIRE(interface);
    interface->GetMempoolState().Start(directory / "epoch");
    interface->StartMempoolNotifications(pool);
    auto& state{interface->GetMempoolState()};
    const auto epoch{state.GetSnapshot().epoch};
    BOOST_CHECK_EQUAL(state.GetSnapshot().seq, 0);
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint(InsecureRand256(), 0);
    tx.vout.emplace_back(COIN, CScript() << OP_TRUE);
    TestMemPoolEntryHelper entry;
    const std::vector<std::pair<MemPoolRemovalReason, std::string>> reasons{
        {MemPoolRemovalReason::EXPIRY, "expired"},
        {MemPoolRemovalReason::BLOCK, "included-in-block"},
        {MemPoolRemovalReason::CONFLICT, "collision-in-block-tx"},
        {MemPoolRemovalReason::REORG, "reorg"}};
    int64_t sequence{0};
    for (const auto& reason : reasons) {
        pool.AddUnchecked(tx.GetId(), entry.FromTx(tx), nullptr);
        BOOST_REQUIRE(state.GetAcceptance(tx.GetId()));
        BOOST_CHECK_EQUAL(state.GetAcceptance(tx.GetId())->seq, ++sequence);
        CheckEvent(tx.GetId(), epoch, sequence);
        if (reason.first == MemPoolRemovalReason::EXPIRY) {
            BOOST_CHECK_EQUAL(pool.Expire(1, nullptr), 1);
        } else {
            pool.RemoveRecursive(CTransaction{tx}, nullptr, reason.first);
        }
        BOOST_CHECK(!state.GetAcceptance(tx.GetId()));
        CheckEvent(tx.GetId(), epoch, ++sequence, reason.second);
        // Removing a transaction that is already absent must not emit again.
        pool.RemoveRecursive(CTransaction{tx}, nullptr, reason.first);
    }
    pool.AddUnchecked(tx.GetId(), entry.FromTx(tx), nullptr, nullptr, nullptr, false);
    BOOST_CHECK(!state.GetAcceptance(tx.GetId()));
    pool.RemoveRecursive(CTransaction{tx}, nullptr, MemPoolRemovalReason::BLOCK);
    CheckEvent(tx.GetId(), epoch, ++sequence, "included-in-block");
    pool.AddUnchecked(tx.GetId(), entry.FromTx(tx), nullptr);
    CheckEvent(tx.GetId(), epoch, ++sequence);
    pool.Clear();
    BOOST_CHECK(!state.GetAcceptance(tx.GetId()));
    CheckEvent(tx.GetId(), epoch, ++sequence, "reorg");
    interface->StopMempoolNotifications();
    BOOST_CHECK_EQUAL(state.GetSnapshot().seq, -1);
    // The observer is detached before shutdown returns.
    pool.AddUnchecked(tx.GetId(), entry.FromTx(tx), nullptr);
    pool.Clear();
}

BOOST_AUTO_TEST_CASE(concurrent_pool_changes_keep_contiguous_event_order)
{
    MempoolArgGuard restore;
    const std::string address{"tcp://127.0.0.1:*"};
    auto& probe{MakePublisher<CZMQPublishHashTransactionNotifier>(address)};
    Connect(probe);
    CTxMemPool pool;
    gArgs.ForceSetArg(restore.key, address);
    std::unique_ptr<CZMQNotificationInterface> interface{CZMQNotificationInterface::Create()};
    BOOST_REQUIRE(interface);
    interface->GetMempoolState().Start(directory / "epoch");
    interface->StartMempoolNotifications(pool);
    const auto epoch{interface->GetMempoolState().GetSnapshot().epoch};
    constexpr int cycles{32};
    std::atomic<bool> succeeded{true};
    const auto mutate = [&](uint32_t output) {
        try {
            CMutableTransaction tx;
            tx.vin.resize(1);
            tx.vin[0].prevout = COutPoint(uint256S("1234"), output);
            tx.vout.emplace_back(COIN, CScript() << OP_TRUE);
            TestMemPoolEntryHelper entry;
            for (int i = 0; i < cycles; ++i) {
                pool.AddUnchecked(tx.GetId(), entry.FromTx(tx), nullptr);
                pool.RemoveRecursive(CTransaction{tx}, nullptr, MemPoolRemovalReason::BLOCK);
            }
        } catch (...) {
            succeeded = false;
        }
    };
    std::thread first{mutate, 0}, second{mutate, 1};
    first.join();
    second.join();
    BOOST_REQUIRE(succeeded);
    BOOST_CHECK_EQUAL(pool.Size(), 0);
    std::set<std::string> accepted;
    for (int64_t seq = 1; seq <= cycles * 4; ++seq) {
        const auto frames{Event()};
        BOOST_CHECK_EQUAL(frames[0], "txinmempool");
        UniValue metadata;
        BOOST_REQUIRE(metadata.read(frames[2]));
        BOOST_CHECK_EQUAL(metadata["epoch"].get_int64(), epoch);
        BOOST_CHECK_EQUAL(metadata["seq"].get_int64(), seq);
        if (metadata["status"].get_str() == "ACCEPTED") {
            BOOST_CHECK(accepted.insert(frames[1]).second);
        } else {
            BOOST_CHECK_EQUAL(metadata["status"].get_str(), "DISCARDED");
            BOOST_CHECK_EQUAL(metadata["reason"].get_str(), "included-in-block");
            BOOST_CHECK_EQUAL(accepted.erase(frames[1]), 1);
        }
    }
    BOOST_CHECK(accepted.empty());
}

BOOST_AUTO_TEST_CASE(pool_without_subscriber_publishes_and_failure_does_not_abort_mutations)
{
    MempoolArgGuard restore;
    CTxMemPool pool;
    gArgs.ForceSetArg(restore.key, "tcp://127.0.0.1:*");
    std::unique_ptr<CZMQNotificationInterface> interface{CZMQNotificationInterface::Create()};
    BOOST_REQUIRE(interface);
    interface->GetMempoolState().Start(directory / "epoch");
    interface->StartMempoolNotifications(pool);
    auto& state{interface->GetMempoolState()};
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint(InsecureRand256(), 0);
    tx.vout.emplace_back(COIN, CScript() << OP_TRUE);
    TestMemPoolEntryHelper entry;
    pool.AddUnchecked(tx.GetId(), entry.FromTx(tx), nullptr);
    const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{5}};
    while (state.GetSnapshot().seq != 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    BOOST_CHECK_EQUAL(state.GetSnapshot().seq, 1);
    // Unsupported reasons fail the stream instead of publishing false metadata.
    BOOST_CHECK_NO_THROW(pool.RemoveRecursive(CTransaction{tx}, nullptr, MemPoolRemovalReason::UNKNOWN));
    while (state.GetStatus() != MempoolNotifierState::Status::FAILED &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    BOOST_CHECK(state.GetStatus() == MempoolNotifierState::Status::FAILED);
    BOOST_CHECK_EQUAL(pool.Size(), 0);
    BOOST_CHECK_NO_THROW(pool.AddUnchecked(tx.GetId(), entry.FromTx(tx), nullptr));
    BOOST_CHECK_EQUAL(pool.Size(), 1);
    BOOST_CHECK_THROW(state.GetSnapshot(), std::runtime_error);
    interface.reset();
    pool.Clear();
}

BOOST_AUTO_TEST_SUITE_END()
