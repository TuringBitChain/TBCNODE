// Copyright (c) 2020 Bitcoin Association
// Distributed under the Open TBC software license, see the accompanying file LICENSE.

#include <net/net.h>
#include <net/netbase.h>
#include <net/stream.h>

// Enable enum_cast for StreamType, so we can log informatively
const enumTableT<StreamType>& enumTable(StreamType)
{
    static enumTableT<StreamType> table
    {
        { StreamType::UNKNOWN,     "UNKNOWN" },
        { StreamType::GENERAL,     "GENERAL" },
        { StreamType::BLOCK,       "BLOCK" },
        { StreamType::TRANSACTION, "TRANSACTION" }
    };
    return table;
}

namespace
{
    bool IsOversizedMessage(const Config& config, const CNetMessage& msg)
    {   
        if(!msg.in_data)
        {   
            // Header only, cannot be oversized.
            return false;
        }

        return msg.hdr.IsOversized(config);
    }
}

Stream::Stream(CNode& node, StreamType streamType, SOCKET socket)
: mNode{node}, mStreamType{streamType}, mSocket{socket}
{
}

Stream::~Stream()
{
    Shutdown();
}

void Stream::Shutdown()
{
    // Close the socket connection
    LOCK(cs_mSocket);
    if(mSocket != INVALID_SOCKET)
    {
        LogPrint(BCLog::NET, "closing %s stream to peer=%d\n", enum_cast<std::string>(mStreamType),
            mNode.GetId());
        CloseSocket(mSocket);
    }
}

bool Stream::SetSocketForSelect(fd_set& setRecv, fd_set& setSend, fd_set& setError,
                                 SOCKET& socketMax, bool pauseRecv) const
{
    // Implement the following logic:
    // * If there is data to send select() for sending data.
    // * If there is space left in the receive buffer select() for receiving data.

    bool select_recv = !pauseRecv;
    bool select_send;
    {   
        LOCK(cs_mSendMsgQueue);
        select_send = !mSendMsgQueue.empty();
    }

    LOCK(cs_mSocket);
    if(mSocket == INVALID_SOCKET)
    {   
        return false;
    }

    FD_SET(mSocket, &setError);
    socketMax = std::max(socketMax, mSocket);

    if(select_send)
    {   
        FD_SET(mSocket, &setSend);
    }
    if(select_recv)
    {   
        FD_SET(mSocket, &setRecv);
    }

    return true;
}

void Stream::ServiceSocket(fd_set& setRecv, fd_set& setSend, fd_set& setError,
                            CConnman& connman, const Config& config, const CNetAddr& peerAddr,
                            bool& gotNewMsgs, size_t& bytesRecv, size_t& bytesSent)
{
    //
    // Receive
    //
    bool recvSet = false;
    bool sendSet = false;
    bool errorSet = false;
    {   
        LOCK(cs_mSocket);
        if (mSocket == INVALID_SOCKET)
        {   
            return;
        }
        recvSet = FD_ISSET(mSocket, &setRecv);
        sendSet = FD_ISSET(mSocket, &setSend);
        errorSet = FD_ISSET(mSocket, &setError);
    }
    if (recvSet || errorSet)
    {   
        // typical socket buffer is 8K-64K
        char pchBuf[0x10000];
        ssize_t nBytes = 0;
        {   
            LOCK(cs_mSocket);
            if (mSocket == INVALID_SOCKET)
            {   
                return;
            }
            nBytes = recv(mSocket, pchBuf, sizeof(pchBuf), MSG_DONTWAIT);
        }
        if (nBytes > 0)
        {   
            bytesRecv = static_cast<size_t>(nBytes);
            const RECV_STATUS status = ReceiveMsgBytes(config, pchBuf, bytesRecv, gotNewMsgs);
            if (status != RECV_OK)
            {   
                LogPrintf("node addrName:%s CloseSocketDisconnect line:%d\n",mNode.GetAddrName(),__LINE__);
                mNode.CloseSocketDisconnect();
                if (status == RECV_BAD_LENGTH)
                {   
                    // Ban the peer if try to send messages with bad length
                    connman.Ban(peerAddr, BanReasonNodeMisbehaving);
                }
            }
        }
        else if (nBytes == 0)
        {   
            // socket closed gracefully
            if (!mNode.GetDisconnect())
            {   
                LogPrint(BCLog::NET, "stream socket closed\n");
            }
            LogPrintf("node addrName:%s CloseSocketDisconnect line:%d\n",mNode.GetAddrName(),__LINE__);
            mNode.CloseSocketDisconnect();
        }
        else if (nBytes < 0)
        {
            // error
            int nErr = WSAGetLastError();
            if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS)
            {
                if (!mNode.GetDisconnect())
                {
                    LogPrintf("stream socket recv addr:%s error %s\n", peerAddr.ToStringIP(),NetworkErrorString(nErr));
                }
        LogPrintf("node addrName:%s CloseSocketDisconnect line:%d\n",mNode.GetAddrName(),__LINE__);
                mNode.CloseSocketDisconnect();
            }
        }
    }

    //
    // Send
    //
    if (sendSet)
    {
        bytesSent = SocketSendData();
    }
}

