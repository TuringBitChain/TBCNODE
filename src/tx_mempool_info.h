// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2019 Bitcoin Association
// Distributed under the Open TBC software license, see the accompanying file LICENSE.

#pragma once

#include <amount.h>
#include <primitives/transaction.h>

class CTxMemPoolEntry;

/**
 * Information about a mempool transaction.
 */
struct TxMempoolInfo
{
    explicit TxMempoolInfo() = default;
    explicit TxMempoolInfo(const CTxMemPoolEntry& entry);
    TxMempoolInfo(const CTransactionRef& ptx) : tx{ptx} {}

    /** The transaction itself */
    CTransactionRef tx {nullptr};

    /** Time the transaction entered the mempool. */
    int64_t nTime {0};

    /** Feerate of the transaction. */
    CFeeRate feeRate {};

    /** The fee delta. */
    Amount nFeeDelta {};

    /** size of the serialized transaction */
    size_t nTxSize {};
};

