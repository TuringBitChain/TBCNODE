// Copyright (c) 2020 Bitcoin Association.
// Distributed under the Open TBC software license, see the accompanying file LICENSE.

#include <config.h>
#include <hash.h>
#include <net/association.h>
#include <net/net.h>
#include <net/netbase.h>
#include <net/stream.h>
#include <test/test_bitcoin.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>

namespace
{
    CService ip(uint32_t i)
    {
        struct in_addr s;
        s.s_addr = i;
        return CService(CNetAddr(s), Params().GetDefaultPort());
    }

    // Every socket is closed even when a BOOST_REQUIRE aborts the test.
    struct ScopedTestSocket
    {
        SOCKET handle {INVALID_SOCKET};

        ScopedTestSocket() = default;
        ScopedTestSocket(const ScopedTestSocket&) = delete;
        ScopedTestSocket& operator=(const ScopedTestSocket&) = delete;

        ~ScopedTestSocket()
        {
            if (handle != INVALID_SOCKET)
            {
                CloseSocket(handle);
            }
        }

        SOCKET Release()
        {
            const SOCKET socket = handle;
            handle = INVALID_SOCKET;
            return socket;
        }
    };

    enum class SocketEvent { READ, WRITE, EXCEPTION };
    using TestDeadline = std::chrono::steady_clock::time_point;

    bool WaitForSocket(SOCKET socket, SocketEvent event, TestDeadline deadline)
    {
        while (std::chrono::steady_clock::now() < deadline)
        {
            fd_set ready;
            FD_ZERO(&ready);
            FD_SET(socket, &ready);
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            auto timeout = MillisToTimeval(std::max<int64_t>(0, remaining));
            const int result = select(socket + 1,
                event == SocketEvent::READ ? &ready : nullptr,
                event == SocketEvent::WRITE ? &ready : nullptr,
                event == SocketEvent::EXCEPTION ? &ready : nullptr, &timeout);
            if (result == SOCKET_ERROR && WSAGetLastError() == WSAEINTR)
            {
                continue;
            }
            BOOST_REQUIRE_MESSAGE(result != SOCKET_ERROR,
                "select failed: " << NetworkErrorString(WSAGetLastError()));
            return result > 0;
        }
        return false;
    }

