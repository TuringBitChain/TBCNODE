// Copyright (c) 2014-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CRYPTO_SHA256_H
#define BITCOIN_CRYPTO_SHA256_H

#include <cstdint>
#include <cstdlib>
#include <string>

/** A hasher class for SHA-256. */
class CSHA256 {
private:
    uint32_t s[8];
    uint8_t buf[64];
    uint64_t bytes;

public:
    static const size_t OUTPUT_SIZE = 32;

    CSHA256();

    /**
     * Resume SHA-256 from a compression midstate.
     *
     * This constructor is intended only for OP_PARTIAL_HASH. `partial_hash`
     * must point to at least 32 bytes containing the eight SHA-256 state words
     * in big-endian order. `hash_size` must point to an 8-byte little-endian
     * count of bytes already compressed, and that count must be a multiple of
     * the SHA-256 block size (64 bytes).
     *
     * No pending buffer is restored: at a block boundary the buffer is
     * logically empty. Calling this constructor for a non-block-aligned state
     * is invalid. The caller must also authenticate the supplied midstate when
     * required by the surrounding protocol.
     *
     * TODO: Replace this raw-pointer constructor with a
     * block-aligned-midstate API that accepts a fixed-size 32-byte state and a
     * uint64_t processed-byte count by value, and enforces these invariants
     * inside CSHA256.
     */
    CSHA256(const uint8_t *partial_hash, const uint8_t *hash_size);
    CSHA256 &Write(const uint8_t *data, size_t len);
    void Finalize(uint8_t hash[OUTPUT_SIZE]);
    CSHA256 &Reset();
};

/**
 * Autodetect the best available SHA256 implementation.
 * Returns the name of the implementation.
 */
std::string SHA256AutoDetect();

#endif // BITCOIN_CRYPTO_SHA256_H
