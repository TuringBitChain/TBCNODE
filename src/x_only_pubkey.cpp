// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "x_only_pubkey.h"

#include "with_schnorr_secp256k1/schnorr_wrap.h"

bool XOnlyPubKey::IsValid() const {
    // Check that it's not all zeros
    bool all_zero = true;
    for (int i = 0; i < 32; i++) {
        if (vch[i] != 0) {
            all_zero = false;
            break;
        }
    }
    return !all_zero;
}

bool XOnlyPubKey::IsFullyValid() const {
    if (!IsValid()) return false;

    return tbc_xonly_pubkey_parse(vch) == 1;
}

bool XOnlyPubKey::VerifySchnorr(const uint256 &hash,
                                const std::vector<uint8_t> &vchSig) const {
    if (!IsValid()) return false;
    if (vchSig.size() != 64) return false;
    
    return tbc_schnorr_verify_bip340(hash.begin(), vchSig.data(), vch) == 1;
}

/* static */ int ECCSchnorrVerifyHandle::refcount = 0;

ECCSchnorrVerifyHandle::ECCSchnorrVerifyHandle() {
    if (refcount == 0) {
        tbc_schnorr_ctx_ref();
    }
    refcount++;
}

ECCSchnorrVerifyHandle::~ECCSchnorrVerifyHandle() {
    refcount--;
    if (refcount == 0) {
        tbc_schnorr_ctx_unref();
    }
}