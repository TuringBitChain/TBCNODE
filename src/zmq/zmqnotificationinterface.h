// Copyright (c) 2015-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ZMQ_ZMQNOTIFICATIONINTERFACE_H
#define BITCOIN_ZMQ_ZMQNOTIFICATIONINTERFACE_H

#include "validation/async_subscriber.h"   // v2.6.1 P4 §5.3
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
    // v2.6.1 P4 §5.3：以下 callback 在 ConnectTip 三锁帧内被同步调用，
    //                 实现 push 到 m_async_worker 即返回，真正的 zmq_send
    //                 在独立 worker 线程执行，避免 socket 慢 IO 阻塞主验证链。
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

    // v2.6.1 P4 §5.3：单线程 FIFO 异步派发，保证 ZMQ 发送顺序与
    // 主链信号触发顺序一致；shutdown 时优雅 drain。
    tbc::validation::AsyncSubscriber m_async_worker{"zmq", 16'384};
};

#endif // BITCOIN_ZMQ_ZMQNOTIFICATIONINTERFACE_H
