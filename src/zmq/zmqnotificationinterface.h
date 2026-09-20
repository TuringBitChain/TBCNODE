// Copyright (c) 2015-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ZMQ_ZMQNOTIFICATIONINTERFACE_H
#define BITCOIN_ZMQ_ZMQNOTIFICATIONINTERFACE_H

#include "validationinterface.h"
#include "txmempool.h"
#include "mempool_notifier_state.h"

#include <list>
#include <map>

class CBlockIndex;
class CZMQAbstractNotifier;
class CZMQPublishTxInMempoolNotifier;

class CZMQNotificationInterface final : public CValidationInterface {
public:
    virtual ~CZMQNotificationInterface();

    static CZMQNotificationInterface *Create();

    // Startup enables this state after binding the configured publishers.
    MempoolNotifierState& GetMempoolState() { return mMempoolState; }

    // The event producer must allocate/enqueue positions under the mempool
    // lock, then call this from an ordered publisher without that lock.
    bool PublishMempoolTransaction(const uint256& txid,
                                  MempoolNotifierState::Position position,
                                  std::optional<MemPoolRemovalReason> reason = std::nullopt);

protected:
    bool Initialize();
    void Shutdown();

    // CValidationInterface
    void TransactionAddedToMempool(const CTransactionRef &tx) override;
    void TransactionAdded(const CTransactionRef& tx) override;
    void TransactionDiscardedFromMempool(const uint256& txid,
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
    MempoolNotifierState mMempoolState;
    CZMQPublishTxInMempoolNotifier* mMempoolNotifier{nullptr};
};

#endif // BITCOIN_ZMQ_ZMQNOTIFICATIONINTERFACE_H