    void ConnectLoopback(ScopedTestSocket& sender, ScopedTestSocket& receiver)
    {
        ScopedTestSocket listener;
        listener.handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOST_REQUIRE(listener.handle != INVALID_SOCKET);
        BOOST_REQUIRE(IsSelectableSocket(listener.handle));
        BOOST_REQUIRE(SetSocketNonBlocking(listener.handle, true));

        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        BOOST_REQUIRE_EQUAL(bind(listener.handle, reinterpret_cast<sockaddr*>(&address),
            sizeof(address)), 0);
        BOOST_REQUIRE_EQUAL(listen(listener.handle, 1), 0);
        socklen_t addressSize = sizeof(address);
        BOOST_REQUIRE_EQUAL(getsockname(listener.handle, reinterpret_cast<sockaddr*>(&address),
            &addressSize), 0);

        sender.handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOST_REQUIRE(sender.handle != INVALID_SOCKET);
        BOOST_REQUIRE(IsSelectableSocket(sender.handle));
        BOOST_REQUIRE(SetSocketNonBlocking(sender.handle, true));
#ifdef SO_NOSIGPIPE
        const int noSigpipe = 1;
        BOOST_REQUIRE_EQUAL(setsockopt(sender.handle, SOL_SOCKET, SO_NOSIGPIPE,
            reinterpret_cast<const char*>(&noSigpipe), sizeof(noSigpipe)), 0);
#endif
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        if (connect(sender.handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
        {
            const int error = WSAGetLastError();
            BOOST_REQUIRE(error == WSAEINPROGRESS || error == WSAEWOULDBLOCK || error == WSAEINVAL);
            BOOST_REQUIRE(WaitForSocket(sender.handle, SocketEvent::WRITE, deadline));
            int socketError = 0;
            socklen_t errorSize = sizeof(socketError);
            BOOST_REQUIRE_EQUAL(getsockopt(sender.handle, SOL_SOCKET, SO_ERROR,
                reinterpret_cast<char*>(&socketError), &errorSize), 0);
            BOOST_REQUIRE_EQUAL(socketError, 0);
        }
        BOOST_REQUIRE(WaitForSocket(listener.handle, SocketEvent::READ, deadline));
        receiver.handle = accept(listener.handle, nullptr, nullptr);
        BOOST_REQUIRE(receiver.handle != INVALID_SOCKET);
        BOOST_REQUIRE(IsSelectableSocket(receiver.handle));
        BOOST_REQUIRE(SetSocketNonBlocking(receiver.handle, true));
    }

    void SendBytes(SOCKET socket, const char* data, size_t size, int flags = 0)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        size_t sent = 0;
        while (sent < size)
        {
            BOOST_REQUIRE(std::chrono::steady_clock::now() < deadline);
            const int count = send(socket, data + sent, static_cast<int>(size - sent),
                flags | MSG_NOSIGNAL);
            if (count == SOCKET_ERROR)
            {
                const int error = WSAGetLastError();
                if (error == WSAEINTR)
                {
                    continue;
                }
                BOOST_REQUIRE_MESSAGE(error == WSAEWOULDBLOCK,
                    "send failed: " << NetworkErrorString(error));
                BOOST_REQUIRE(WaitForSocket(socket, SocketEvent::WRITE, deadline));
                continue;
            }
            BOOST_REQUIRE(count > 0);
            sent += static_cast<size_t>(count);
        }
    }

    CDataStream PingMessage(uint64_t nonce)
    {
        CDataStream payload {SER_NETWORK, INIT_PROTO_VERSION};
        payload << nonce;
        CMessageHeader header {Params().NetMagic(), NetMsgType::PING,
            static_cast<unsigned int>(payload.size())};
        const uint256 hash = Hash(payload.begin(), payload.end());
        std::copy_n(hash.begin(), CMessageHeader::CHECKSUM_SIZE, header.pchChecksum);
        CDataStream message {SER_NETWORK, INIT_PROTO_VERSION};
        message << header;
        message.write(payload.data(), payload.size());
        return message;
    }

    size_t ReceiveOneMessage(CNode& node, CConnman& connman, const Config& config)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        size_t totalBytes = 0;
        while (node.vProcessMsg.empty())
        {
            BOOST_REQUIRE(std::chrono::steady_clock::now() < deadline);
            fd_set readSet, writeSet, errorSet;
            FD_ZERO(&readSet);
            FD_ZERO(&writeSet);
            FD_ZERO(&errorSet);
            SOCKET socketMax = 0;
            BOOST_REQUIRE(node.SetSocketsForSelect(readSet, writeSet, errorSet, socketMax));
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            auto timeout = MillisToTimeval(std::max<int64_t>(0, remaining));
            const int result = select(socketMax + 1, &readSet, &writeSet, &errorSet, &timeout);
            if (result == SOCKET_ERROR && WSAGetLastError() == WSAEINTR)
            {
                continue;
            }
            BOOST_REQUIRE_MESSAGE(result > 0, "Timed out or failed while receiving the PING");
            size_t bytesRecv = 0, bytesSent = 0;
            node.ServiceSockets(readSet, writeSet, errorSet, connman, config, bytesRecv, bytesSent);
            totalBytes += bytesRecv;
            BOOST_REQUIRE(!node.GetDisconnect());
        }
        return totalBytes;
    }

    void CheckInitialStreamStats(const StreamStats& stats)
    {
        BOOST_CHECK_EQUAL(stats.streamType, "GENERAL");
        BOOST_CHECK_EQUAL(stats.nLastSend, 0);
        BOOST_CHECK_EQUAL(stats.nLastRecv, 0);
        BOOST_CHECK_EQUAL(stats.nSendBytes, 0);
        BOOST_CHECK_EQUAL(stats.nSendSize, 0);
        BOOST_CHECK_EQUAL(stats.nRecvBytes, 0);
        BOOST_CHECK_EQUAL(stats.nMinuteBytesPerSec, 0);
        BOOST_CHECK_EQUAL(stats.nSpotBytesPerSec, 0);
    }
}

BOOST_FIXTURE_TEST_SUITE(TestNetAssociation, BasicTestingSetup)

