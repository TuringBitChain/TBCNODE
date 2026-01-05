// Copyright (c) 2015-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ZMQ_ZMQNOTIFICATIONINTERFACE_H
#define BITCOIN_ZMQ_ZMQNOTIFICATIONINTERFACE_H

#include "validationinterface.h"
#include "txmempool.h"

#include <list>
#include <map>

class CBlockIndex;
class CZMQAbstractNotifier;

class CZMQNotificationInterface final : public CValidationInterface {
public:
    virtual ~CZMQNotificationInterface();

    static CZMQNotificationInterface *Create();

protected:
    bool Initialize();
    void Shutdown();

    // CValidationInterface
    void TransactionAddedToMempool(const CTransactionRef &tx) override;
    void TransactionAdded(const CTransactionRef& tx) override;
    void TransactionRemovedFromMempool(const uint256& txid,
                                       MemPoolRemovalReason reason,
                                       const CTransaction* conflictedWith) override;
    void TransactionRemovedFromMempoolBlock(const uint256& txid,
                                            MemPoolRemovalReason reason) override;
    void
    BlockConnected(const std::shared_ptr<const CBlock> &pblock,
                   const CBlockIndex *pindexConnected,
                   const std::vector<CTransactionRef> &vtxConflicted) override;
    void
    BlockDisconnected(const std::shared_ptr<const CBlock> &pblock) override;
    void BlockConnected2(const CBlockIndex* pindexConnected,
                   const std::vector<CTransactionRef>& vtxNew) override;
    void UpdatedBlockTip(const CBlockIndex *pindexNew,
                         const CBlockIndex *pindexFork,
                         bool fInitialDownload) override;

private:
    CZMQNotificationInterface();

    void *pcontext;
    std::list<CZMQAbstractNotifier *> notifiers;
};

#endif // BITCOIN_ZMQ_ZMQNOTIFICATIONINTERFACE_H
