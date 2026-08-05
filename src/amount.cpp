// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2019 Bitcoin Association
// Distributed under the Open TBC software license, see the accompanying file LICENSE.

#include "amount.h"

#include "tinyformat.h"

#include <limits>

const std::string CURRENCY_UNIT = "TBC";

namespace {

int64_t SaturatingInt64(__int128 value) {
    if (value > std::numeric_limits<int64_t>::max()) {
        return std::numeric_limits<int64_t>::max();
    }
    if (value < std::numeric_limits<int64_t>::min()) {
        return std::numeric_limits<int64_t>::min();
    }
    return static_cast<int64_t>(value);
}

} // namespace

std::string Amount::ToString() const {
    return strprintf("%d.%06d %s", amount / TBCCOIN.GetSatoshis(),
                     amount % TBCCOIN.GetSatoshis(), CURRENCY_UNIT);
}

CFeeRate::CFeeRate(const Amount nFeePaid, size_t nBytes_) {
    assert(nBytes_ <= uint64_t(std::numeric_limits<int64_t>::max()));
    int64_t nSize = int64_t(nBytes_);

    if (nSize > 0) {
        const __int128 rate =
            static_cast<__int128>(nFeePaid.GetSatoshis()) * 1000 / nSize;
        nSatoshisPerK = Amount(SaturatingInt64(rate));
    } else {
        nSatoshisPerK = Amount(0);
    }
}

Amount CFeeRate::GetFee(size_t nBytes_) const {
    assert(nBytes_ <= uint64_t(std::numeric_limits<int64_t>::max()));
    int64_t nSize = int64_t(nBytes_);

    if (nSize < 1000) {
        nSize = 1000;
    }

    const __int128 fee = static_cast<__int128>(nSize) *
                         nSatoshisPerK.GetSatoshis() / 1000;
    Amount nFee(SaturatingInt64(fee));

    if (nFee == Amount(0) && nSize != 0) {
        if (nSatoshisPerK > Amount(0)) {
            nFee = Amount(1);
        }
        if (nSatoshisPerK < Amount(0)) {
            nFee = Amount(-1);
        }
    }

    return nFee;
}

std::string CFeeRate::ToString() const {
    return strprintf(
        "%d.%06d %s/kB", nSatoshisPerK.GetSatoshis() / TBCCOIN.GetSatoshis(),
        nSatoshisPerK.GetSatoshis() % TBCCOIN.GetSatoshis(), CURRENCY_UNIT);
}
