// Copyright (c) 2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "zmqabstractnotifier.h"
#include "util.h"

CZMQAbstractNotifier::~CZMQAbstractNotifier() {
    assert(!psocket);
}

bool CZMQAbstractNotifier::NotifyBlock(const CBlockIndex * /*CBlockIndex*/) {
    return true;
}

bool CZMQAbstractNotifier::NotifyBlock2(const CBlockIndex*)
{
    return true;
}

bool CZMQAbstractNotifier::NotifyTransaction2(const CTransaction&)
{
    return true;
}

bool CZMQAbstractNotifier::NotifyTransaction(
    const CTransaction & /*transaction*/) {
    return true;
}

bool CZMQAbstractNotifier::NotifyRemovedFromMempool(const uint256& txid, MemPoolRemovalReason reason,
                                                    const CTransaction* conflictedWith)
{
    return true;
}

bool CZMQAbstractNotifier::NotifyRemovedFromMempoolBlock(const uint256& txid, MemPoolRemovalReason reason)
{
    return true;
}