// Basic stream testing only without a real network connection
BOOST_AUTO_TEST_CASE(TestBasicStream)
{
    // Create dummy CNode just to be abke to pass it to the CStream
    CAddress dummy_addr{ ip(0xa0b0c001), NODE_NONE };
    CConnman::CAsyncTaskPool asyncTaskPool { GlobalConfig::GetConfig() };
    CNodePtr pDummyNode =
        CNode::Make(
            0,
            NODE_NETWORK,
            0,
            INVALID_SOCKET,
            dummy_addr,
            0u,
            0u,
            asyncTaskPool,
            "",
            true);

    // Create a stream
    Stream stream { *pDummyNode, StreamType::GENERAL, INVALID_SOCKET };

    fd_set readSet, writeSet, errorSet;
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);
    FD_ZERO(&errorSet);
    SOCKET socketMax = 0;
    BOOST_CHECK(!stream.SetSocketForSelect(readSet, writeSet, errorSet, socketMax, false));
    BOOST_CHECK_EQUAL(socketMax, 0);

    // Check initial state
    StreamStats stats {};
    stream.CopyStats(stats);
    CheckInitialStreamStats(stats);

    BOOST_CHECK_EQUAL(stream.GetSendQueueSize(), 0);
    AverageBandwidth abw { stream.GetAverageBandwidth() };
    BOOST_CHECK_EQUAL(abw.first, 0);
    BOOST_CHECK_EQUAL(abw.second, 0);

    // Update avg bandwidth calcs
    std::this_thread::sleep_for(std::chrono::seconds(1));
    stream.AvgBandwithCalc();
    abw = stream.GetAverageBandwidth();
    BOOST_CHECK_EQUAL(abw.first, 0);
    BOOST_CHECK_EQUAL(abw.second, 1);
}

// Basic association testing only without a real network connection
BOOST_AUTO_TEST_CASE(TestBasicAssociation)
{
    // Create dummy CNode just to be abke to pass it to the CStream
    CAddress dummy_addr{ ip(0xa0b0c001), NODE_NONE };
    CConnman::CAsyncTaskPool asyncTaskPool { GlobalConfig::GetConfig() };
    CNodePtr pDummyNode =
        CNode::Make(
            0,
            NODE_NETWORK,
            0,
            INVALID_SOCKET,
            dummy_addr,
            0u,
            0u,
            asyncTaskPool,
            "",
            true);

    // Create an association
    Association association { *pDummyNode, INVALID_SOCKET, dummy_addr };

    // Check initial state
    CAddress peerAddr { association.GetPeerAddr() };
    BOOST_CHECK_EQUAL(peerAddr.ToString(), "1.192.176.160:8333");

    AssociationStats stats {};
    association.CopyStats(stats);
    BOOST_CHECK_EQUAL(stats.streamStats.size(), 1);
    CheckInitialStreamStats(stats.streamStats[0]);
    BOOST_CHECK_EQUAL(stats.nLastSend, 0);
    BOOST_CHECK_EQUAL(stats.nLastRecv, 0);
    BOOST_CHECK_EQUAL(stats.addr.ToString(), "1.192.176.160:8333");
    BOOST_CHECK_EQUAL(stats.nAvgBandwidth, 0);
    BOOST_CHECK_EQUAL(stats.nSendBytes, 0);
    BOOST_CHECK_EQUAL(stats.nRecvBytes, 0);
    BOOST_CHECK_EQUAL(stats.nSendSize, 0);
    for(const auto& cmdTot : stats.mapSendBytesPerMsgCmd)
    {
        BOOST_CHECK_EQUAL(cmdTot.second, 0);
    }
    for(const auto& cmdTot : stats.mapRecvBytesPerMsgCmd)
    {
        BOOST_CHECK_EQUAL(cmdTot.second, 0);
    }

    BOOST_CHECK_EQUAL(association.GetTotalSendQueueSize(), 0);
    BOOST_CHECK_EQUAL(association.GetAverageBandwidth(), 0);
    AverageBandwidth abw { association.GetAverageBandwidth(StreamType::GENERAL) };
    BOOST_CHECK_EQUAL(abw.first, 0);
    BOOST_CHECK_EQUAL(abw.second, 0);

    // Update avg bandwidth calcs
    std::this_thread::sleep_for(std::chrono::seconds(1));
    association.AvgBandwithCalc();
    BOOST_CHECK_EQUAL(association.GetAverageBandwidth(), 0);
    abw = association.GetAverageBandwidth(StreamType::GENERAL);
    BOOST_CHECK_EQUAL(abw.first, 0);
    BOOST_CHECK_EQUAL(abw.second, 1);
}