size_t Stream::PushMessage(std::vector<uint8_t>&& serialisedHeader, CSerializedNetMsg&& msg,
    size_t nPayloadLength, size_t nTotalSize)
{   
    size_t nBytesSent {0};

    LOCK(cs_mSendMsgQueue);
    bool optimisticSend { mSendMsgQueue.empty() };

    // Track send queue length
    mSendMsgQueueSize += nTotalSize;

    mSendMsgQueue.push_back(std::make_unique<CVectorStream>(std::move(serialisedHeader)));
    if(nPayloadLength)
    {   
        mSendMsgQueue.push_back(msg.MoveData());
    }

    // If write queue empty, attempt "optimistic write"
    if(optimisticSend)
    {   
        nBytesSent = SocketSendData();
    }

    return nBytesSent;
}

size_t Stream::GetNewMsgs(std::list<CNetMessage>& msgList)
{
    size_t nSizeAdded {0};

    LOCK(cs_mRecvMsgQueue);
    auto it { mRecvMsgQueue.begin() };
    for(; it != mRecvMsgQueue.end(); ++it)
    {   
        if(!it->complete())
        {   
            break;
        }
        nSizeAdded += it->vRecv.size() + CMessageHeader::HEADER_SIZE;
    }

    msgList.splice(msgList.end(), mRecvMsgQueue, mRecvMsgQueue.begin(), it);

    return nSizeAdded;
}

void Stream::CopyStats(StreamStats& stats) const
{
    stats.streamType = enum_cast<std::string>(mStreamType);
    stats.nLastSend = mLastSendTime;
    stats.nLastRecv = mLastRecvTime;

    {
        LOCK(cs_mSendMsgQueue);
        stats.nSendBytes = mTotalBytesSent;
        stats.nSendSize = mSendMsgQueueSize.getSendQueueBytes();
    }

    {
        LOCK(cs_mRecvMsgQueue);
        stats.nRecvBytes = mTotalBytesRecv;

        // Avg bandwidth measurements
        if(!mAvgBandwidth.empty())
        {
            stats.nMinuteBytesPerSec = GetAverageBandwidth().first;
            stats.nSpotBytesPerSec = static_cast<uint64_t>(mAvgBandwidth.back());
        }
        else
        {
            stats.nMinuteBytesPerSec = 0;
            stats.nSpotBytesPerSec = 0;
        }
    }
}

void Stream::AvgBandwithCalc()
{   
    LOCK(cs_mRecvMsgQueue);
    int64_t currTime { GetTimeMicros() };
    if(mLastSpotMeasurementTime > 0)
    {   
        double secsSinceLastSpot { static_cast<double>(currTime - mLastSpotMeasurementTime) / MICROS_PER_SECOND };
        if(secsSinceLastSpot > 0)
        {   
            double spotbw { mBytesRecvThisSpot / secsSinceLastSpot };
            mAvgBandwidth.push_back(spotbw);
        }
    }

    mLastSpotMeasurementTime = currTime;
    mBytesRecvThisSpot = 0;
}

// If we have sufficient samples then get average bandwidth from node,
// otherwise we must be in early startup measuring the bandwidth so just
// report it as 0.
AverageBandwidth Stream::GetAverageBandwidth() const
{
    LOCK(cs_mRecvMsgQueue);

    if(!mAvgBandwidth.empty())
    {   
        // If we don't yet have a full minutes worth of measurements then just
        // average with what we have
        uint64_t avg { static_cast<uint64_t>(Average(mAvgBandwidth.begin(), mAvgBandwidth.end())) };
        return { avg, mAvgBandwidth.size() };
    }

    return {0,0};
}

size_t Stream::GetSendQueueSize() const
{
    LOCK(cs_mSendMsgQueue);
    return mSendMsgQueueSize.getSendQueueBytes();
}

