// Distributed under the Open TBC software license, see the accompanying file LICENSE.

#include "zmq/zmqnotificationinterface.h"
#include "zmq/zmqpublishnotifier.h"
#include "chain.h"
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
            {MemPoolRemovalReason::SIZELIMIT, "sizelimit"},
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

BOOST_AUTO_TEST_SUITE_END()
