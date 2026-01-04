// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_X_ONLY_PUBKEY_H
#define BITCOIN_X_ONLY_PUBKEY_H

#include "uint256.h"

#include <vector>

/**
 * An encapsulated x-only public key (32 bytes) for Schnorr signatures.
 */
class XOnlyPubKey {
private:
    /**
     * Store the 32-byte x-coordinate of the public key.
     */
    uint8_t vch[32];

    //! Set this key data to be invalid
    void Invalidate() { 
        memset(vch, 0, 32); 
    }

public:
    //! Construct an invalid public key.
    XOnlyPubKey() { Invalidate(); }

    //! Initialize a public key using begin/end iterators to byte data.
    template <typename T> void Set(const T pbegin, const T pend) {
        if (pend - pbegin == 32) {
            memcpy(vch, (uint8_t *)&pbegin[0], 32);
        } else {
            Invalidate();
        }
    }

    //! Construct a public key using begin/end iterators to byte data.
    template <typename T> XOnlyPubKey(const T pbegin, const T pend) {
        Set(pbegin, pend);
    }

    //! Construct a public key from a byte vector.
    XOnlyPubKey(const std::vector<uint8_t> &_vch) {
        Set(_vch.begin(), _vch.end()); 
    }

    //! Simple read-only vector-like interface to the pubkey data.
    unsigned int size() const { return 32; }
    const uint8_t *begin() const { return vch; }
    const uint8_t *end() const { return vch + 32; }
    const uint8_t &operator[](unsigned int pos) const { return vch[pos]; }

    //! Comparator implementation.
    friend bool operator==(const XOnlyPubKey &a, const XOnlyPubKey &b) {
        return memcmp(a.vch, b.vch, 32) == 0;
    }
    friend bool operator!=(const XOnlyPubKey &a, const XOnlyPubKey &b) {
        return !(a == b);
    }
    friend bool operator<(const XOnlyPubKey &a, const XOnlyPubKey &b) {
        return memcmp(a.vch, b.vch, 32) < 0;
    }

    /*
     * Check syntactic correctness (32 bytes, non-zero).
     *
     * Note that this is consensus critical as CheckSig() calls it!
     */
    bool IsValid() const;

    //! fully validate whether this is a valid x-only public key
    bool IsFullyValid() const;

    /**
     * Verify a Schnorr signature (64 bytes).
     * If this public key is not fully valid, the return value will be false.
     */
    bool VerifySchnorr(const uint256 &hash, 
                      const std::vector<uint8_t> &vchSig) const;
};

/**
 * A handle for the ECC Schnorr verification context.
 * RAII for the ECC Schnorr verification context.
 */
class ECCSchnorrVerifyHandle {
private:
    static int refcount;
public:
    ECCSchnorrVerifyHandle();
    ~ECCSchnorrVerifyHandle();
};

#endif // BITCOIN_X_ONLY_PUBKEY_H

