// Copyright (c) 2015-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "zmqnotificationinterface.h"
#include "zmqpublishnotifier.h"

#include "streams.h"
#include "util.h"
#include "validation.h"
#include "version.h"

void zmqError(const char *str) {
    LogPrint(BCLog::ZMQ, "zmq: Error: %s, errno=%s\n", str,
             zmq_strerror(errno));
}

CZMQNotificationInterface::CZMQNotificationInterface() : pcontext(nullptr) {}

CZMQNotificationInterface::~CZMQNotificationInterface() {
    Shutdown();

    for (std::list<CZMQAbstractNotifier *>::iterator i = notifiers.begin();
         i != notifiers.end(); ++i) {
        delete *i;
    }
}

CZMQNotificationInterface *CZMQNotificationInterface::Create() {
    CZMQNotificationInterface *notificationInterface = nullptr;
    std::map<std::string, CZMQNotifierFactory> factories;
    std::list<CZMQAbstractNotifier *> notifiers;

    factories["pubhashblock"] =
        CZMQAbstractNotifier::Create<CZMQPublishHashBlockNotifier>;
    factories["pubhashtx"] =
        CZMQAbstractNotifier::Create<CZMQPublishHashTransactionNotifier>;
    factories["pubrawblock"] =
        CZMQAbstractNotifier::Create<CZMQPublishRawBlockNotifier>;
    factories["pubrawtx"] =
        CZMQAbstractNotifier::Create<CZMQPublishRawTransactionNotifier>;
    factories["pubdiscardedfrommempool"] =
        CZMQAbstractNotifier::Create<CZMQPublishRemovedFromMempoolNotifier>;
    factories["pubremovedfrommempoolblock"] =
        CZMQAbstractNotifier::Create<CZMQPublishRemovedFromMempoolBlockNotifier>;
    factories["pubhashblockincr"] =
        CZMQAbstractNotifier::Create<CZMQPublishHashBlockNotifier2>;
    factories["pubrawblockincr"] =
        CZMQAbstractNotifier::Create<CZMQPublishRawBlockNotifier2>;
    factories["pubhashtxincr"] =
        CZMQAbstractNotifier::Create<CZMQPublishHashTransactionNotifier2>;
    factories["pubrawtxincr"] =
        CZMQAbstractNotifier::Create<CZMQPublishRawTransactionNotifier2>;

    for (std::map<std::string, CZMQNotifierFactory>::const_iterator i =
             factories.begin();
         i != factories.end(); ++i) {
        std::string arg("-zmq" + i->first);
        if (gArgs.IsArgSet(arg)) {
            CZMQNotifierFactory factory = i->second;
            std::string address = gArgs.GetArg(arg, "");
            CZMQAbstractNotifier *notifier = factory();
            notifier->SetType(i->first);
            notifier->SetAddress(address);
            notifiers.push_back(notifier);
        }
    }

    if (!notifiers.empty()) {
        notificationInterface = new CZMQNotificationInterface();
        notificationInterface->notifiers = notifiers;

        if (!notificationInterface->Initialize()) {
            delete notificationInterface;
            notificationInterface = nullptr;
        }
    }

    return notificationInterface;
}

// Called at startup to conditionally set up ZMQ socket(s)
bool CZMQNotificationInterface::Initialize() {
    int major = 0, minor = 0, patch = 0;
    zmq_version(&major, &minor, &patch);
    LogPrint(BCLog::ZMQ, "zmq: version %d.%d.%d\n", major, minor, patch);

    LogPrint(BCLog::ZMQ, "zmq: Initialize notification interface\n");
    assert(!pcontext);

    pcontext = zmq_ctx_new();

    if (!pcontext) {
        zmqError("Unable to initialize context");
        return false;
    }

    std::list<CZMQAbstractNotifier *>::iterator i = notifiers.begin();
    for (; i != notifiers.end(); ++i) {
        CZMQAbstractNotifier *notifier = *i;
        if (notifier->Initialize(pcontext)) {
            LogPrint(BCLog::ZMQ, "  Notifier %s ready (address = %s)\n",
                     notifier->GetType(), notifier->GetAddress());
        } else {
            LogPrint(BCLog::ZMQ, "  Notifier %s failed (address = %s)\n",
                     notifier->GetType(), notifier->GetAddress());
            break;
        }
    }

    if (i != notifiers.end()) {
        return false;
    }

    // v2.6.1 P4 §5.3：启动异步 worker（必须在 notifiers ready 之后；
    // RegisterValidationInterface 注册后才会进 callback）
    m_async_worker.Start();
    LogPrint(BCLog::ZMQ, "zmq: async subscriber worker started\n");

    return true;
}

// Called during shutdown sequence
void CZMQNotificationInterface::Shutdown() {
    LogPrint(BCLog::ZMQ, "zmq: Shutdown notification interface\n");
    // v2.6.1 P4 §5.3：先 stop async worker，确保不再有 task 引用 notifiers
    m_async_worker.Stop();
    if (pcontext) {
        for (std::list<CZMQAbstractNotifier *>::iterator i = notifiers.begin();
             i != notifiers.end(); ++i) {
            CZMQAbstractNotifier *notifier = *i;
            LogPrint(BCLog::ZMQ, "   Shutdown notifier %s at %s\n",
                     notifier->GetType(), notifier->GetAddress());
            notifier->Shutdown();
        }
        zmq_ctx_term(pcontext);

        pcontext = 0;
    }
}

// v2.6.1 P4 §5.3：Worker-thread-only helpers.
// 这些函数 ONLY 在 m_async_worker 线程上下文执行。CValidationInterface
// 的 callback 入口仅做 lambda capture + Enqueue，立刻返回到主验证链帧。
// notifiers 链表只被 worker 线程读写（Shutdown 时 worker 已 stop）→ 无需额外锁。

