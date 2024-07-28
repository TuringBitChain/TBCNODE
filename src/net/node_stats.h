// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2017 The Bitcoin developers
// Copyright (c) 2020 Bitcoin Association
// Distributed under the Open TBC software license, see the accompanying file LICENSE.

#pragma once

#include <net/net_types.h>

class StreamStats
{
public:
    std::string streamType;
    int64_t nLastSend;
    int64_t nLastRecv;
    uint64_t nSendBytes;
    uint64_t nRecvBytes;
    uint64_t nSendSize;
    uint64_t nSpotBytesPerSec;
    uint64_t nMinuteBytesPerSec;
};

class AssociationStats
{
public:
    int64_t nLastSend;
    int64_t nLastRecv;
    CAddress addr;
    mapMsgCmdSize mapSendBytesPerMsgCmd;
    mapMsgCmdSize mapRecvBytesPerMsgCmd;
    uint64_t nSendBytes;
    uint64_t nRecvBytes;
    uint64_t nSendSize;
    uint64_t nAvgBandwidth;

    std::vector<StreamStats> streamStats;
};

class NodeStats
{
public:
    NodeId nodeid;
    ServiceFlags nServices;
    bool fRelayTxes;
    bool fPauseSend;
    bool fPauseRecv;
    bool fInbound;
    bool fAddnode;
    bool fWhitelisted;
    int64_t nTimeConnected;
    int64_t nTimeOffset;
    std::string addrName;
    int nVersion;
    std::string cleanSubVer;
    int nStartingHeight;
    double dPingTime;
    double dPingWait;
    double dMinPing;
    // What this peer sees as my address
    std::string addrLocal;
    size_t nInvQueueSize;

    AssociationStats associationStats;
};
