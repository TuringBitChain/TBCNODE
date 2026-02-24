#include <script/interpreter.h>
#include <vector>

#include <boost/test/unit_test.hpp>
#include <test/test_bitcoin.h>

#include "tbc_script_validation.h"

BOOST_FIXTURE_TEST_SUITE(tbc_checkdatasig_tests, BasicTestingSetup)

namespace {
using TBCScriptValidation::failure;
using TBCScriptValidation::success;
using TBCScriptValidation::CheckPass;
using TBCScriptValidation::CheckPassForAllFlags;
using TBCScriptValidation::CheckError;
using TBCScriptValidation::CheckErrorForAllFlags;

// Test data for CHECKDATASIG: keys + message/hashes + ECDSA/Schnorr sigs.
struct TestData {
    // message
    std::vector<uint8_t> message;
    std::vector<uint8_t> messageSingleSha256;

    TBCScriptValidation::KeyMaterial keys;
    TBCScriptValidation::SignatureMaterial sigs;

    TestData() : message(InsecureRandBytes(32)) {
        uint256 msgSingleHash;
        CHash256().Write(message.data(), message.size()).SingleFinalize(msgSingleHash.begin());
        messageSingleSha256.assign(msgSingleHash.begin(), msgSingleHash.end());

        uint256 msgDoubleHash;
        CHash256().Write(message.data(), message.size()).Finalize(msgDoubleHash.begin());

        if (!keys.key.Sign(msgDoubleHash, sigs.ecdsaSig)) {
            BOOST_FAIL("Failed to generate ECDSA signature (double sha256)");
        }
        if (!keys.key.SignSchnorr(msgDoubleHash, sigs.schnorrSig)) {
            BOOST_FAIL("Failed to generate Schnorr signature (double sha256)");
        }
    }
};
} // namespace

