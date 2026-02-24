#include <script/interpreter.h>
#include <script/sighashtype.h>
#include <tuple>
#include <vector>

#include <boost/test/unit_test.hpp>
#include <test/test_bitcoin.h>

#include "amount.h"
#include "key.h"
#include "primitives/transaction.h"
#include "script/script_flags.h"
#include "tbc_script_validation.h"

BOOST_FIXTURE_TEST_SUITE(tbc_checksig_tests, BasicTestingSetup)
namespace {
using TBCScriptValidation::failure;
using TBCScriptValidation::success;
using TBCScriptValidation::CheckPass;
using TBCScriptValidation::CheckPassForAllFlags;
using TBCScriptValidation::CheckError;
using TBCScriptValidation::CheckErrorForAllFlags;

// return the signed transaction, checker and the signature
template <TBCScriptValidation::SignatureMethod signatureMethod>
std::tuple<CTransactionRef, std::unique_ptr<TransactionSignatureChecker>, std::vector<uint8_t>> BuildSignedTransaction(const CScript& scriptPubKey,
    Amount amount, SigHashType sigHashType, const CKey& key) {

    CMutableTransaction txCredit = TBCScriptValidation::BuildCreditingTransaction(scriptPubKey, amount);
    CMutableTransaction txSpend = TBCScriptValidation::BuildSpendingTransaction(CScript(), txCredit);
    uint256 sighash = SignatureHash(scriptPubKey, CTransaction(txSpend), 0,
                                         sigHashType, amount, nullptr, true);
    std::vector<uint8_t> sig;

    if constexpr (signatureMethod == TBCScriptValidation::SignatureMethod::ECDSA) {
        if (!key.Sign(sighash, sig)) {
            BOOST_FAIL("Failed to generate ECDSA signature");
        }
    } else if constexpr (signatureMethod == TBCScriptValidation::SignatureMethod::SCHNORR) {
        if (!key.SignSchnorr(sighash, sig)) {
            BOOST_FAIL("Failed to generate Schnorr signature");
        }
    }

    sig.push_back(static_cast<uint8_t>(sigHashType.getRawSigHashType() & 0xff));
    txSpend.vin[0].scriptSig = CScript() << sig;
    CTransactionRef txRef = MakeTransactionRef(CTransaction(txSpend));
    return {txRef, std::make_unique<TransactionSignatureChecker>(txRef.get(), 0, amount), sig};
}

struct TestData {
    enum class OpCode {
        CHECKSIG,
        CHECKSIGVERIFY,
    };

    TBCScriptValidation::KeyMaterial keys;
    TBCScriptValidation::SignatureMaterial sigs;
    Amount amount;
    TBCScriptValidation::TransactionMaterial txs;
    TBCScriptValidation::TransactionSignatureCheckerMaterial checkers;

    explicit TestData(OpCode op) : keys(), sigs(), amount(Amount(static_cast<int64_t>(1 + InsecureRandRange(100000000000000)))) {
        SigHashType sigHashType = SigHashType().withForkId(true);
        opcodetype opcode = (op == OpCode::CHECKSIG) ? OP_CHECKSIG : OP_CHECKSIGVERIFY;

        // ECDSA
        std::tie(txs.ecdsaTx, checkers.ecdsaChecker, sigs.ecdsaSig) = BuildSignedTransaction<TBCScriptValidation::SignatureMethod::ECDSA>(
            CScript() << keys.compressedPubkey << opcode, amount, sigHashType, keys.key);

        // Schnorr
        std::tie(txs.schnorrTx, checkers.schnorrChecker, sigs.schnorrSig) = BuildSignedTransaction<TBCScriptValidation::SignatureMethod::SCHNORR>(
            CScript() << keys.xonlyPubkey << opcode, amount, sigHashType, keys.key);
    }
};


} // namespace