void CZMQNotificationInterface::UpdatedBlockTip(const CBlockIndex *pindexNew,
                                                const CBlockIndex *pindexFork,
                                                bool fInitialDownload) {
    // In IBD or blocks were disconnected without any new ones
    if (fInitialDownload || pindexNew == pindexFork) return;

    // pindexNew 是 mapBlockIndex 长寿对象，capture 指针安全
    m_async_worker.Enqueue([this, pindexNew] {
        for (auto i = notifiers.begin(); i != notifiers.end();) {
            CZMQAbstractNotifier *notifier = *i;
            if (notifier->NotifyBlock(pindexNew)) {
                ++i;
            } else {
                notifier->Shutdown();
                i = notifiers.erase(i);
                delete notifier;
            }
        }
    });
}

void CZMQNotificationInterface::TransactionAddedToMempool(
    const CTransactionRef &ptx) {
    // capture by value 保证 worker 线程取到有效 tx
    m_async_worker.Enqueue([this, ptx] {
        const CTransaction &tx = *ptx;
        for (auto i = notifiers.begin(); i != notifiers.end();) {
            CZMQAbstractNotifier *notifier = *i;
            if (notifier->NotifyTransaction(tx)) {
                ++i;
            } else {
                notifier->Shutdown();
                i = notifiers.erase(i);
                delete notifier;
            }
        }
    });
}

void CZMQNotificationInterface::TransactionDiscardedFromMempool(
    const uint256& txid, MemPoolRemovalReason reason,
    const CTransaction* conflictedWith) {
    // conflictedWith 是 raw pointer（mempool entry 内的 tx），主验证链
    // 帧返回前还活着；为 worker 线程消费安全考虑做一份 deep-copy。
    std::shared_ptr<const CTransaction> conflicted_copy;
    if (conflictedWith != nullptr) {
        conflicted_copy = std::make_shared<const CTransaction>(*conflictedWith);
    }
    m_async_worker.Enqueue([this, txid, reason, conflicted_copy] {
        const CTransaction *cw = conflicted_copy ? conflicted_copy.get() : nullptr;
        for (auto i = notifiers.begin(); i != notifiers.end();) {
            CZMQAbstractNotifier *notifier = *i;
            if (notifier->NotifyRemovedFromMempool(txid, reason, cw)) {
                ++i;
            } else {
                notifier->Shutdown();
                i = notifiers.erase(i);
                delete notifier;
            }
        }
    });
}

void CZMQNotificationInterface::TransactionRemovedFromMempoolBlock(
    const uint256& txid, MemPoolRemovalReason reason) {
    m_async_worker.Enqueue([this, txid, reason] {
        for (auto i = notifiers.begin(); i != notifiers.end();) {
            CZMQAbstractNotifier *notifier = *i;
            if (notifier->NotifyDiscardedFromMempoolBlock(txid, reason)) {
                ++i;
            } else {
                notifier->Shutdown();
                i = notifiers.erase(i);
                delete notifier;
            }
        }
    });
}

void CZMQNotificationInterface::TransactionAdded(const CTransactionRef& ptx) {
    m_async_worker.Enqueue([this, ptx] {
        const CTransaction& tx = *ptx;
        for (auto i = notifiers.begin(); i != notifiers.end();) {
            CZMQAbstractNotifier* notifier = *i;
            if (notifier->NotifyTransaction2(tx)) {
                ++i;
            } else {
                notifier->Shutdown();
                i = notifiers.erase(i);
                delete notifier;
            }
        }
    });
}

void CZMQNotificationInterface::BlockConnected(
    const std::shared_ptr<const CBlock> &pblock,
    const CBlockIndex *pindexConnected,
    const std::vector<CTransactionRef> &vtxConflicted) {
    // pblock shared_ptr capture：worker 引用计数延寿
    m_async_worker.Enqueue([this, pblock] {
        for (const CTransactionRef &ptx : pblock->vtx) {
            const CTransaction &tx = *ptx;
            for (auto i = notifiers.begin(); i != notifiers.end();) {
                CZMQAbstractNotifier *notifier = *i;
                if (notifier->NotifyTransaction(tx)) {
                    ++i;
                } else {
                    notifier->Shutdown();
                    i = notifiers.erase(i);
                    delete notifier;
                }
            }
        }
    });
}

void CZMQNotificationInterface::BlockDisconnected(
    const std::shared_ptr<const CBlock> &pblock) {
    m_async_worker.Enqueue([this, pblock] {
        for (const CTransactionRef &ptx : pblock->vtx) {
            const CTransaction &tx = *ptx;
            for (auto i = notifiers.begin(); i != notifiers.end();) {
                CZMQAbstractNotifier *notifier = *i;
                if (notifier->NotifyTransaction(tx)) {
                    ++i;
                } else {
                    notifier->Shutdown();
                    i = notifiers.erase(i);
                    delete notifier;
                }
            }
        }
    });
}

// Notify for every connected block, even on re-org
// Only notify for transactions in vtxNew (that are not already in mempool)
void CZMQNotificationInterface::BlockConnected2(
    const CBlockIndex* pindexConnected,
    const std::vector<CTransactionRef>& vtxNew) {
    if (IsInitialBlockDownload()) {
        return;
    }
    m_async_worker.Enqueue([this, pindexConnected] {
        for (auto i = notifiers.begin(); i != notifiers.end();) {
            CZMQAbstractNotifier* notifier = *i;
            if (notifier->NotifyBlock2(pindexConnected)) {
                ++i;
            } else {
                notifier->Shutdown();
                i = notifiers.erase(i);
                delete notifier;
            }
        }
    });
}