BOOST_AUTO_TEST_CASE(checkdatasig_test) {
    TestData data;
    BaseSignatureChecker checker;

    // Flag format: [dataConversionMethod, sigFunc] where:
    //   dataConversionMethod: 0x01=SINGLE_SHA256, 0x02=DOUBLE_SHA256
    //   sigFunc: 0x00=NONE, 0x01=ECDSA, 0x02=SCHNORR
    
    // ========================================================================
    // SECTION 1: ECDSA Signature Tests
    // ========================================================================

    // Test 1: ECDSA with compressed pubkey - valid signature (double sha256 method)
    CheckPassForAllFlags("ECDSA: Valid compressed pubkey (double sha256 method)",
                         CScript() << data.sigs.ecdsaSig << data.message << data.keys.compressedPubkey
                                   << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
                         1, success, checker);

    // Test 2: ECDSA with uncompressed pubkey - valid signature (double sha256 method)
    CheckPassForAllFlags("ECDSA: Valid uncompressed pubkey (double sha256 method)",
                         CScript() << data.sigs.ecdsaSig << data.message << data.keys.uncompressedPubkey
                                   << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
                         1, success, checker);

    // Test 3: ECDSA with hybrid pubkey - valid signature (double sha256 method)
    CheckPass("ECDSA: Valid hybrid pubkey (double sha256 method)",
              CScript() << data.sigs.ecdsaSig << data.message << data.keys.hybridPubkey
                        << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
              0, 1, success, checker);

    CheckError("ECDSA: Valid hybrid pubkey (double sha256 method)",
               CScript() << data.sigs.ecdsaSig << data.message << data.keys.hybridPubkey
                         << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_LEGACY_PUBKEY, checker);

    // Test 4: ECDSA with compressed pubkey - valid signature (single sha256 method)
    CheckPassForAllFlags("ECDSA: Valid compressed pubkey (single sha256 method)",
                         CScript() << data.sigs.ecdsaSig << data.messageSingleSha256 << data.keys.compressedPubkey
                                   << std::vector<uint8_t>{0x01, 0x01} << OP_CHECKDATASIG,
                         1, success, checker);

    // Test 5: ECDSA with uncompressed pubkey - valid signature (single sha256 method)
    CheckPassForAllFlags("ECDSA: Valid uncompressed pubkey (single sha256 method)",
                         CScript() << data.sigs.ecdsaSig << data.messageSingleSha256 << data.keys.uncompressedPubkey
                                   << std::vector<uint8_t>{0x01, 0x01} << OP_CHECKDATASIG,
                         1, success, checker);

    // Test 6: ECDSA with hybrid pubkey - valid signature (single sha256 method)
    CheckPass("ECDSA: Valid hybrid pubkey (single sha256 method)",
              CScript() << data.sigs.ecdsaSig << data.messageSingleSha256 << data.keys.hybridPubkey
                        << std::vector<uint8_t>{0x01, 0x01} << OP_CHECKDATASIG,
              0, 1, success, checker);

    CheckError("ECDSA: Valid hybrid pubkey (single sha256 method)",
               CScript() << data.sigs.ecdsaSig << data.messageSingleSha256 << data.keys.hybridPubkey
                         << std::vector<uint8_t>{0x01, 0x01} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_LEGACY_PUBKEY, checker);

    // Test 7: ECDSA signature with mismatched message
    std::vector<uint8_t> mismatchedMessage = {0x01, 0x02};
    CheckPass("ECDSA: Mismatched message",
              CScript() << data.sigs.ecdsaSig << mismatchedMessage << data.keys.compressedPubkey
                        << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
              0, 1, failure, checker);

    CheckError("ECDSA: Mismatched message",
               CScript() << data.sigs.ecdsaSig << mismatchedMessage << data.keys.compressedPubkey
                         << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_NULLFAIL, checker);

    // Test 8: ECDSA signature with invalid signature
    std::vector<uint8_t> invalidEcdsaSig{0x05, 0x06, 0x07, 0x08};
    CheckPass("ECDSA: Invalid signature",
              CScript() << invalidEcdsaSig << data.message << data.keys.compressedPubkey
                        << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
              0, 1, failure, checker);

    CheckError("ECDSA: Invalid signature",
               CScript() << invalidEcdsaSig << data.message << data.keys.compressedPubkey
                         << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_DER, checker);

    // Test 9: ECDSA - Invalid DER signature encoding
    // Use a signature with incorrect length field (length says 0x44 but actual length is shorter)
    std::vector<uint8_t> invalidDerSig = ParseHex(
        "3044022057292e2d4dfe775becdd0a9e6547997c728cdf35390f6a017da56d654d374e49"
        "02206b643be2fc53763b4e284845bfea2c597d2dc7759941dce937636c9d341b71");
    invalidDerSig.pop_back();
    CheckError("ECDSA: Invalid DER signature encoding",
               CScript() << invalidDerSig << data.message << data.keys.compressedPubkey
                         << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_DER, checker);

    // Test 10: ECDSA - Signature with high S value
    // DER signature with high S: 0x304502...022100... (S value > order/2)
    std::vector<uint8_t> highSSig = ParseHex(
        "304502203e4516da7253cf068effec6b95c41221c0cf3a8e6ccb8cbf1725b562e9afde2c"
        "022100ab1e3da73d67e32045a20e0b999e049978ea8d6ee5480d485fcf2ce0d03b2ef0");
    CheckError("ECDSA: Signature with high S value",
               CScript() << highSSig << data.message << data.keys.compressedPubkey
                         << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_HIGH_S, checker);

    // Test 11: ECDSA - Invalid pubkey
    std::vector<uint8_t> invalidEcdsaPubkey(32, 0x11);
    CheckPass("ECDSA: Invalid pubkey",
              CScript() << data.sigs.ecdsaSig << data.message << invalidEcdsaPubkey
                        << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
              0, 1, failure, checker);

    CheckError("ECDSA: Invalid pubkey",
               CScript() << data.sigs.ecdsaSig << data.message << invalidEcdsaPubkey
                         << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_LEGACY_PUBKEY, checker);

    // Test 12: ECDSA - Uncompressed pubkey with COMPRESSED_PUBKEYTYPE flags
    CheckError("ECDSA: Uncompressed pubkey rejected with COMPRESSED_PUBKEYTYPE flags",
               CScript() << data.sigs.ecdsaSig << data.message << data.keys.uncompressedPubkey
                         << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS | SCRIPT_VERIFY_COMPRESSED_PUBKEYTYPE, SCRIPT_ERR_NONCOMPRESSED_PUBKEY, checker);

    // Test 13: ECDSA - Hybrid pubkey with COMPRESSED_PUBKEYTYPE flags
    CheckError("ECDSA: Hybrid pubkey rejected with COMPRESSED_PUBKEYTYPE flags",
               CScript() << data.sigs.ecdsaSig << data.message << data.keys.hybridPubkey
                         << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS | SCRIPT_VERIFY_COMPRESSED_PUBKEYTYPE, SCRIPT_ERR_LEGACY_PUBKEY, checker);

    // ========================================================================
    // SECTION 2: Schnorr Signature Tests
    // ========================================================================

    // Test 14: Valid Schnorr signature verification (double sha256 method)
    CheckPassForAllFlags("Schnorr: Valid signature (double sha256 method)",
                         CScript() << data.sigs.schnorrSig << data.message << data.keys.xonlyPubkey
                                   << std::vector<uint8_t>{0x02, 0x02} << OP_CHECKDATASIG,
                         1, success, checker);

    // Test 15: Valid Schnorr signature verification (single sha256 method)
    CheckPassForAllFlags("Schnorr: Valid signature (single sha256 method)",
                         CScript() << data.sigs.schnorrSig << data.messageSingleSha256 << data.keys.xonlyPubkey
                                   << std::vector<uint8_t>{0x01, 0x02} << OP_CHECKDATASIG,
                         1, success, checker);

    // Test 16: Schnorr - Mismatched message
    CheckPass("Schnorr: Mismatched message",
              CScript() << data.sigs.schnorrSig << mismatchedMessage << data.keys.xonlyPubkey
                        << std::vector<uint8_t>{0x02, 0x02} << OP_CHECKDATASIG,
              0, 1, failure, checker);

    CheckError("Schnorr: Mismatched message",
               CScript() << data.sigs.schnorrSig << mismatchedMessage << data.keys.xonlyPubkey
                         << std::vector<uint8_t>{0x02, 0x02} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_NULLFAIL, checker);

    // Test 17: Schnorr - Invalid signature
    std::vector<uint8_t> invalidSchnorrSig(64, 0xff);
    CheckPass("Schnorr: Invalid signature bytes",
              CScript() << invalidSchnorrSig << data.message << data.keys.xonlyPubkey
                        << std::vector<uint8_t>{0x02, 0x02} << OP_CHECKDATASIG,
              0, 1, failure, checker);

    CheckError("Schnorr: Invalid signature bytes",
               CScript() << invalidSchnorrSig << data.message << data.keys.xonlyPubkey
                         << std::vector<uint8_t>{0x02, 0x02} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_NULLFAIL, checker);

    // Test 18: Schnorr - Invalid signature length
    std::vector<uint8_t> shortSchnorrSig(32, 0x11);
    CheckErrorForAllFlags("Schnorr: Invalid signature length",
                          CScript() << shortSchnorrSig << data.message << data.keys.xonlyPubkey
                                    << std::vector<uint8_t>{0x02, 0x02} << OP_CHECKDATASIG,
                          SCRIPT_ERR_SCHNORR_SIG_SIZE, checker);

    // Test 19: Schnorr - Invalid pubkey size
    std::vector<uint8_t> badSchnorrPubkey(33, 0x11);
    CheckErrorForAllFlags("Schnorr: Invalid pubkey size",
                          CScript() << data.sigs.schnorrSig << data.message << badSchnorrPubkey
                                    << std::vector<uint8_t>{0x02, 0x02} << OP_CHECKDATASIG,
                          SCRIPT_ERR_XONLY_PUBKEY_SIZE, checker);

    // ========================================================================
    // SECTION 3: None Signature Method Tests
    // ========================================================================

    std::vector<uint8_t> emptySig;
    // Test 20: NONE method with empty signature - compressed pubkey
    CheckPassForAllFlags("NONE: Empty signature with compressed pubkey",
                         CScript() << emptySig << data.message << data.keys.compressedPubkey
                                   << std::vector<uint8_t>{0x02, 0x00} << OP_CHECKDATASIG,
                         1, failure, checker);

    // Test 21: NONE method with empty signature - uncompressed pubkey
    CheckPassForAllFlags("NONE: Empty signature with uncompressed pubkey",
                         CScript() << emptySig << data.message << data.keys.uncompressedPubkey
                                   << std::vector<uint8_t>{0x02, 0x00} << OP_CHECKDATASIG,
                         1, failure, checker);

    // Test 22: NONE method with empty signature - x-only pubkey
    CheckPassForAllFlags("NONE: Empty signature with x-only pubkey",
                         CScript() << emptySig << data.message << data.keys.xonlyPubkey
                                   << std::vector<uint8_t>{0x02, 0x00} << OP_CHECKDATASIG,
                         1, failure, checker);

    // Test 23: NONE method with non-empty signature
    CheckErrorForAllFlags("NONE: Non-empty signature should fail",
                          CScript() << data.sigs.ecdsaSig << data.message << data.keys.compressedPubkey
                                    << std::vector<uint8_t>{0x02, 0x00} << OP_CHECKDATASIG,
                          SCRIPT_ERR_EMPTY_SIG_SIZE, checker);

    // Test 24: NONE method with hybrid pubkey
    CheckPass("NONE: Empty signature with hybrid pubkey",
              CScript() << emptySig << data.message << data.keys.hybridPubkey
                        << std::vector<uint8_t>{0x02, 0x00} << OP_CHECKDATASIG,
              0, 1, failure, checker);

    CheckError("NONE: Empty signature with hybrid pubkey",
               CScript() << emptySig << data.message << data.keys.hybridPubkey
                         << std::vector<uint8_t>{0x02, 0x00} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_PUBKEY_NOT_XONLY_OR_LEGACY, checker);

    // Test 25: NONE method with uncompressed pubkey and COMPRESSED_PUBKEYTYPE flags
    CheckError("NONE: Uncompressed pubkey rejected with COMPRESSED_PUBKEYTYPE flags",
               CScript() << emptySig << data.message << data.keys.uncompressedPubkey
                         << std::vector<uint8_t>{0x02, 0x00} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS | SCRIPT_VERIFY_COMPRESSED_PUBKEYTYPE, SCRIPT_ERR_PUBKEY_NOT_XONLY_OR_COMPRESSED, checker);

    // ========================================================================
    // SECTION 4: Flag Validation Tests
    // ========================================================================

    // Test 26: Invalid message type flag
    CheckErrorForAllFlags("Flag: Invalid message type 0x00",
                          CScript() << data.sigs.schnorrSig << data.message << data.keys.xonlyPubkey
                                    << std::vector<uint8_t>{0x00, 0x02} << OP_CHECKDATASIG,
                          SCRIPT_ERR_CHECKDATASIG_FLAG, checker);

    // Test 27: Invalid sigFunc
    CheckErrorForAllFlags("Flag: Invalid sigFunc 0x03",
                          CScript() << data.sigs.schnorrSig << data.message << data.keys.xonlyPubkey
                                    << std::vector<uint8_t>{0x01, 0x03} << OP_CHECKDATASIG,
                          SCRIPT_ERR_CHECKDATASIG_FLAG, checker);

    // Test 28: Flag length too short
    CheckErrorForAllFlags("Flag: Length too short",
                          CScript() << data.sigs.schnorrSig << data.message << data.keys.xonlyPubkey
                                    << 0x01 << OP_CHECKDATASIG,
                          SCRIPT_ERR_CHECKDATASIG_FLAG, checker);

    // Test 29: Flag length too long
    CheckErrorForAllFlags("Flag: Length too long",
                          CScript() << data.sigs.schnorrSig << data.message << data.keys.xonlyPubkey
                                    << std::vector<uint8_t>{0x01, 0x02, 0x03} << OP_CHECKDATASIG,
                          SCRIPT_ERR_CHECKDATASIG_FLAG, checker);

    // ========================================================================
    // SECTION 5: Data Conversion Method MisMatch Tests
    // ========================================================================

    // Test 30: Flag is Double SHA256 (0x02) but message passed is already SHA256 hashed
    CheckPass("Data Conversion Method Mismatch: Double SHA256 flag but SHA256 hashed message passed",
              CScript() << data.sigs.schnorrSig << data.messageSingleSha256 << data.keys.xonlyPubkey
                        << std::vector<uint8_t>{0x02, 0x02} << OP_CHECKDATASIG,
              0, 1, failure, checker);

    CheckError("Data Conversion Method Mismatch: Double SHA256 flag but SHA256 hashed message passed",
               CScript() << data.sigs.schnorrSig << data.messageSingleSha256 << data.keys.xonlyPubkey
                         << std::vector<uint8_t>{0x02, 0x02} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_NULLFAIL, checker);

    // Test 31: Flag is Single SHA256 (0x01) but raw message passed
    CheckPass("Data Conversion Method Mismatch: Single SHA256 flag but raw message passed",
              CScript() << data.sigs.schnorrSig << data.message << data.keys.xonlyPubkey
                        << std::vector<uint8_t>{0x01, 0x02} << OP_CHECKDATASIG,
              0, 1, failure, checker);

    CheckError("Data Conversion Method Mismatch: Single SHA256 flag but raw message passed",
               CScript() << data.sigs.schnorrSig << data.message << data.keys.xonlyPubkey
                         << std::vector<uint8_t>{0x01, 0x02} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_NULLFAIL, checker);

    // ========================================================================
    // SECTION 6: Sig Function MisMatch Tests
    // ========================================================================

    // Test 32: ECDSA with Schnorr flag
    CheckErrorForAllFlags("Sig Function Mismatch: ECDSA with Schnorr flag",
                          CScript() << data.sigs.ecdsaSig << data.message << data.keys.compressedPubkey
                                    << std::vector<uint8_t>{0x02, 0x02} << OP_CHECKDATASIG,
                          SCRIPT_ERR_SCHNORR_SIG_SIZE, checker);

    // Test 33: Schnorr with ECDSA flag (64-byte sig rejected as ECDSA encoding)
    CheckErrorForAllFlags("Sig Function Mismatch: Schnorr with ECDSA flag",
                          CScript() << data.sigs.schnorrSig << data.message << data.keys.xonlyPubkey
                                    << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
                          SCRIPT_ERR_ECDSA_SIG_SIZE, checker);

    // ========================================================================
    // SECTION 7: Signature Mismatch Tests
    // ========================================================================

    // Test 34: Use Schnorr signature with ECDSA (64-byte sig rejected as ECDSA encoding)
    CheckErrorForAllFlags("Signature Mismatch: Schnorr sig with ECDSA",
                          CScript() << data.sigs.schnorrSig << data.message << data.keys.compressedPubkey
                                    << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
                          SCRIPT_ERR_ECDSA_SIG_SIZE, checker);

    // Test 35: Use ECDSA signature with Schnorr
    CheckErrorForAllFlags("Signature Mismatch: ECDSA sig with Schnorr",
                          CScript() << data.sigs.ecdsaSig << data.message << data.keys.xonlyPubkey
                                    << std::vector<uint8_t>{0x02, 0x02} << OP_CHECKDATASIG,
                          SCRIPT_ERR_SCHNORR_SIG_SIZE, checker);

    // ========================================================================
    // SECTION 8: Pubkey Mismatch Tests
    // ========================================================================

    // Test 36: Use x-only pubkey with ECDSA signature
    CheckError("Pubkey Mismatch: x-only pubkey with ECDSA",
            CScript() << data.sigs.ecdsaSig << data.message << data.keys.xonlyPubkey
                      << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
            STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_LEGACY_PUBKEY, checker);

    // Test 37: Use ECDSA compressed key with Schnorr signature
    CheckError("Pubkey Mismatch: ECDSA pubkey with Schnorr",
            CScript() << data.sigs.schnorrSig << data.message << data.keys.compressedPubkey
                      << std::vector<uint8_t>{0x02, 0x02} << OP_CHECKDATASIG,
            STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_XONLY_PUBKEY_SIZE, checker);

    // ========================================================================
    // SECTION 10: Stack and Parameter Validation Tests
    // ========================================================================

    // Test 38: Insufficient stack parameters (empty stack)
    CheckErrorForAllFlags("Stack: Insufficient parameters (0 elements)",
                          CScript() << OP_CHECKDATASIG,
                          SCRIPT_ERR_INVALID_STACK_OPERATION, checker);

    // Test 39: Insufficient stack parameters (1 element)
    CheckErrorForAllFlags("Stack: Insufficient parameters (1 element)",
                          CScript() << data.sigs.schnorrSig << OP_CHECKDATASIG,
                          SCRIPT_ERR_INVALID_STACK_OPERATION, checker);

    // Test 40: Insufficient stack parameters (2 elements)
    CheckErrorForAllFlags("Stack: Insufficient parameters (2 elements)",
                          CScript() << data.sigs.schnorrSig << data.message << OP_CHECKDATASIG,
                          SCRIPT_ERR_INVALID_STACK_OPERATION, checker);

    // Test 41: Insufficient stack parameters (3 elements)
    CheckErrorForAllFlags("Stack: Insufficient parameters (3 elements)",
                          CScript() << data.sigs.schnorrSig << data.message << data.keys.xonlyPubkey << OP_CHECKDATASIG,
                          SCRIPT_ERR_INVALID_STACK_OPERATION, checker);

    // ========================================================================
    // SECTION 11: OP_CHECKDATASIGVERIFY Tests
    // ========================================================================

    // Test 42: CHECKDATASIGVERIFY success - ECDSA (stack should be empty)
    CheckPassForAllFlags("CHECKDATASIGVERIFY: ECDSA success",
                         CScript() << data.sigs.ecdsaSig << data.message << data.keys.compressedPubkey
                                   << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIGVERIFY,
                         0, {}, checker);

    // Test 43: CHECKDATASIGVERIFY success - Schnorr (stack should be empty)
    CheckPassForAllFlags("CHECKDATASIGVERIFY: Schnorr success",
                         CScript() << data.sigs.schnorrSig << data.message << data.keys.xonlyPubkey
                                   << std::vector<uint8_t>{0x02, 0x02} << OP_CHECKDATASIGVERIFY,
                         0, {}, checker);

    // Test 44: CHECKDATASIGVERIFY failure - NONE method
    CheckErrorForAllFlags("CHECKDATASIGVERIFY: NONE method fails",
                          CScript() << emptySig << data.message << data.keys.compressedPubkey
                                    << std::vector<uint8_t>{0x02, 0x00} << OP_CHECKDATASIGVERIFY,
                          SCRIPT_ERR_CHECKDATASIGVERIFY, checker);
}

BOOST_AUTO_TEST_SUITE_END()