BOOST_AUTO_TEST_CASE(checksig_test) {
    TestData checksigData(TestData::OpCode::CHECKSIG);

    // A separate key set used for "wrong pubkey" tests (cryptographically different from checksigData.keys).
    TBCScriptValidation::KeyMaterial otherKeys;

    // ========================================================================
    // SECTION 1: ECDSA Signature Tests
    // ========================================================================

    // Test 1: ECDSA with compressed pubkey - valid signature (ForkID).
    // Only run with STANDARD_SCRIPT_VERIFY_FLAGS: with flags=0, SCRIPT_ENABLE_SIGHASH_FORKID is off,
    // so the ForkID signature would be verified against legacy sighash and fail.
    CheckPass("ECDSA: Valid compressed pubkey",
              CScript() << checksigData.sigs.ecdsaSig << OP_CODESEPARATOR << checksigData.keys.compressedPubkey << OP_CHECKSIG,
              STANDARD_SCRIPT_VERIFY_FLAGS, 1, success, *checksigData.checkers.ecdsaChecker);

    // Test 2: ECDSA - Valid ForkID sig fails without ForkID flag (flags=0).
    // The signature is correct but SCRIPT_ENABLE_SIGHASH_FORKID is not set, so sighash
    // is computed without ForkID and verification fails, leaving false on the stack.
    CheckPass("ECDSA: Valid ForkID sig fails without ForkID flag (flags=0)",
                CScript() << checksigData.sigs.ecdsaSig << OP_CODESEPARATOR << checksigData.keys.compressedPubkey << OP_CHECKSIG,
                0, 1, failure, *checksigData.checkers.ecdsaChecker);

    // Test 3: ECDSA with uncompressed pubkey - sig was over compressed scriptCode, so tx checker verification fails.
    CheckPass("ECDSA: Uncompressed pubkey (flags=0, expect false with tx checker)",
              CScript() << checksigData.sigs.ecdsaSig << OP_CODESEPARATOR << checksigData.keys.uncompressedPubkey << OP_CHECKSIG,
              0, 1, failure, *checksigData.checkers.ecdsaChecker);

    CheckError("ECDSA: Uncompressed pubkey with STANDARD_SCRIPT_VERIFY_FLAGS",
              CScript() << checksigData.sigs.ecdsaSig << OP_CODESEPARATOR << checksigData.keys.uncompressedPubkey << OP_CHECKSIG,
              STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_NULLFAIL, *checksigData.checkers.ecdsaChecker);


    // Test 4: ECDSA - tampered signature triggers NULLFAIL under STANDARD flags.
    std::vector<uint8_t> wrongEcdsaSig = checksigData.sigs.ecdsaSig;
    if (wrongEcdsaSig.size() > 2) {
        wrongEcdsaSig[wrongEcdsaSig.size() - 2] ^= 0x01;
    }
    CheckError("ECDSA: Tampered signature with NULLFAIL",
               CScript() << wrongEcdsaSig << OP_CODESEPARATOR << checksigData.keys.compressedPubkey << OP_CHECKSIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_NULLFAIL, *checksigData.checkers.ecdsaChecker);

    // Test 5: ECDSA - empty signature (OP_CHECKSIG pushes false; NULLFAIL exempts empty sig)
    std::vector<uint8_t> emptySig;
    CheckPassForAllFlags("ECDSA: Empty signature",
                         CScript() << emptySig << OP_CODESEPARATOR << checksigData.keys.compressedPubkey << OP_CHECKSIG,
                         1, failure, *checksigData.checkers.ecdsaChecker);

    // Test 6: ECDSA - invalid signature encoding
    std::vector<uint8_t> invalidEcdsaSig{0x05, 0x06, 0x07, 0x08};
    CheckPass("ECDSA: Invalid signature encoding (flags=0)",
              CScript() << invalidEcdsaSig << OP_CODESEPARATOR << checksigData.keys.compressedPubkey << OP_CHECKSIG,
              0, 1, failure, *checksigData.checkers.ecdsaChecker);

    CheckError("ECDSA: Invalid signature encoding (STANDARD)",
               CScript() << invalidEcdsaSig << OP_CODESEPARATOR << checksigData.keys.compressedPubkey << OP_CHECKSIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_DER, *checksigData.checkers.ecdsaChecker);

    // Test 7: ECDSA - Invalid DER (truncated)
    std::vector<uint8_t> invalidDerSig = ParseHex(
        "3044022057292e2d4dfe775becdd0a9e6547997c728cdf35390f6a017da56d654d374e49"
        "02206b643be2fc53763b4e284845bfea2c597d2dc7759941dce937636c9d341b71");
    invalidDerSig.pop_back();
    invalidDerSig.push_back(SIGHASH_ALL | SIGHASH_FORKID);
    CheckError("ECDSA: Invalid DER signature encoding",
               CScript() << invalidDerSig << OP_CODESEPARATOR << checksigData.keys.compressedPubkey << OP_CHECKSIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_DER, *checksigData.checkers.ecdsaChecker);

    // Test 8: ECDSA - High S value
    std::vector<uint8_t> highSSig = ParseHex(
        "304502203e4516da7253cf068effec6b95c41221c0cf3a8e6ccb8cbf1725b562e9afde2c"
        "022100ab1e3da73d67e32045a20e0b999e049978ea8d6ee5480d485fcf2ce0d03b2ef0");
    highSSig.push_back(SIGHASH_ALL | SIGHASH_FORKID);
    CheckError("ECDSA: Signature with high S value",
               CScript() << highSSig << OP_CODESEPARATOR << checksigData.keys.compressedPubkey << OP_CHECKSIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_HIGH_S, *checksigData.checkers.ecdsaChecker);

    // Test 9: ECDSA - Invalid pubkey format (32 bytes in ECDSA sig path -> legacy pubkey error).
    std::vector<uint8_t> invalidPubkey(32, 0x11);
    CheckPass("ECDSA: Invalid pubkey format (flags=0)",
              CScript() << checksigData.sigs.ecdsaSig << OP_CODESEPARATOR << invalidPubkey << OP_CHECKSIG,
              0, 1, failure, *checksigData.checkers.ecdsaChecker);

    CheckError("ECDSA: Invalid pubkey format (STANDARD)",
               CScript() << checksigData.sigs.ecdsaSig << OP_CODESEPARATOR << invalidPubkey << OP_CHECKSIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_LEGACY_PUBKEY, *checksigData.checkers.ecdsaChecker);

    // Test 10: ECDSA - Uncompressed pubkey with COMPRESSED_PUBKEYTYPE
    CheckError("ECDSA: Uncompressed pubkey with COMPRESSED_PUBKEYTYPE",
               CScript() << checksigData.sigs.ecdsaSig << OP_CODESEPARATOR << checksigData.keys.uncompressedPubkey << OP_CHECKSIG,
               STANDARD_SCRIPT_VERIFY_FLAGS | SCRIPT_VERIFY_COMPRESSED_PUBKEYTYPE, SCRIPT_ERR_NONCOMPRESSED_PUBKEY, *checksigData.checkers.ecdsaChecker);

    // Test 11: ECDSA - Hybrid pubkey
    CheckPass("ECDSA: Hybrid pubkey (flags=0)",
              CScript() << checksigData.sigs.ecdsaSig << OP_CODESEPARATOR << checksigData.keys.hybridPubkey << OP_CHECKSIG,
              0, 1, failure, *checksigData.checkers.ecdsaChecker);

    CheckError("ECDSA: Hybrid pubkey with STANDARD_SCRIPT_VERIFY_FLAGS",
               CScript() << checksigData.sigs.ecdsaSig << OP_CODESEPARATOR << checksigData.keys.hybridPubkey << OP_CHECKSIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_LEGACY_PUBKEY, *checksigData.checkers.ecdsaChecker);

    // Test 12: ECDSA - zero-length pubkey
    std::vector<uint8_t> emptyPubkey;
    CheckPass("ECDSA: Zero-length pubkey (flags=0)",
              CScript() << checksigData.sigs.ecdsaSig << OP_CODESEPARATOR << emptyPubkey << OP_CHECKSIG,
              0, 1, failure, *checksigData.checkers.ecdsaChecker);
    CheckError("ECDSA: Zero-length pubkey (STANDARD)",
               CScript() << checksigData.sigs.ecdsaSig << OP_CODESEPARATOR << emptyPubkey << OP_CHECKSIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_LEGACY_PUBKEY, *checksigData.checkers.ecdsaChecker);

    // Test 13: ECDSA - non-ForkID signature with STANDARD (must use ForkID)
    std::vector<uint8_t> nonForkIdEcdsaSig(checksigData.sigs.ecdsaSig.begin(), checksigData.sigs.ecdsaSig.end() - 1);
    nonForkIdEcdsaSig.push_back(SIGHASH_ALL);
    CheckError("ECDSA: Non-ForkID signature with STANDARD",
               CScript() << nonForkIdEcdsaSig << OP_CODESEPARATOR << checksigData.keys.compressedPubkey << OP_CHECKSIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_MUST_USE_FORKID, *checksigData.checkers.ecdsaChecker);

    // Test 14: ECDSA - undefined sig hashtype with STRICTENC
    std::vector<uint8_t> undefinedHashtypeSig(checksigData.sigs.ecdsaSig.begin(), checksigData.sigs.ecdsaSig.end() - 1);
    undefinedHashtypeSig.push_back(0x00); // undefined
    CheckError("ECDSA: Undefined sig hashtype",
               CScript() << undefinedHashtypeSig << OP_CODESEPARATOR << checksigData.keys.compressedPubkey << OP_CHECKSIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_HASHTYPE, *checksigData.checkers.ecdsaChecker);

    // Test 15: ECDSA - 64-byte signature (Schnorr length)
    std::vector<uint8_t> sixtyFourByteSig(64, 0x11);
    sixtyFourByteSig.push_back(SIGHASH_ALL | SIGHASH_FORKID);
    CheckError("ECDSA: 64-byte signature (ECDSA_SIG_SIZE)",
               CScript() << sixtyFourByteSig << OP_CODESEPARATOR << checksigData.keys.compressedPubkey << OP_CHECKSIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_XONLY_PUBKEY_SIZE, *checksigData.checkers.ecdsaChecker);

    // Test 16: ECDSA - Wrong pubkey (cryptographically different valid key).
    CheckPass("ECDSA: Wrong pubkey (different valid key, flags=0)",
              CScript() << checksigData.sigs.ecdsaSig << OP_CODESEPARATOR << otherKeys.compressedPubkey << OP_CHECKSIG,
              0, 1, failure, *checksigData.checkers.ecdsaChecker);
    CheckError("ECDSA: Wrong pubkey (different valid key, STANDARD)",
               CScript() << checksigData.sigs.ecdsaSig << OP_CODESEPARATOR << otherKeys.compressedPubkey << OP_CHECKSIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_NULLFAIL, *checksigData.checkers.ecdsaChecker);

    // ========================================================================
    // SECTION 2: Schnorr Signature Tests
    // ========================================================================

    // Test 17: Schnorr - valid signature (ForkID). OP_CODESEPARATOR so scriptCode = pubkey+OP_CHECKSIG.
    CheckPass("Schnorr: Valid signature",
              CScript() << checksigData.sigs.schnorrSig << OP_CODESEPARATOR << checksigData.keys.xonlyPubkey << OP_CHECKSIG,
              STANDARD_SCRIPT_VERIFY_FLAGS, 1, success, *checksigData.checkers.schnorrChecker);

    // Test 18: Schnorr - Valid ForkID sig fails without ForkID flag (flags=0).
    // The Schnorr sig is correct but SCRIPT_ENABLE_SIGHASH_FORKID is not set,
    // so sighash is computed without ForkID and verification fails.
    CheckPass("Schnorr: Valid ForkID sig fails without ForkID flag (flags=0)",
              CScript() << checksigData.sigs.schnorrSig << OP_CODESEPARATOR << checksigData.keys.xonlyPubkey << OP_CHECKSIG,
              0, 1, failure, *checksigData.checkers.schnorrChecker);

    // Test 19: Schnorr - tampered signature triggers NULLFAIL under STANDARD flags.
    std::vector<uint8_t> wrongSchnorrSig = checksigData.sigs.schnorrSig;
    if (wrongSchnorrSig.size() > 0) {
        wrongSchnorrSig[0] ^= 0x01;
    }
    CheckError("Schnorr: Tampered signature with NULLFAIL",
               CScript() << wrongSchnorrSig << OP_CODESEPARATOR << checksigData.keys.xonlyPubkey << OP_CHECKSIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_NULLFAIL, *checksigData.checkers.schnorrChecker);

    // Test 20: Schnorr - empty signature
    CheckPassForAllFlags("Schnorr: Empty signature",
                         CScript() << emptySig << OP_CODESEPARATOR << checksigData.keys.xonlyPubkey << OP_CHECKSIG,
                         1, failure, *checksigData.checkers.schnorrChecker);

    // Test 21: Schnorr - 63+1=64 bytes: not 65-byte Schnorr, falls through to ECDSA path -> invalid DER.
    std::vector<uint8_t> wrongLenSig(63, 0x11);
    wrongLenSig.push_back(SIGHASH_ALL | SIGHASH_FORKID);
    CheckError("Schnorr: 63+1 byte sig (invalid DER in ECDSA path)",
               CScript() << wrongLenSig << OP_CODESEPARATOR << checksigData.keys.xonlyPubkey << OP_CHECKSIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_DER, *checksigData.checkers.schnorrChecker);

    // Test 22: Schnorr - invalid pubkey size (33 bytes)
    std::vector<uint8_t> badXonlyPubkey(33, 0x11);
    CheckErrorForAllFlags("Schnorr: Invalid pubkey size (33 bytes)",
                          CScript() << checksigData.sigs.schnorrSig << OP_CODESEPARATOR << badXonlyPubkey << OP_CHECKSIG,
                          SCRIPT_ERR_XONLY_PUBKEY_SIZE, *checksigData.checkers.schnorrChecker);

    // Test 23: Schnorr - zero-length pubkey
    std::vector<uint8_t> emptyXonlyPubkey;
    CheckErrorForAllFlags("Schnorr: Zero-length pubkey",
                          CScript() << checksigData.sigs.schnorrSig << OP_CODESEPARATOR << emptyXonlyPubkey << OP_CHECKSIG,
                          SCRIPT_ERR_XONLY_PUBKEY_SIZE, *checksigData.checkers.schnorrChecker);

    // Test 24: Schnorr - non-ForkID signature (hashtype 0x01, no ForkID bit).
    std::vector<uint8_t> nonForkIdSchnorrSig(checksigData.sigs.schnorrSig.begin(), checksigData.sigs.schnorrSig.end() - 1);
    nonForkIdSchnorrSig.push_back(SIGHASH_ALL);
    CheckPass("Schnorr: Non-ForkID sig fails without strict hashtype check (flags=0)",
              CScript() << nonForkIdSchnorrSig << OP_CODESEPARATOR << checksigData.keys.xonlyPubkey << OP_CHECKSIG,
              0, 1, failure, *checksigData.checkers.schnorrChecker);
    CheckError("Schnorr: Non-ForkID sig with STANDARD (MUST_USE_FORKID)",
               CScript() << nonForkIdSchnorrSig << OP_CODESEPARATOR << checksigData.keys.xonlyPubkey << OP_CHECKSIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_MUST_USE_FORKID, *checksigData.checkers.schnorrChecker);

    // Test 25: Schnorr - undefined hashtype (0x00).
    std::vector<uint8_t> undefinedHashtypeSchnorrSig(checksigData.sigs.schnorrSig.begin(), checksigData.sigs.schnorrSig.end() - 1);
    undefinedHashtypeSchnorrSig.push_back(0x00); // undefined hashtype
    CheckPass("Schnorr: Undefined hashtype fails without strict check (flags=0)",
              CScript() << undefinedHashtypeSchnorrSig << OP_CODESEPARATOR << checksigData.keys.xonlyPubkey << OP_CHECKSIG,
              0, 1, failure, *checksigData.checkers.schnorrChecker);
    CheckError("Schnorr: Undefined hashtype with STANDARD (SIG_HASHTYPE)",
               CScript() << undefinedHashtypeSchnorrSig << OP_CODESEPARATOR << checksigData.keys.xonlyPubkey << OP_CHECKSIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_HASHTYPE, *checksigData.checkers.schnorrChecker);

    // Test 26: Schnorr - Wrong pubkey
    CheckPass("Schnorr: Wrong pubkey (different valid key, flags=0)",
              CScript() << checksigData.sigs.schnorrSig << OP_CODESEPARATOR << otherKeys.xonlyPubkey << OP_CHECKSIG,
              0, 1, failure, *checksigData.checkers.schnorrChecker);
    CheckError("Schnorr: Wrong pubkey (different valid key, STANDARD)",
               CScript() << checksigData.sigs.schnorrSig << OP_CODESEPARATOR << otherKeys.xonlyPubkey << OP_CHECKSIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_NULLFAIL, *checksigData.checkers.schnorrChecker);

    // ========================================================================
    // SECTION 3: Stack and Parameter Validation
    // ========================================================================

    // Test 27: Insufficient stack (0 elements)
    CheckErrorForAllFlags("Stack: Insufficient parameters (0 elements)",
                          CScript() << OP_CHECKSIG,
                          SCRIPT_ERR_INVALID_STACK_OPERATION, *checksigData.checkers.ecdsaChecker);

    // Test 28: Insufficient stack (1 element)
    CheckErrorForAllFlags("Stack: Insufficient parameters (1 element)",
                          CScript() << checksigData.sigs.ecdsaSig << OP_CHECKSIG,
                          SCRIPT_ERR_INVALID_STACK_OPERATION, *checksigData.checkers.ecdsaChecker);


    TestData checksigverifyData(TestData::OpCode::CHECKSIGVERIFY);

    // ========================================================================
    // SECTION 4: OP_CHECKSIGVERIFY Tests
    // ========================================================================

    // Test 29: CHECKSIGVERIFY success - ECDSA. Use sig/tx signed with scriptCode = pubkey+OP_CHECKSIGVERIFY.
    // Stack is empty on success.
    CheckPass("CHECKSIGVERIFY: ECDSA success",
              CScript() << checksigverifyData.sigs.ecdsaSig << OP_CODESEPARATOR << checksigverifyData.keys.compressedPubkey << OP_CHECKSIGVERIFY,
              STANDARD_SCRIPT_VERIFY_FLAGS, 0, {}, *checksigverifyData.checkers.ecdsaChecker);

    // Test 30: CHECKSIGVERIFY success - Schnorr. Stack is empty on success.
    CheckPass("CHECKSIGVERIFY: Schnorr success",
              CScript() << checksigverifyData.sigs.schnorrSig << OP_CODESEPARATOR << checksigverifyData.keys.xonlyPubkey << OP_CHECKSIGVERIFY,
              STANDARD_SCRIPT_VERIFY_FLAGS, 0, {}, *checksigverifyData.checkers.schnorrChecker);

    // Test 31: CHECKSIGVERIFY - empty signature for legacy pubkey
    CheckErrorForAllFlags("CHECKSIGVERIFY: Empty signature for legacy pubkey",
                          CScript() << emptySig << OP_CODESEPARATOR << checksigverifyData.keys.compressedPubkey << OP_CHECKSIGVERIFY,
                          SCRIPT_ERR_CHECKSIGVERIFY, *checksigverifyData.checkers.ecdsaChecker);

    // Test 32: CHECKSIGVERIFY - empty signature for x-only pubkey
    CheckErrorForAllFlags("CHECKSIGVERIFY: Empty signature for x-only pubkey",
                          CScript() << emptySig << OP_CODESEPARATOR << checksigverifyData.keys.xonlyPubkey << OP_CHECKSIGVERIFY,
                          SCRIPT_ERR_CHECKSIGVERIFY, *checksigverifyData.checkers.schnorrChecker);
}

BOOST_AUTO_TEST_SUITE_END()
