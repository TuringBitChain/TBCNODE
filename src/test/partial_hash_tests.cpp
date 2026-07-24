// Copyright (c) 2024 The Open TBC developers
// Distributed under the Open TBC software license, see the accompanying file LICENSE.
//
// Unit tests for the OP_PARTIAL_HASH opcode (SHA-256 midstate resumption).
//
// OP_PARTIAL_HASH consumes three stack items (top to bottom):
//   stacktop(-1) vchSize     : total message length, little-endian bytes
//   stacktop(-2) vchPartHash : 32-byte SHA-256 midstate, or empty for a fresh hash
//   stacktop(-3) vch         : the remaining (not-yet-hashed) message bytes
// and pushes the resulting 32-byte SHA-256 digest.
//
// The known-answer vectors below are generated independently (see
// test/data generation in the commit message) and verified to round-trip:
//   D0          = bytes 0x00..0x3f                       (exactly one 64-byte block)
//   D1          = 0xAA * 32                               (remaining data)
//   MIDSTATE    = SHA-256 internal state after compressing D0
//   FULL96      = SHA-256(D0 || D1)
//   SHA_D0      = SHA-256(D0)

#include "test/test_bitcoin.h"

#include "crypto/common.h"
#include "crypto/sha256.h"
#include "config.h"
#include "script/interpreter.h"
#include "script/limitedstack.h"
#include "script/script.h"
#include "taskcancellation.h"

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstring>
#include <new>
#include <type_traits>
#include <vector>

typedef std::vector<uint8_t> valtype;
typedef std::vector<valtype> stacktype;