Stream::RECV_STATUS Stream::ReceiveMsgBytes(const Config& config, const char* pch, size_t nBytes,
    bool& complete)
{
    complete = false;
    int64_t nTimeMicros = GetTimeMicros();

    LOCK(cs_mRecvMsgQueue);
    mLastRecvTime = nTimeMicros / MICROS_PER_SECOND;
    mTotalBytesRecv += nBytes;
    mBytesRecvThisSpot += nBytes;

    while (nBytes > 0)
    {   
        // Get current incomplete message, or create a new one.
        if (mRecvMsgQueue.empty() || mRecvMsgQueue.back().complete())
        {   
            mRecvMsgQueue.push_back(CNetMessage(Params().NetMagic(), SER_NETWORK, INIT_PROTO_VERSION));
        }

        CNetMessage& msg = mRecvMsgQueue.back();

        // Absorb network data.
        int handled;
        if (!msg.in_data)
        {   
            handled = msg.readHeader(config, pch, nBytes);
            if (handled < 0)
            {   
                return RECV_BAD_LENGTH;//Notify bad message as soon as seen in the header
            }
        }
        else
        {   
            handled = msg.readData(pch, nBytes);
        }

        if (handled < 0)
        {   
            return RECV_FAIL;
        }

        if (IsOversizedMessage(config, msg))
        {   
            LogPrint(BCLog::NET, "Oversized message from peer=%i, disconnecting\n", mNode.GetId());
            return RECV_BAD_LENGTH;
        }

        pch += handled;
        nBytes -= handled;

        if (msg.complete())
        {      
            msg.nTime = nTimeMicros;
            complete = true;
        }
    }   
    
    return RECV_OK;
}   

size_t Stream::SocketSendData()
{   
    size_t nSentSize = 0;
    size_t nMsgCount = 0;
    size_t nSendBufferMaxSize = g_connman->GetSendBufferSize();

    LOCK(cs_mSendMsgQueue);

    for(const auto& data : mSendMsgQueue)
    {   
        auto sent = SendMessage(*data, nSendBufferMaxSize);
        nSentSize += sent.sentSize;
        mSendMsgQueueSize -= sent.sentSize;

        if(sent.sendComplete == false)
        {   
            break;
        }

        ++nMsgCount;
    }

    mSendMsgQueue.erase(mSendMsgQueue.begin(), mSendMsgQueue.begin() + nMsgCount);

    if (mSendMsgQueue.empty())
    {   
        assert(!mSendChunk);
        assert(mSendMsgQueueSize.getSendQueueBytes() == 0);
    }

    return nSentSize;
}

Stream::CSendResult Stream::SendMessage(CForwardAsyncReadonlyStream& data, size_t maxChunkSize)
{   
    if (maxChunkSize == 0)
    {   
        // if maxChunkSize is 0 assign some default chunk size value
        maxChunkSize = 1024;
    }
    size_t sentSize = 0;

    do
    {   
        int nBytes = 0;
        if (!mSendChunk)
        {   
            mSendChunk = data.ReadAsync(maxChunkSize);

            if (!mSendChunk->Size())
            {   
                // we need to wait for data to load so we should let others
                // send data in the meantime
                mSendChunk = std::nullopt;
                return {false, sentSize};
            }
        }

        {   
            LOCK(cs_mSocket);
            if (mSocket == INVALID_SOCKET)
            {   
                return {false, sentSize};
            }

            nBytes = send(mSocket,
                          reinterpret_cast<const char *>(mSendChunk->Begin()),
                          mSendChunk->Size(),
                          MSG_NOSIGNAL | MSG_DONTWAIT);
        }

        if (nBytes == 0)
        {   
            // couldn't send anything at all
            return {false, sentSize};
        }
        if (nBytes < 0)
        {   
            // error
            int nErr = WSAGetLastError();
            if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS)
            {
                LogPrintf("socket send error %s\n", NetworkErrorString(nErr));
        LogPrintf("node addrName:%s CloseSocketDisconnect line:%d\n",mNode.GetAddrName(),__LINE__);
                mNode.CloseSocketDisconnect();
            }

            return {false, sentSize};
        }

        assert(nBytes > 0);
        mLastSendTime = GetSystemTimeInSeconds();
        mTotalBytesSent += nBytes;
        sentSize += nBytes;
        if (static_cast<size_t>(nBytes) != mSendChunk->Size())
        {
            // could not send full message; stop sending more
            mSendChunk =
                CSpan {
                    mSendChunk->Begin() + nBytes,
                    mSendChunk->Size() - nBytes
                };
            return {false, sentSize};
        }

        mSendChunk = std::nullopt;
    } while(!data.EndOfStream());

    return {true, sentSize};
}