// A zero receive threshold makes the first complete message pause the real
// CNode receive path, without starting a network or message-handler thread.
BOOST_AUTO_TEST_CASE(TestUrgentDataRespectsReceivePauseWithZeroThreshold)
{
    BOOST_REQUIRE(!g_connman);
    g_connman = std::make_unique<CConnman>(testConfig, 0x1337, 0x1337,
        std::chrono::milliseconds{0});
    BOOST_REQUIRE_EQUAL(g_connman->GetReceiveFloodSize(), 0);

    CConnman::CAsyncTaskPool asyncTaskPool {testConfig};
    ScopedTestSocket sender, receiver;
    ConnectLoopback(sender, receiver);
    const SOCKET receiverSocket = receiver.handle;
    in_addr loopback {};
    loopback.s_addr = htonl(INADDR_LOOPBACK);
    const CAddress peerAddress {CService{CNetAddr{loopback}, 0}, NODE_NONE};
    const CNodePtr node = CNode::Make(0, NODE_NETWORK, 0, receiver.Release(),
        peerAddress, 0u, 0u, asyncTaskPool, "", true);

    const auto firstPing = PingMessage(11);
    const auto secondPing = PingMessage(22);
    BOOST_REQUIRE_EQUAL(firstPing.size(), CMessageHeader::HEADER_SIZE + sizeof(uint64_t));
    SendBytes(sender.handle, firstPing.data(), firstPing.size());
    BOOST_CHECK_EQUAL(ReceiveOneMessage(*node, *g_connman, testConfig), firstPing.size());
    BOOST_REQUIRE_EQUAL(node->vProcessMsg.size(), 1);
    BOOST_CHECK_EQUAL(node->nProcessQueueSize, firstPing.size());
    BOOST_REQUIRE(node->fPauseRecv.load());

    SendBytes(sender.handle, secondPing.data(), secondPing.size());
    const char urgent = '!';
    SendBytes(sender.handle, &urgent, 1, MSG_OOB);
    BOOST_REQUIRE(WaitForSocket(receiverSocket, SocketEvent::EXCEPTION,
        std::chrono::steady_clock::now() + std::chrono::seconds{5}));

    fd_set readSet, writeSet, errorSet;
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);
    FD_ZERO(&errorSet);
    SOCKET socketMax = 0;
    const bool haveSockets = node->SetSocketsForSelect(readSet, writeSet, errorSet, socketMax);
    BOOST_CHECK(!haveSockets);
    BOOST_CHECK(!FD_ISSET(receiverSocket, &readSet));
    BOOST_CHECK(!FD_ISSET(receiverSocket, &errorSet));
    BOOST_CHECK(!FD_ISSET(receiverSocket, &writeSet));
    BOOST_CHECK_EQUAL(socketMax, 0);
    if (haveSockets)
    {
        // Exercise the vulnerable event dispatch when this test runs on the baseline.
        auto timeout = MillisToTimeval(5000);
        BOOST_REQUIRE(select(socketMax + 1, &readSet, &writeSet, &errorSet, &timeout) > 0);
    }
    size_t bytesRecv = 0, bytesSent = 0;
    node->ServiceSockets(readSet, writeSet, errorSet, *g_connman, testConfig, bytesRecv, bytesSent);
    BOOST_CHECK_EQUAL(bytesRecv, 0);
    BOOST_CHECK_EQUAL(bytesSent, 0);
    BOOST_CHECK_EQUAL(node->nProcessQueueSize, firstPing.size());
    BOOST_CHECK(node->fPauseRecv.load());
    BOOST_REQUIRE_EQUAL(node->vProcessMsg.size(), 1);

    {
        LOCK(node->cs_vProcessMsg);
        BOOST_CHECK_EQUAL(node->vProcessMsg.front().hdr.GetCommand(), NetMsgType::PING);
        uint64_t nonce = 0;
        node->vProcessMsg.front().vRecv >> nonce;
        BOOST_CHECK_EQUAL(nonce, 11);
        // Model the consumer releasing its queued message and recomputing backpressure.
        node->vProcessMsg.clear();
        node->nProcessQueueSize = 0;
        node->fPauseRecv = node->nProcessQueueSize > g_connman->GetReceiveFloodSize();
    }
    BOOST_REQUIRE(!node->fPauseRecv.load());
    BOOST_CHECK_EQUAL(ReceiveOneMessage(*node, *g_connman, testConfig), secondPing.size());
    BOOST_REQUIRE_EQUAL(node->vProcessMsg.size(), 1);
    BOOST_CHECK_EQUAL(node->nProcessQueueSize, secondPing.size());
    BOOST_CHECK_EQUAL(node->vProcessMsg.front().hdr.GetCommand(), NetMsgType::PING);
    uint64_t nonce = 0;
    node->vProcessMsg.front().vRecv >> nonce;
    BOOST_CHECK_EQUAL(nonce, 22);
    BOOST_CHECK(node->vProcessMsg.front().vRecv.empty());
    BOOST_CHECK(!node->GetDisconnect());
}

BOOST_AUTO_TEST_SUITE_END();