namespace {

const valtype D0 = [] {
    valtype v(64);
    for (size_t i = 0; i < 64; ++i) v[i] = static_cast<uint8_t>(i);
    return v;
}();

const valtype D1(32, 0xAA);

const valtype MIDSTATE = {
    0xfc, 0x99, 0xa2, 0xdf, 0x88, 0xf4, 0x2a, 0x7a, 0x7b, 0xb9, 0xd1,
    0x80, 0x33, 0xcd, 0xc6, 0xa2, 0x02, 0x56, 0x75, 0x5f, 0x9d, 0x5b,
    0x9a, 0x50, 0x44, 0xa9, 0xcc, 0x31, 0x5a, 0xbe, 0x84, 0xa7};

const valtype FULL96 = {
    0x3e, 0xca, 0xba, 0xe1, 0x57, 0x63, 0x97, 0xf8, 0x50, 0xa2, 0x14,
    0xef, 0x85, 0xa8, 0x37, 0xd0, 0xab, 0xba, 0x93, 0x5f, 0xc2, 0x8c,
    0x73, 0x66, 0x97, 0x6a, 0x36, 0x7b, 0x5b, 0x84, 0x9c, 0x90};

const valtype SHA_D0 = {
    0xfd, 0xea, 0xb9, 0xac, 0xf3, 0x71, 0x03, 0x62, 0xbd, 0x26, 0x58,
    0xcd, 0xc9, 0xa2, 0x9e, 0x8f, 0x9c, 0x75, 0x7f, 0xcf, 0x98, 0x11,
    0x60, 0x3a, 0x8c, 0x44, 0x7c, 0xd1, 0xd9, 0x15, 0x11, 0x08};

// Encode `value` as `nbytes` little-endian bytes. nbytes may exceed 8 to
// exercise the oversized-size-field path.
valtype EncodeSizeLE(uint64_t value, size_t nbytes) {
    valtype v(nbytes, 0x00);
    for (size_t i = 0; i < nbytes && i < 8; ++i) {
        v[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xff);
    }
    return v;
}

// Run a single OP_PARTIAL_HASH against the given initial stack across the
// standard flag sets. Returns whether evaluation succeeded; on success the
// resulting stack top is returned via `out`.
bool RunPartialHash(const stacktype &initial, ScriptError &err, valtype *out = nullptr) {
    const Config &config = GlobalConfig::GetConfig();
    BaseSignatureChecker sigchecker;
    auto source = task::CCancellationSource::Make();

    const uint32_t flags[] = {0, STANDARD_SCRIPT_VERIFY_FLAGS,
                              MANDATORY_SCRIPT_VERIFY_FLAGS};
    bool firstResult = false;
    for (size_t fi = 0; fi < 3; ++fi) {
        err = SCRIPT_ERR_OK;
        LimitedStack stack(initial, UINT32_MAX);
        auto r = EvalScript(config, true, source->GetToken(), stack,
                            CScript() << OP_PARTIAL_HASH, flags[fi], sigchecker,
                            &err);
        bool ok = r.value();
        if (fi == 0) {
            firstResult = ok;
            if (ok && out && stack.size() > 0) {
                *out = stack.stacktop(-1).GetElement();
            }
        } else {
            // OP_PARTIAL_HASH is not gated by any flag: every flag set must
            // agree on accept/reject.
            BOOST_CHECK_EQUAL(ok, firstResult);
        }
    }
    return firstResult;
}

// Recreate the exact CSHA256 resumption path used by OP_PARTIAL_HASH, but do it
// in storage we prefill with a chosen byte pattern. Because the resumption
// constructor never initialises buf[64], any case with hash_size % 64 != 0 will
// consume these prefilled bytes as if they were buffered prefix data.
valtype ResumePartialHashFromPrefilledStorage(const valtype &midstate,
                                             uint64_t hashSize,
                                             const valtype &suffix,
                                             uint8_t fillByte) {
    static_assert(std::is_trivially_destructible<CSHA256>::value,
                  "CSHA256 lifetime handling in this test assumes trivial destruction");

    alignas(CSHA256) std::array<unsigned char, sizeof(CSHA256)> storage;
    std::memset(storage.data(), fillByte, storage.size());

    std::array<uint8_t, 8> hashSizeLe{};
    WriteLE64(hashSizeLe.data(), hashSize);

    CSHA256 *hasher =
        new (storage.data()) CSHA256(midstate.data(), hashSizeLe.data());

    valtype out(CSHA256::OUTPUT_SIZE);
    hasher->Write(suffix.data(), suffix.size()).Finalize(out.data());
    hasher->~CSHA256();
    return out;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(partial_hash_tests, BasicTestingSetup)

// Empty midstate => a fresh, complete SHA-256 of the supplied data.
BOOST_AUTO_TEST_CASE(full_hash_empty_midstate) {
    ScriptError err;
    valtype out;
    stacktype stack = {D0, valtype{}, EncodeSizeLE(D0.size(), 1)};
    BOOST_CHECK(RunPartialHash(stack, err, &out));
    BOOST_CHECK(out == SHA_D0);
}

// Valid 32-byte midstate resumed at a 64-byte boundary => SHA-256(D0 || D1).
BOOST_AUTO_TEST_CASE(partial_hash_aligned_midstate) {
    ScriptError err;
    valtype out;
    // partHashSize = 96 - 32 = 64 (block aligned), the legitimate use.
    stacktype stack = {D1, MIDSTATE, EncodeSizeLE(96, 1)};
    BOOST_CHECK(RunPartialHash(stack, err, &out));
    BOOST_CHECK(out == FULL96);
}

// With a block-aligned resume point, buf[64] is irrelevant: two different
// prefilled storage patterns must still produce the same digest.
BOOST_AUTO_TEST_CASE(aligned_resume_ignores_prefilled_buffer_bytes) {
    const valtype hashA =
        ResumePartialHashFromPrefilledStorage(MIDSTATE, 64, D1, 0x00);
    const valtype hashB =
        ResumePartialHashFromPrefilledStorage(MIDSTATE, 64, D1, 0xff);

    BOOST_CHECK(hashA == FULL96);
    BOOST_CHECK(hashB == FULL96);
    BOOST_CHECK(hashA == hashB);
}

// This is the root cause behind the OP_PARTIAL_HASH issue: once hashSize % 64
// is non-zero, resumption consumes whatever bytes happen to already live in the
// storage slot that becomes buf[64]. The same logical inputs produce different
// hashes when that hidden storage differs. OP_PARTIAL_HASH rejects this shape;
// this low-level test documents why the constructor must remain block-aligned.
BOOST_AUTO_TEST_CASE(unaligned_resume_depends_on_prefilled_buffer_bytes) {
    // suffix = 32 bytes, declared total = 65 bytes => partHashSize = 33.
    const uint64_t declaredTotalSize = 65;

    const valtype hashA = ResumePartialHashFromPrefilledStorage(
        MIDSTATE, declaredTotalSize - D1.size(), D1, 0x11);
    const valtype hashB = ResumePartialHashFromPrefilledStorage(
        MIDSTATE, declaredTotalSize - D1.size(), D1, 0x77);

    BOOST_CHECK(hashA != hashB);
}


// Fewer than three stack items must fail.
BOOST_AUTO_TEST_CASE(too_few_stack_items) {
    ScriptError err;
    stacktype stack = {D0, valtype{}};
    BOOST_CHECK(!RunPartialHash(stack, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_INVALID_STACK_OPERATION);
}

// Empty midstate but declared total size != remaining length must fail.
BOOST_AUTO_TEST_CASE(empty_midstate_size_mismatch) {
    ScriptError err;
    stacktype stack = {D0, valtype{}, EncodeSizeLE(D0.size() + 1, 1)};
    BOOST_CHECK(!RunPartialHash(stack, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_INVALID_STACK_OPERATION);
}

// A midstate that is neither empty nor exactly 32 bytes must fail.
BOOST_AUTO_TEST_CASE(bad_midstate_length) {
    ScriptError err;
    stacktype stack = {D1, valtype(16, 0x11), EncodeSizeLE(96, 1)};
    BOOST_CHECK(!RunPartialHash(stack, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_INVALID_STACK_OPERATION);
}

// 32-byte midstate but declared total size < remaining length must fail.
BOOST_AUTO_TEST_CASE(midstate_size_too_small) {
    ScriptError err;
    // remaining = 32, declared total = 16 < 32.
    stacktype stack = {D1, MIDSTATE, EncodeSizeLE(16, 1)};
    BOOST_CHECK(!RunPartialHash(stack, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_INVALID_STACK_OPERATION);
}

// --- OP_PARTIAL_HASH security invariants ---
//
// These cases ensure the opcode rejects inputs that cannot safely satisfy the
// block-aligned midstate constructor contract.

// partHashSize not a multiple of 64: the midstate cannot be resumed safely
// (Write() would consume uninitialised buffer bytes). Must be rejected.
BOOST_AUTO_TEST_CASE(unaligned_parthashsize_rejected) {
    ScriptError err;
    // remaining = 32, declared total = 64 => partHashSize = 32 (not % 64 == 0).
    stacktype stack = {D1, MIDSTATE, EncodeSizeLE(64, 1)};
    BOOST_CHECK(!RunPartialHash(stack, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_INVALID_STACK_OPERATION);
}

// Size field longer than 8 bytes: assembling a uint64_t shifts by >= 64 bits
// (undefined behaviour). Must be rejected before that happens.
BOOST_AUTO_TEST_CASE(oversized_size_field_rejected) {
    ScriptError err;
    // 9-byte little-endian encoding of 96.
    stacktype stack = {D1, MIDSTATE, EncodeSizeLE(96, 9)};
    BOOST_CHECK(!RunPartialHash(stack, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_INVALID_STACK_OPERATION);
}

BOOST_AUTO_TEST_SUITE_END()
