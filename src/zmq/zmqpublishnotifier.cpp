// Copyright (c) 2015-2016 The Bitcoin Core developers
// Copyright (c) 2019 Bitcoin Association
// Distributed under the Open TBC software license, see the accompanying file LICENSE.

#include "zmqpublishnotifier.h"
#include "config.h"
#include "core_io.h"
#include "rpc/server.h"
#include "rpc/jsonwriter.h"
#include "rpc/text_writer.h"
#include "streams.h"
#include "util.h"
#include "validation.h"

#include <cstdarg>

static std::multimap<std::string, CZMQAbstractPublishNotifier *>
    mapPublishNotifiers;

static const char *MSG_HASHBLOCK = "hashblock";
static const char *MSG_HASHTX = "hashtx";
static const char *MSG_RAWBLOCK = "rawblock";
static const char *MSG_RAWTX = "rawtx";

static const char* const MSG_HASHBLOCKNEW = "hashblockincr";
static const char* const MSG_RAWBLOCKNEW = "rawblockincr";
static const char* const MSG_HASHTXINCR = "hashtxincr";
static const char* const MSG_RAWTXINCR = "rawtxincr";
static const char *MSG_DISCARDEDFROMMEMPOOL = "discardedfrommempool";
static const char *MSG_REMOVEDFROMMEMPOOLBLOCK = "removedfrommempoolblock";

// Internal function to send multipart message
static int zmq_send_multipart(void *sock, const void *data, size_t size, ...) {
    va_list args;
    auto closeVaList = [](va_list* args) {va_end(*args);};
    va_start(args, size);
    std::unique_ptr<va_list, decltype(closeVaList)> guard{&args, closeVaList};

    while (1) {
        zmq_msg_t msg;

        int rc = zmq_msg_init_size(&msg, size);
        if (rc != 0) {
            zmqError("Unable to initialize ZMQ msg");
            return -1;
        }

        void *buf = zmq_msg_data(&msg);
        memcpy(buf, data, size);

        data = va_arg(args, const void *);

        rc = zmq_msg_send(&msg, sock, data ? ZMQ_SNDMORE : 0);
        if (rc == -1) {
            zmqError("Unable to send ZMQ msg");
            zmq_msg_close(&msg);
            return -1;
        }

        zmq_msg_close(&msg);

        if (!data) break;

        size = va_arg(args, size_t);
    }
    return 0;
}

bool CZMQAbstractPublishNotifier::Initialize(void *pcontext) {
    assert(!psocket);

    // check if address is being used by other publish notifier
    std::multimap<std::string, CZMQAbstractPublishNotifier *>::iterator i =
        mapPublishNotifiers.find(address);

    if (i == mapPublishNotifiers.end()) {
        psocket = zmq_socket(pcontext, ZMQ_PUB);
        if (!psocket) {
            zmqError("Failed to create socket");
            return false;
        }

        int rc = zmq_bind(psocket, address.c_str());
        if (rc != 0) {
            zmqError("Failed to bind address");
            zmq_close(psocket);
            return false;
        }

        // register this notifier for the address, so it can be reused for other
        // publish notifier
        mapPublishNotifiers.insert(std::make_pair(address, this));
        return true;
    } else {
        LogPrint(BCLog::ZMQ, "zmq: Reusing socket for address %s\n", address);

        psocket = i->second->psocket;
        mapPublishNotifiers.insert(std::make_pair(address, this));

        return true;
    }
}

void CZMQAbstractPublishNotifier::Shutdown() {
    assert(psocket);

    int count = mapPublishNotifiers.count(address);

    // remove this notifier from the list of publishers using this address
    typedef std::multimap<std::string, CZMQAbstractPublishNotifier *>::iterator
        iterator;
    std::pair<iterator, iterator> iterpair =
        mapPublishNotifiers.equal_range(address);

    for (iterator it = iterpair.first; it != iterpair.second; ++it) {
        if (it->second == this) {
            mapPublishNotifiers.erase(it);
            break;
        }
    }

    if (count == 1) {
        LogPrint(BCLog::ZMQ, "Close socket at address %s\n", address);
        int linger = 0;
        zmq_setsockopt(psocket, ZMQ_LINGER, &linger, sizeof(linger));
        zmq_close(psocket);
    }

    psocket = 0;
}

bool CZMQAbstractPublishNotifier::SendZMQMessage(const char *command,
                                              const void *data, size_t size) {
    assert(psocket);

    /* send three parts, command & data & a LE 4byte sequence number */
    uint8_t msgseq[sizeof(uint32_t)];
    WriteLE32(&msgseq[0], nSequence);
    int rc = zmq_send_multipart(psocket, command, strlen(command), data, size,
                                msgseq, (size_t)sizeof(uint32_t), (void *)0);
    if (rc == -1) return false;

    /* increment memory only sequence number after sending */
    nSequence++;

    return true;
}

bool CZMQAbstractPublishNotifier::SendZMQMessage(const char* command, const uint256& hash) 
{
    LogPrint(BCLog::ZMQ, "zmq: Publish %s %s\n", command, hash.GetHex());
    char data[32];
    for (unsigned int i = 0; i < 32; i++) 
    {
        data[31 - i] = hash.begin()[i];
    }
    return SendZMQMessage(command, data, 32);
}

