// Copyright (c) 2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CRYPTO_CHACHA20_H
#define BITCOIN_CRYPTO_CHACHA20_H

#include <cstdint>
#include <cstdlib>

#include "span.h"

/** Type for 96-bit nonces used by the Set function below.
 *
 * The first field corresponds to the LE32-encoded first 4 bytes of the nonce, also referred
 * to as the '32-bit fixed-common part' in Example 2.8.2 of RFC8439.
 *
 * The second field corresponds to the LE64-encoded last 8 bytes of the nonce.
 *
 */
using Nonce96 = std::pair<uint32_t, uint64_t>;

/** A PRNG class for ChaCha20. */
class ChaCha20 
{
public:
    /** Expected key length in constructor and SetKey. */
    static constexpr unsigned KEYLEN{32};

    /** Block size (inputs/outputs to Keystream / Crypt should be multiples of this). */
    static constexpr unsigned BLOCKLEN{64};

    ChaCha20();
    ChaCha20(const uint8_t *key, size_t keylen);
    /** Initialize a cipher with specified 32-byte key. */
    ChaCha20(bsv::span<const std::byte> key) noexcept;
    void SetKey(const uint8_t *key, size_t keylen);

    /** Set 32-byte key, and seek to nonce 0 and block position 0. */
    void SetKey(bsv::span<const std::byte> key) noexcept;
    
    void SetIV(uint64_t iv);
    void Seek(uint64_t pos);

    /** Set the 96-bit nonce and 32-bit block counter.
     *
     * Block_counter selects a position to seek to (to byte BLOCKLEN*block_counter). After 256 GiB,
     * the block counter overflows, and nonce.first is incremented.
     */
    void Seek(Nonce96 nonce, uint32_t block_counter) noexcept;

    void Output(uint8_t *output_, size_t bytes);

    /** outputs the keystream to output. */
    void Keystream(bsv::span<std::byte> output) noexcept;
    
    /** en/deciphers the message <input_> and write the result into <output_>
     *
     * The size of input_ and output_ must be equal, and be a multiple of BLOCKLEN.
     */
    void Crypt(bsv::span<const std::byte> input_, bsv::span<std::byte> output_) noexcept;

private:
    uint32_t input[16];
};

class ChaCha20Unrestricted 
{
public:
    /** Expected key length in constructor and SetKey. */
    static constexpr unsigned KEYLEN = ChaCha20::KEYLEN;

    /** For safety, disallow initialization without key. */
    ChaCha20Unrestricted() noexcept = delete;

    /** Initialize a cipher with specified 32-byte key. */
    ChaCha20Unrestricted(bsv::span<const std::byte> key) noexcept : m_aligned(key) {}

    /** Destructor to clean up private memory. */
    ~ChaCha20Unrestricted();

    /** Set 32-byte key, and seek to nonce 0 and block position 0. */
    void SetKey(bsv::span<const std::byte> key) noexcept;

    /** Set the 96-bit nonce and 32-bit block counter. See ChaCha20Unrestricted::Seek. */
    void Seek(Nonce96 nonce, uint32_t block_counter) noexcept
    {
        m_aligned.Seek(nonce, block_counter);
        m_bufleft = 0;
    }

    /** en/deciphers the message <input> and write the result into <output>
     *
     * The size of input and output must be equal.
     */
    void Crypt(bsv::span<const std::byte> input, bsv::span<std::byte> output) noexcept;

    /** outputs the keystream to out. */
    void Keystream(bsv::span<std::byte> out) noexcept;

private:
    ChaCha20 m_aligned;
    std::array<std::byte, ChaCha20::BLOCKLEN> m_buffer;
    unsigned m_bufleft{0};

};

/** Forward-secure ChaCha20
 *
 * This implements a stream cipher that automatically transitions to a new stream with a new key
 * and new nonce after a predefined number of encryptions or decryptions.
 *
 * See BIP324 for details.
 */
class FSChaCha20
{
private:
    /** Internal stream cipher. */
    ChaCha20Unrestricted m_chacha20;

    /** The number of encryptions/decryptions before a rekey happens. */
    const uint32_t m_rekey_interval;

    /** The number of encryptions/decryptions since the last rekey. */
    uint32_t m_chunk_counter{0};

    /** The number of rekey operations that have happened. */
    uint64_t m_rekey_counter{0};

public:
    /** Length of keys expected by the constructor. */
    static constexpr unsigned KEYLEN = 32;

    // No copy or move to protect the secret.
    FSChaCha20(const FSChaCha20&) = delete;
    FSChaCha20(FSChaCha20&&) = delete;
    FSChaCha20& operator=(const FSChaCha20&) = delete;
    FSChaCha20& operator=(FSChaCha20&&) = delete;

    /** Construct an FSChaCha20 cipher that rekeys every rekey_interval Crypt() calls. */
    FSChaCha20(bsv::span<const std::byte> key, uint32_t rekey_interval) noexcept;

    /** Encrypt or decrypt a chunk. */
    void Crypt(bsv::span<const std::byte> input, bsv::span<std::byte> output) noexcept;
};

#endif // BITCOIN_CRYPTO_CHACHA20_H