bool CZMQAbstractPublishNotifier::SendZMQMessage(const char* command, const CBlockIndex* pindex) 
{
    LogPrint(BCLog::ZMQ, "zmq: Publish  %s %s\n", command, pindex->GetBlockHash().GetHex());

    const Config& config = GlobalConfig::GetConfig();
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION | RPCSerializationFlags());
    {
        LOCK(cs_main);
        CBlock block;
        if (!ReadBlockFromDisk(block, pindex, config))
        {
            zmqError("Can't read block from disk");
            return false;
        }

        ss << block;
    }

    return SendZMQMessage(command, &(*ss.begin()), ss.size());
}

bool CZMQAbstractPublishNotifier::SendZMQMessage(const char* command, const CTransaction& transaction) 
{
    uint256 txid = transaction.GetId();
    LogPrint(BCLog::ZMQ, "zmq: Publish %s %s\n", command, txid.GetHex());
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION | RPCSerializationFlags());
    ss << transaction;
    return SendZMQMessage(command, &(*ss.begin()), ss.size());
}

bool CZMQPublishHashBlockNotifier::NotifyBlock(const CBlockIndex *pindex) {
    uint256 hash = pindex->GetBlockHash();
    LogPrint(BCLog::ZMQ, "zmq: Publish hashblock %s\n", hash.GetHex());
    char data[32];
    for (unsigned int i = 0; i < 32; i++)
        data[31 - i] = hash.begin()[i];
    return SendZMQMessage(MSG_HASHBLOCK, data, 32);
}

bool CZMQPublishHashTransactionNotifier::NotifyTransaction(
    const CTransaction &transaction) {
    uint256 txid = transaction.GetId();
    LogPrint(BCLog::ZMQ, "zmq: Publish hashtx %s\n", txid.GetHex());
    char data[32];
    for (unsigned int i = 0; i < 32; i++)
        data[31 - i] = txid.begin()[i];
    return SendZMQMessage(MSG_HASHTX, data, 32);
}

bool CZMQPublishRemovedFromMempoolNotifier::NotifyRemovedFromMempool(const uint256& txid,
                                                                     MemPoolRemovalReason reason,
                                                                     const CTransaction* conflictedWith)
{

    CStringWriter tw;
    CJSONWriter jw(tw, false);

    jw.writeBeginObject();
    jw.pushKV("txid", txid.GetHex(), true);

    switch (reason)
    {
        case MemPoolRemovalReason::EXPIRY:
            jw.pushKV("reason", "expired", false);
            break;
        case MemPoolRemovalReason::SIZELIMIT:
            jw.pushKV("reason", "mempool-sizelimit-exceeded", false);
            break;
        case MemPoolRemovalReason::CONFLICT:
            if (conflictedWith != nullptr)
            {
                jw.pushKV("reason", "collision-in-block-tx");
                jw.writeBeginObject("collidedWith");
                jw.pushKV("txid", conflictedWith->GetId().GetHex(), true);
                jw.pushKV("size", int64_t(conflictedWith->GetTotalSize()), true);
                jw.pushK("hex");
                jw.pushQuote(true, false);
                EncodeHexTx(*conflictedWith, jw.getWriter(), 0);
                jw.pushQuote(true, false);
                jw.writeEndObject(false);
            }
            else
            {
                jw.pushKV("reason", "collision-in-block-tx", false);
            }
            break;
        default:
            jw.pushKV("reason", "unknown-reason", false);
    }

    jw.writeEndObject(false);

    std::string message = tw.MoveOutString();

    return SendZMQMessage(MSG_DISCARDEDFROMMEMPOOL, message.data(), message.size());
}

bool CZMQPublishRemovedFromMempoolBlockNotifier::NotifyDiscardedFromMempoolBlock(const uint256& txid,
                                                                               MemPoolRemovalReason reason)
{

    CStringWriter tw;
    CJSONWriter jw(tw, false);

    jw.writeBeginObject();

    switch (reason)
    {
        case MemPoolRemovalReason::REORG:
            jw.pushKV("reason", "reorg");
            break;
        case MemPoolRemovalReason::BLOCK:
            jw.pushKV("reason", "included-in-block");
            break;
        default:
            jw.pushKV("reason", "unknown-reason");
    }
    
    jw.pushK("txid");
    jw.pushV(txid.GetHex(), false);
    jw.writeEndObject(false);

    std::string message = tw.MoveOutString();

    return SendZMQMessage(MSG_REMOVEDFROMMEMPOOLBLOCK, message.data(), message.size());
}

bool CZMQPublishRawBlockNotifier::NotifyBlock(const CBlockIndex *pindex) {
    return SendZMQMessage(MSG_RAWBLOCK, pindex);
}

bool CZMQPublishRawTransactionNotifier::NotifyTransaction(
    const CTransaction &transaction) {
    return SendZMQMessage(MSG_RAWTX, transaction);
}

bool CZMQPublishHashBlockNotifier2::NotifyBlock2(const CBlockIndex* pindex) 
{
    return SendZMQMessage(MSG_HASHBLOCKNEW, pindex->GetBlockHash());
}

bool CZMQPublishRawBlockNotifier2::NotifyBlock2(const CBlockIndex* pindex) 
{
    return SendZMQMessage(MSG_RAWBLOCKNEW, pindex);
}

bool CZMQPublishHashTransactionNotifier2::NotifyTransaction2(const CTransaction& transaction) 
{
    return SendZMQMessage(MSG_HASHTXINCR, transaction.GetId());
}

bool CZMQPublishRawTransactionNotifier2::NotifyTransaction2(const CTransaction& transaction) 
{
    return SendZMQMessage(MSG_RAWTXINCR, transaction);
}
