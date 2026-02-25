#include <algorithm>
#include <numeric>
#include <script/interpreter.h>
#include <tuple>
#include <script/sighashtype.h>
#include <vector>

#include <boost/test/unit_test.hpp>
#include <test/test_bitcoin.h>

#include "amount.h"
#include "key.h"
#include "primitives/transaction.h"
#include "script/script_error.h"
#include "script/script_flags.h"
#include "tbc_script_validation.h"
#include "utilstrencodings.h"

BOOST_FIXTURE_TEST_SUITE(tbc_checkmultisig_tests, BasicTestingSetup)
namespace {
using TBCScriptValidation::failure;
using TBCScriptValidation::success;
using TBCScriptValidation::CheckPass;
using TBCScriptValidation::CheckPassForAllFlags;
using TBCScriptValidation::CheckError;
using TBCScriptValidation::CheckErrorForAllFlags;

template <TBCScriptValidation::SignatureMethod signatureMethod>
void SignMultisigInput(
    const CScript& scriptPubKey,
    const CMutableTransaction& txSpendTemplate,
    Amount amount,
    SigHashType sigHashType,
    const std::vector<TBCScriptValidation::KeyMaterial>& keys,
    const std::vector<size_t>& sigToKey,
    std::vector<TBCScriptValidation::SignatureMaterial>& sigs) {
    uint8_t hashTypeByte = static_cast<uint8_t>(sigHashType.getRawSigHashType() & 0xff);
    for (size_t i = 0; i < sigs.size(); ++i) {
        size_t keyIdx = sigToKey[i];
        uint256 sighash = SignatureHash(scriptPubKey, CTransaction(txSpendTemplate), 0,
                                        sigHashType, amount, nullptr, true);
        if constexpr (signatureMethod == TBCScriptValidation::SignatureMethod::ECDSA) {
            if (!keys[keyIdx].key.Sign(sighash, sigs[i].ecdsaSig)) {
                BOOST_FAIL("Failed to generate ECDSA signature for multisig");
            }
            sigs[i].ecdsaSig.push_back(hashTypeByte);
        } else if constexpr (signatureMethod == TBCScriptValidation::SignatureMethod::SCHNORR) {
            if (!keys[keyIdx].key.SignSchnorr(sighash, sigs[i].schnorrSig)) {
                BOOST_FAIL("Failed to generate Schnorr signature for multisig");
            }
            sigs[i].schnorrSig.push_back(hashTypeByte);
        }
    }
}

template <TBCScriptValidation::SignatureMethod signatureMethod>
CScript BuildMultisigScriptPubKey(
    size_t numSigs,
    size_t numKeys,
    const std::vector<TBCScriptValidation::KeyMaterial>& keys,
    opcodetype opcode) {
    CScript script;
    script << static_cast<int64_t>(numSigs);
    for (const auto& km : keys) {
        if constexpr (signatureMethod == TBCScriptValidation::SignatureMethod::ECDSA) {
            script << km.compressedPubkey;
        } else {
            script << km.xonlyPubkey;
        }
    }
    script << static_cast<int64_t>(numKeys) << opcode;
    return script;
}

template <TBCScriptValidation::SignatureMethod signatureMethod>
std::pair<CTransactionRef, std::unique_ptr<TransactionSignatureChecker>>
BuildSignedMultisigTransaction(
    size_t numSigs,
    size_t numKeys,
    const std::vector<TBCScriptValidation::KeyMaterial>& keys,
    const std::vector<size_t>& sigToKey,
    std::vector<TBCScriptValidation::SignatureMaterial>& sigs,
    Amount amount,
    SigHashType sigHashType,
    opcodetype opcode) {
    CScript scriptPubKey =
        BuildMultisigScriptPubKey<signatureMethod>(numSigs, numKeys, keys, opcode);
    CMutableTransaction txCredit =
        TBCScriptValidation::BuildCreditingTransaction(scriptPubKey, amount);
    CMutableTransaction txSpend =
        TBCScriptValidation::BuildSpendingTransaction(CScript() << OP_0, txCredit);

    SignMultisigInput<signatureMethod>(scriptPubKey, txSpend, amount, sigHashType, keys,
                                      sigToKey, sigs);

    CScript scriptSig;
    scriptSig << OP_0;
    for (const auto& s : sigs) {
        if constexpr (signatureMethod == TBCScriptValidation::SignatureMethod::ECDSA) {
            scriptSig << s.ecdsaSig;
        } else {
            scriptSig << s.schnorrSig;
        }
    }
    txSpend.vin[0].scriptSig = scriptSig;
    CTransactionRef tx = MakeTransactionRef(CTransaction(txSpend));
    auto checker = std::make_unique<TransactionSignatureChecker>(tx.get(), 0, amount);
    return {tx, std::move(checker)};
}

struct TestData {
    enum class OpCode {
        CHECKMULTISIG,
        CHECKMULTISIGVERIFY,
    };

    std::vector<TBCScriptValidation::KeyMaterial> keys;
    std::vector<TBCScriptValidation::SignatureMaterial> sigs;
    std::vector<size_t> sigToKey;
    Amount amount;
    TBCScriptValidation::TransactionMaterial txs;
    TBCScriptValidation::TransactionSignatureCheckerMaterial checkers;

    // Build the Schnorr CHECKMULTISIG dummy bitfield from sigToKey.
    // Each bit i (little-endian) is set iff key i has a corresponding signature.
    // Byte count = ceil(numKeys / 8); padding bits in the last byte must be 0.
    std::vector<uint8_t> BuildSchnorrDummy(size_t numKeys) const {
        const size_t byteCount = (numKeys + 7) / 8;
        std::vector<uint8_t> dummy(byteCount, 0);
        for (size_t keyIdx : sigToKey) {
            dummy[keyIdx / 8] |= static_cast<uint8_t>(1u << (keyIdx % 8));
        }
        return dummy;
    }

    TestData(size_t numKeys, size_t numSigs, OpCode op = OpCode::CHECKMULTISIG)
        : amount(Amount(static_cast<int64_t>(1 + InsecureRandRange(100000000000000)))) {
        BOOST_REQUIRE(numSigs <= numKeys);

        keys.resize(numKeys);
        sigs.resize(numSigs);

        // Randomly choose numSigs distinct key indices; CHECKMULTISIG requires
        // signatures to appear in the same order as the public keys, so we sort.
        std::vector<size_t> indices(numKeys);
        std::iota(indices.begin(), indices.end(), 0);
        std::random_device rd;
        std::mt19937 g(rd());
        std::shuffle(indices.begin(), indices.end(), g);
        sigToKey.assign(indices.begin(), indices.begin() + numSigs);
        std::sort(sigToKey.begin(), sigToKey.end());

        SigHashType sigHashType = SigHashType().withForkId(true);
        opcodetype opcode =
            (op == OpCode::CHECKMULTISIG) ? OP_CHECKMULTISIG : OP_CHECKMULTISIGVERIFY;

        std::tie(txs.ecdsaTx, checkers.ecdsaChecker) =
            BuildSignedMultisigTransaction<TBCScriptValidation::SignatureMethod::ECDSA>(
                numSigs, numKeys, keys, sigToKey, sigs, amount, sigHashType, opcode);

        std::tie(txs.schnorrTx, checkers.schnorrChecker) =
            BuildSignedMultisigTransaction<TBCScriptValidation::SignatureMethod::SCHNORR>(
                numSigs, numKeys, keys, sigToKey, sigs, amount, sigHashType, opcode);
    }
};

void PushDummy(CScript& script, const std::vector<uint8_t>& dummy) {
    if (dummy.empty()) {
        script << OP_0;
        return;
    }

    if (dummy.size() == 1) {
        const uint8_t b = dummy[0];
        if (b >= 1 && b <= 16) {
            script << static_cast<opcodetype>(OP_1 + b - 1);
            return;
        }
        if (b == 0x81) {
            script << OP_1NEGATE;
            return;
        }
    }

    script << dummy;
}

template <TBCScriptValidation::SignatureMethod signatureMethod>
CScript BuildScript(TestData::OpCode op,
                    const std::vector<TBCScriptValidation::KeyMaterial>& keys,
                    const std::vector<TBCScriptValidation::SignatureMaterial>& sigs,
                    const std::vector<uint8_t>& dummy) {
    CScript script;
    PushDummy(script, dummy);

    for (const auto& sig : sigs) {
        if constexpr (signatureMethod == TBCScriptValidation::SignatureMethod::ECDSA) {
            script << sig.ecdsaSig;
        } else if constexpr (signatureMethod == TBCScriptValidation::SignatureMethod::SCHNORR) {
            script << sig.schnorrSig;
        }
    }

    script << OP_CODESEPARATOR;
    script << static_cast<int64_t>(sigs.size());
    for (const auto& km : keys) {
        if constexpr (signatureMethod == TBCScriptValidation::SignatureMethod::ECDSA) {
            script << km.compressedPubkey;
        } else if constexpr (signatureMethod == TBCScriptValidation::SignatureMethod::SCHNORR) {
            script << km.xonlyPubkey;
        }
    }

    const opcodetype opcode =
        (op == TestData::OpCode::CHECKMULTISIG) ? OP_CHECKMULTISIG : OP_CHECKMULTISIGVERIFY;
    script << static_cast<int64_t>(keys.size()) << opcode;
    return script;
}

template <TBCScriptValidation::SignatureMethod signatureMethod>
CScript BuildCheckmultisigScript(const std::vector<TBCScriptValidation::KeyMaterial>& keys,
                            const std::vector<TBCScriptValidation::SignatureMaterial>& sigs,
                            const std::vector<uint8_t>& dummy) {
    return BuildScript<signatureMethod>(TestData::OpCode::CHECKMULTISIG, keys, sigs, dummy);
}

template <TBCScriptValidation::SignatureMethod signatureMethod>
CScript BuildCheckmultisigverifyScript(const std::vector<TBCScriptValidation::KeyMaterial>& keys,
                            const std::vector<TBCScriptValidation::SignatureMaterial>& sigs,
                            const std::vector<uint8_t>& dummy) {
    return BuildScript<signatureMethod>(TestData::OpCode::CHECKMULTISIGVERIFY, keys, sigs, dummy);
}

} // namespace

BOOST_AUTO_TEST_CASE(checkmultisig_test) {
    TestData multisigData(5, 3, TestData::OpCode::CHECKMULTISIG);

    // ========================================================================
    // SECTION 1: ECDSA CHECKMULTISIG
    // ========================================================================
    std::vector<uint8_t> emptyDummy;
    std::vector<uint8_t> validSchnorrDummy = multisigData.BuildSchnorrDummy(multisigData.keys.size());

    // Test 1: valid ECDSA multisig 
    CheckPass("ECDSA CHECKMULTISIG: Valid signatures",
              BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                  multisigData.keys, multisigData.sigs, emptyDummy),
              STANDARD_SCRIPT_VERIFY_FLAGS, 1, success, *multisigData.checkers.ecdsaChecker);

    // Test 2: valid ForkID signatures with flags=0 should fail (checked against legacy sighash).
    CheckPass("ECDSA CHECKMULTISIG: Valid ForkID signatures fail without ForkID flag",
              BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                  multisigData.keys, multisigData.sigs, emptyDummy),
              0, 1, failure, *multisigData.checkers.ecdsaChecker);

    // Test 3: tampered signature
    std::vector<TBCScriptValidation::SignatureMaterial> tamperedEcdsaSigs = multisigData.sigs;
    if (!tamperedEcdsaSigs.empty() && tamperedEcdsaSigs.back().ecdsaSig.size() > 2) {
        tamperedEcdsaSigs.back().ecdsaSig[tamperedEcdsaSigs.back().ecdsaSig.size() - 2] ^= 0x01;
    }
    CheckPass("ECDSA CHECKMULTISIG: Tampered signature pushes false (flags=0)",
             BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                 multisigData.keys, tamperedEcdsaSigs, emptyDummy),
             0, 1, failure, *multisigData.checkers.ecdsaChecker);
    CheckError("ECDSA CHECKMULTISIG: Tampered signature with STANDARD (NULLFAIL)",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                   multisigData.keys, tamperedEcdsaSigs, emptyDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_NULLFAIL, *multisigData.checkers.ecdsaChecker);

    // Test 4: invalid ECDSA encoding.
    std::vector<TBCScriptValidation::SignatureMaterial> invalidDerEcdsa = multisigData.sigs;
    invalidDerEcdsa.back().ecdsaSig = {0x05, 0x06, 0x07, 0x08};
    CheckPass("ECDSA CHECKMULTISIG: Invalid DER (flags=0)",
              BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                  multisigData.keys, invalidDerEcdsa, emptyDummy),
              0, 1, failure, *multisigData.checkers.ecdsaChecker);
    CheckError("ECDSA CHECKMULTISIG: Invalid DER (STANDARD)",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                   multisigData.keys, invalidDerEcdsa, emptyDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_DER, *multisigData.checkers.ecdsaChecker);

    // Test 5: non-ForkID hashtype in STANDARD.
    std::vector<TBCScriptValidation::SignatureMaterial> nonForkIdEcdsa = multisigData.sigs;
    for (auto& sig : nonForkIdEcdsa) {
        BOOST_REQUIRE(!sig.ecdsaSig.empty());
        sig.ecdsaSig.back() = SIGHASH_ALL;
    }
    CheckError("ECDSA CHECKMULTISIG: Non-ForkID signature with STANDARD",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                  multisigData.keys, nonForkIdEcdsa, emptyDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_MUST_USE_FORKID, *multisigData.checkers.ecdsaChecker);

    // Test 6: undefined hashtype in STANDARD.
    std::vector<TBCScriptValidation::SignatureMaterial> undefinedHashTypeEcdsa = multisigData.sigs;
    for (auto& sig : undefinedHashTypeEcdsa) {
        BOOST_REQUIRE(!sig.ecdsaSig.empty());
        sig.ecdsaSig.back() = 0x00;
    }
    CheckError("ECDSA CHECKMULTISIG: Undefined hashtype with STANDARD",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                  multisigData.keys, undefinedHashTypeEcdsa, emptyDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_HASHTYPE, *multisigData.checkers.ecdsaChecker);

    // Test 7: ECDSA - Invalid DER (truncated)
    std::vector<TBCScriptValidation::SignatureMaterial> truncatedDerEcdsa = multisigData.sigs;
    std::vector<uint8_t> invalidDerTruncated = ParseHex(
        "3044022057292e2d4dfe775becdd0a9e6547997c728cdf35390f6a017da56d654d374e49"
        "02206b643be2fc53763b4e284845bfea2c597d2dc7759941dce937636c9d341b71");
    invalidDerTruncated.pop_back();
    invalidDerTruncated.push_back(SIGHASH_ALL | SIGHASH_FORKID);
    truncatedDerEcdsa.back().ecdsaSig = invalidDerTruncated;
    CheckError("ECDSA CHECKMULTISIG: Invalid DER (truncated)",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                   multisigData.keys, truncatedDerEcdsa, emptyDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_DER, *multisigData.checkers.ecdsaChecker);

    // Test 8: ECDSA - High S value, aligned with checksig.
    std::vector<TBCScriptValidation::SignatureMaterial> highSEcdsa = multisigData.sigs;
    std::vector<uint8_t> highSSig = ParseHex(
        "304502203e4516da7253cf068effec6b95c41221c0cf3a8e6ccb8cbf1725b562e9afde2c"
        "022100ab1e3da73d67e32045a20e0b999e049978ea8d6ee5480d485fcf2ce0d03b2ef0");
    highSSig.push_back(SIGHASH_ALL | SIGHASH_FORKID);
    highSEcdsa.back().ecdsaSig = highSSig;
    CheckError("ECDSA CHECKMULTISIG: Signature with high S value",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                   multisigData.keys, highSEcdsa, emptyDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_HIGH_S, *multisigData.checkers.ecdsaChecker);

    // Test 9: wrong pubkey (different key material)
    std::vector<TBCScriptValidation::KeyMaterial> wrongEcdsaKeys = multisigData.keys;
    TBCScriptValidation::KeyMaterial otherKeys;
    wrongEcdsaKeys[multisigData.sigToKey.back()] = otherKeys;
    CheckPass("ECDSA CHECKMULTISIG: Wrong pubkey (flags=0)",
              BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                  wrongEcdsaKeys, multisigData.sigs, emptyDummy),
              0, 1, failure, *multisigData.checkers.ecdsaChecker);
    CheckError("ECDSA CHECKMULTISIG: Wrong pubkey with STANDARD (NULLFAIL)",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                   wrongEcdsaKeys, multisigData.sigs, emptyDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_NULLFAIL, *multisigData.checkers.ecdsaChecker);

    // Test 10: mixed empty/non-empty signatures with NULLFAIL.
    std::vector<TBCScriptValidation::SignatureMaterial> mixedEmptyEcdsa = multisigData.sigs;
    mixedEmptyEcdsa.back().ecdsaSig.clear();
    CheckError("ECDSA CHECKMULTISIG: Mixed empty/non-empty signatures",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                   multisigData.keys, mixedEmptyEcdsa, emptyDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_NULLFAIL, *multisigData.checkers.ecdsaChecker);
    CheckPass("ECDSA CHECKMULTISIG: Mixed empty/non-empty signatures (flags=0)",
              BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                  multisigData.keys, mixedEmptyEcdsa, emptyDummy),
              0, 1, failure, *multisigData.checkers.ecdsaChecker);

    // Test 11: all signatures empty
    std::vector<TBCScriptValidation::SignatureMaterial> allEmptyEcdsa = multisigData.sigs;
    for (auto& sig : allEmptyEcdsa) {
        sig.ecdsaSig.clear();
    }
    CheckPassForAllFlags("ECDSA CHECKMULTISIG: All signatures empty",
                         BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                  multisigData.keys, allEmptyEcdsa, emptyDummy),
                         1, failure, *multisigData.checkers.ecdsaChecker);

    // Test 12: non-null dummy
    CheckPass("ECDSA CHECKMULTISIG: Non-null dummy accepted with flags=0",
              BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                  multisigData.keys, multisigData.sigs, validSchnorrDummy),
              0, 1, failure, *multisigData.checkers.ecdsaChecker);
    CheckError("ECDSA CHECKMULTISIG: Non-null dummy with STANDARD (Schnorr path, bit count mismatch)",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                   multisigData.keys, multisigData.sigs, validSchnorrDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SCHNORR_SIG_SIZE, *multisigData.checkers.ecdsaChecker);

    // Test 13: ECDSA - Invalid pubkey format (32 bytes)
    std::vector<TBCScriptValidation::KeyMaterial> invalidPubkeyEcdsaKeys = multisigData.keys;
    invalidPubkeyEcdsaKeys.back().compressedPubkey = std::vector<uint8_t>(32, 0x11);
    CheckPass("ECDSA CHECKMULTISIG: Invalid pubkey format (flags=0)",
              BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                  invalidPubkeyEcdsaKeys, multisigData.sigs, emptyDummy),
              0, 1, failure, *multisigData.checkers.ecdsaChecker);
    CheckError("ECDSA CHECKMULTISIG: Invalid pubkey format (STANDARD)",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                   invalidPubkeyEcdsaKeys, multisigData.sigs, emptyDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_LEGACY_PUBKEY, *multisigData.checkers.ecdsaChecker);

    // Test 14: ECDSA - Uncompressed pubkey (sig was for compressed scriptCode).
    std::vector<TBCScriptValidation::KeyMaterial> uncompressedEcdsaKeys = multisigData.keys;
    uncompressedEcdsaKeys.back().compressedPubkey = multisigData.keys.back().uncompressedPubkey;
    CheckPass("ECDSA CHECKMULTISIG: Uncompressed pubkey (flags=0)",
              BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                  uncompressedEcdsaKeys, multisigData.sigs, emptyDummy),
              0, 1, failure, *multisigData.checkers.ecdsaChecker);
    CheckError("ECDSA CHECKMULTISIG: Uncompressed pubkey with STANDARD",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                  uncompressedEcdsaKeys, multisigData.sigs, emptyDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_NULLFAIL, *multisigData.checkers.ecdsaChecker);

    // Test 15: ECDSA - Uncompressed pubkey with COMPRESSED_PUBKEYTYPE (may hit NULLFAIL before encoding).
    CheckError("ECDSA CHECKMULTISIG: Uncompressed pubkey with COMPRESSED_PUBKEYTYPE",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                   uncompressedEcdsaKeys, multisigData.sigs, emptyDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS | SCRIPT_VERIFY_COMPRESSED_PUBKEYTYPE, SCRIPT_ERR_NONCOMPRESSED_PUBKEY, *multisigData.checkers.ecdsaChecker);

    // Test 16: ECDSA - Hybrid pubkey (STANDARD rejects).
    std::vector<TBCScriptValidation::KeyMaterial> hybridEcdsaKeys = multisigData.keys;
    hybridEcdsaKeys.back().compressedPubkey = multisigData.keys.back().hybridPubkey;
    CheckPass("ECDSA CHECKMULTISIG: Hybrid pubkey (flags=0)",
              BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                  hybridEcdsaKeys, multisigData.sigs, emptyDummy),
              0, 1, failure, *multisigData.checkers.ecdsaChecker);
    CheckError("ECDSA CHECKMULTISIG: Hybrid pubkey with STANDARD",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                  hybridEcdsaKeys, multisigData.sigs, emptyDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_LEGACY_PUBKEY, *multisigData.checkers.ecdsaChecker);

    // Test 17: ECDSA - Zero-length pubkey.
    std::vector<TBCScriptValidation::KeyMaterial> emptyPubkeyEcdsaKeys = multisigData.keys;
    emptyPubkeyEcdsaKeys.back().compressedPubkey.clear();
    CheckPass("ECDSA CHECKMULTISIG: Zero-length pubkey (flags=0)",
              BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                  emptyPubkeyEcdsaKeys, multisigData.sigs, emptyDummy),
              0, 1, failure, *multisigData.checkers.ecdsaChecker);
    CheckError("ECDSA CHECKMULTISIG: Zero-length pubkey (STANDARD)",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                   emptyPubkeyEcdsaKeys, multisigData.sigs, emptyDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_LEGACY_PUBKEY, *multisigData.checkers.ecdsaChecker);

    // Test 18: ECDSA - 64-byte signature (Schnorr length) in ECDSA path.
    std::vector<TBCScriptValidation::SignatureMaterial> sixtyFourByteEcdsa = multisigData.sigs;
    sixtyFourByteEcdsa.back().ecdsaSig.assign(64, 0x11);
    sixtyFourByteEcdsa.back().ecdsaSig.push_back(SIGHASH_ALL | SIGHASH_FORKID);
    CheckError("ECDSA CHECKMULTISIG: 64-byte signature (ECDSA_SIG_SIZE)",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                   multisigData.keys, sixtyFourByteEcdsa, emptyDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_ECDSA_SIG_SIZE, *multisigData.checkers.ecdsaChecker);

    // ========================================================================
    // SECTION 2: Schnorr CHECKMULTISIG
    // ========================================================================

    // Test 19: valid Schnorr multisig.
    CheckPass("Schnorr CHECKMULTISIG: Valid signatures and bitfield",
              BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::SCHNORR>(
                  multisigData.keys, multisigData.sigs, validSchnorrDummy),
              STANDARD_SCRIPT_VERIFY_FLAGS, 1, success, *multisigData.checkers.schnorrChecker);

    // Test 20: tampered Schnorr signature.
    std::vector<TBCScriptValidation::SignatureMaterial> tamperedSchnorr = multisigData.sigs;
    if (!tamperedSchnorr.empty() && !tamperedSchnorr.back().schnorrSig.empty()) {
        tamperedSchnorr.back().schnorrSig[0] ^= 0x01;
    }
    CheckError("Schnorr CHECKMULTISIG: Tampered signature with STANDARD (NULLFAIL)",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::SCHNORR>(
                   multisigData.keys, tamperedSchnorr, validSchnorrDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_NULLFAIL, *multisigData.checkers.schnorrChecker);

    // Test 21: invalid Schnorr signature length.
    std::vector<TBCScriptValidation::SignatureMaterial> shortSchnorr = multisigData.sigs;
    shortSchnorr.back().schnorrSig = std::vector<uint8_t>(33, 0x11);
    CheckError("Schnorr CHECKMULTISIG: Invalid signature size (STANDARD)",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::SCHNORR>(
                   multisigData.keys, shortSchnorr, validSchnorrDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SCHNORR_SIG_SIZE, *multisigData.checkers.schnorrChecker);

    // Test 22: invalid x-only pubkey size.
    std::vector<TBCScriptValidation::KeyMaterial> badSchnorrKeys = multisigData.keys;
    badSchnorrKeys[multisigData.sigToKey.back()].xonlyPubkey = std::vector<uint8_t>(33, 0x11);
    CheckError("Schnorr CHECKMULTISIG: Invalid pubkey size (STANDARD)",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::SCHNORR>(
                   badSchnorrKeys, multisigData.sigs, validSchnorrDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_XONLY_PUBKEY_SIZE, *multisigData.checkers.schnorrChecker);

    // Test 23: non-ForkID hashtype in STANDARD.
    std::vector<TBCScriptValidation::SignatureMaterial> nonForkIdSchnorr = multisigData.sigs;
    for (auto& sig : nonForkIdSchnorr) {
        BOOST_REQUIRE(!sig.schnorrSig.empty());
        sig.schnorrSig.back() = SIGHASH_ALL;
    }
    CheckError("Schnorr CHECKMULTISIG: Non-ForkID signature with STANDARD",
BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::SCHNORR>(
                  multisigData.keys, nonForkIdSchnorr, validSchnorrDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_MUST_USE_FORKID, *multisigData.checkers.schnorrChecker);

    // Test 24: undefined hashtype in STANDARD.
    std::vector<TBCScriptValidation::SignatureMaterial> undefinedHashTypeSchnorr = multisigData.sigs;
    for (auto& sig : undefinedHashTypeSchnorr) {
        BOOST_REQUIRE(!sig.schnorrSig.empty());
        sig.schnorrSig.back() = 0x00;
    }
    CheckError("Schnorr CHECKMULTISIG: Undefined hashtype with STANDARD",
BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::SCHNORR>(
                  multisigData.keys, undefinedHashTypeSchnorr, validSchnorrDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_HASHTYPE, *multisigData.checkers.schnorrChecker);

    // Test 25: empty dummy means ECDSA branch, Schnorr signature length is rejected there.
    CheckErrorForAllFlags("Schnorr CHECKMULTISIG: Empty dummy routes to ECDSA path",
BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::SCHNORR>(
                  multisigData.keys, multisigData.sigs, emptyDummy),
                          SCRIPT_ERR_ECDSA_SIG_SIZE, *multisigData.checkers.schnorrChecker);

    // Test 26: bitfield size mismatch
    std::vector<uint8_t> longDummy = validSchnorrDummy;
    longDummy.push_back(0x00);
    CheckError("Schnorr CHECKMULTISIG: Bitfield size mismatch",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::SCHNORR>(
                   multisigData.keys, multisigData.sigs, longDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_BITFIELD_SIZE, *multisigData.checkers.schnorrChecker);

    // Test 27: bitfield padding out of range.
    std::vector<uint8_t> rangeDummy = validSchnorrDummy;
    if (!rangeDummy.empty()) {
        rangeDummy.back() |= 0x80;
    }
    CheckError("Schnorr CHECKMULTISIG: Bitfield padding out of range",
BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::SCHNORR>(
                  multisigData.keys, multisigData.sigs, rangeDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_BITFIELD_RANGE, *multisigData.checkers.schnorrChecker);

    // Test 28: bit count does not match number of signatures.
    std::vector<uint8_t> bitCountDummy = validSchnorrDummy;
    size_t extraBit = 0;
    while (extraBit < multisigData.keys.size() &&
           std::find(multisigData.sigToKey.begin(), multisigData.sigToKey.end(), extraBit) != multisigData.sigToKey.end()) {
        ++extraBit;
    }
    BOOST_REQUIRE(extraBit < multisigData.keys.size());
    bitCountDummy[extraBit / 8] |= static_cast<uint8_t>(1u << (extraBit % 8));
    CheckError("Schnorr CHECKMULTISIG: Bit count does not match signature count",
BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::SCHNORR>(
                  multisigData.keys, multisigData.sigs, bitCountDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_BIT_COUNT, *multisigData.checkers.schnorrChecker);

    std::vector<uint8_t> zeroBitDummy(validSchnorrDummy.size(), 0x00);
    CheckError("Schnorr CHECKMULTISIG: Zero bitfield with non-zero signature count",
BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::SCHNORR>(
                  multisigData.keys, multisigData.sigs, zeroBitDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_BIT_COUNT, *multisigData.checkers.schnorrChecker);

    // Test 29: Schnorr - Wrong pubkey with STANDARD (NULLFAIL)
    std::vector<TBCScriptValidation::KeyMaterial> wrongSchnorrKeys = multisigData.keys;
    wrongSchnorrKeys[multisigData.sigToKey.back()] = otherKeys;
    CheckError("Schnorr CHECKMULTISIG: Wrong pubkey with STANDARD (NULLFAIL)",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::SCHNORR>(
                   wrongSchnorrKeys, multisigData.sigs, validSchnorrDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_NULLFAIL, *multisigData.checkers.schnorrChecker);

    // Test 30: Schnorr - Zero-length x-only pubkey
    std::vector<TBCScriptValidation::KeyMaterial> emptyXonlyKeys = multisigData.keys;
    emptyXonlyKeys[multisigData.sigToKey.back()].xonlyPubkey.clear();
    CheckError("Schnorr CHECKMULTISIG: Zero-length pubkey (STANDARD)",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::SCHNORR>(
                   emptyXonlyKeys, multisigData.sigs, validSchnorrDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_XONLY_PUBKEY_SIZE, *multisigData.checkers.schnorrChecker);

    // Test 31: Schnorr - Mixed empty/non-empty signatures.
    std::vector<TBCScriptValidation::SignatureMaterial> mixedEmptySchnorr = multisigData.sigs;
    mixedEmptySchnorr.back().schnorrSig.clear();
    CheckError("Schnorr CHECKMULTISIG: Mixed empty/non-empty signatures",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::SCHNORR>(
                   multisigData.keys, mixedEmptySchnorr, validSchnorrDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SCHNORR_SIG_SIZE, *multisigData.checkers.schnorrChecker);

    // Test 32: Schnorr - All signatures empty.
    std::vector<TBCScriptValidation::SignatureMaterial> allEmptySchnorr = multisigData.sigs;
    for (auto& sig : allEmptySchnorr) {
        sig.schnorrSig.clear();
    }
    CheckError("Schnorr CHECKMULTISIG: All signatures empty",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::SCHNORR>(
                   multisigData.keys, allEmptySchnorr, validSchnorrDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SCHNORR_SIG_SIZE, *multisigData.checkers.schnorrChecker);

    // ========================================================================
    // SECTION 3: Mixed ECDSA/Schnorr signatures
    // ========================================================================

    // Test 33: mixed ECDSA/Schnorr signatures are rejected in ECDSA branch.
    std::vector<TBCScriptValidation::SignatureMaterial> mixedSigInEcdsaBranch = multisigData.sigs;
    mixedSigInEcdsaBranch.back().ecdsaSig = multisigData.sigs.back().schnorrSig;
    CheckError("CHECKMULTISIG: Mixed ECDSA/Schnorr signatures in ECDSA branch",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                   multisigData.keys, mixedSigInEcdsaBranch, emptyDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_ECDSA_SIG_SIZE, *multisigData.checkers.ecdsaChecker);

    // Test 34: mixed ECDSA/Schnorr signatures are rejected in Schnorr branch.
    std::vector<TBCScriptValidation::SignatureMaterial> mixedSigInSchnorrBranch = multisigData.sigs;
    mixedSigInSchnorrBranch.back().schnorrSig = multisigData.sigs.back().ecdsaSig;
    CheckError("CHECKMULTISIG: Mixed ECDSA/Schnorr signatures in Schnorr branch",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::SCHNORR>(
                   multisigData.keys, mixedSigInSchnorrBranch, validSchnorrDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SCHNORR_SIG_SIZE, *multisigData.checkers.schnorrChecker);

    // ========================================================================
    // SECTION 4: Mixed x-only/compressed pubkeys
    // ========================================================================

    // Test 35: x-only/compressed pubkey mix is rejected in ECDSA branch.
    std::vector<TBCScriptValidation::KeyMaterial> mixedPubkeyInEcdsaBranch = multisigData.keys;
    mixedPubkeyInEcdsaBranch[multisigData.sigToKey.back()].compressedPubkey = multisigData.keys[multisigData.sigToKey.back()].xonlyPubkey;
    CheckError("CHECKMULTISIG: Mixed x-only/compressed pubkeys in ECDSA branch",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                   mixedPubkeyInEcdsaBranch, multisigData.sigs, emptyDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_LEGACY_PUBKEY, *multisigData.checkers.ecdsaChecker);

    // Test 36: x-only/compressed pubkey mix is rejected in Schnorr branch.
    std::vector<TBCScriptValidation::KeyMaterial> mixedPubkeyInSchnorrBranch = multisigData.keys;
    mixedPubkeyInSchnorrBranch[multisigData.sigToKey.back()].xonlyPubkey = multisigData.keys[multisigData.sigToKey.back()].compressedPubkey;
    CheckError("CHECKMULTISIG: Mixed x-only/compressed pubkeys in Schnorr branch",
               BuildCheckmultisigScript<TBCScriptValidation::SignatureMethod::SCHNORR>(
                   mixedPubkeyInSchnorrBranch, multisigData.sigs, validSchnorrDummy),
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_XONLY_PUBKEY_SIZE, *multisigData.checkers.schnorrChecker);

    // ========================================================================
    // SECTION 5: Stack / count validation
    // ========================================================================
    // Test 37: No stack parameters (0 elements).
    CheckErrorForAllFlags("CHECKMULTISIG: Insufficient stack parameters (0 elements)",
                          CScript() << OP_CHECKMULTISIG,
                          SCRIPT_ERR_INVALID_STACK_OPERATION, *multisigData.checkers.ecdsaChecker);

    // Test 38: Only dummy on stack (1 element); need nKeys, nSigs, sigs, then dummy.
    CheckErrorForAllFlags("CHECKMULTISIG: Insufficient stack parameters (1 element)",
                          CScript() << OP_0 << OP_CODESEPARATOR << OP_CHECKMULTISIG,
                          SCRIPT_ERR_INVALID_STACK_OPERATION, *multisigData.checkers.ecdsaChecker);

    // Test 39: Negative pubkey count.
    CheckErrorForAllFlags("CHECKMULTISIG: Negative pubkey count",
                          CScript() << OP_0 << OP_1NEGATE << OP_CHECKMULTISIG,
                          SCRIPT_ERR_PUBKEY_COUNT, *multisigData.checkers.ecdsaChecker);

    // Test 40: Negative signature count.
    CheckErrorForAllFlags("CHECKMULTISIG: Negative signature count",
                          CScript() << OP_0 << OP_1NEGATE << multisigData.keys[0].compressedPubkey
                                    << OP_1 << OP_CHECKMULTISIG,
                          SCRIPT_ERR_SIG_COUNT, *multisigData.checkers.ecdsaChecker);

    // Test 41: Signature count larger than pubkey count.
    CheckErrorForAllFlags("CHECKMULTISIG: Signature count larger than pubkey count",
                          CScript() << OP_0 << OP_2 << multisigData.keys[0].compressedPubkey
                                    << OP_1 << OP_CHECKMULTISIG,
                          SCRIPT_ERR_SIG_COUNT, *multisigData.checkers.ecdsaChecker);

    // Test 42: Pubkey count exceeds consensus limit.
    CheckErrorForAllFlags("CHECKMULTISIG: Pubkey count exceeds consensus limit",
                          CScript() << OP_0 << 100 << OP_CHECKMULTISIG,
                          SCRIPT_ERR_PUBKEY_COUNT, *multisigData.checkers.ecdsaChecker);

    // ========================================================================
    // SECTION 6: OP_CHECKMULTISIGVERIFY
    // ========================================================================
    TestData multisigVerifyData(5, 3, TestData::OpCode::CHECKMULTISIGVERIFY);
    std::vector<uint8_t> validVerifySchnorrDummy =
        multisigVerifyData.BuildSchnorrDummy(multisigVerifyData.keys.size());

    // Test 43: CHECKMULTISIGVERIFY ECDSA success.
    CheckPass("CHECKMULTISIGVERIFY: ECDSA success",
              BuildCheckmultisigverifyScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                  multisigVerifyData.keys, multisigVerifyData.sigs, emptyDummy),
              STANDARD_SCRIPT_VERIFY_FLAGS, 0, {}, *multisigVerifyData.checkers.ecdsaChecker);

    // Test 44: CHECKMULTISIGVERIFY Schnorr success.
    CheckPass("CHECKMULTISIGVERIFY: Schnorr success",
              BuildCheckmultisigverifyScript<TBCScriptValidation::SignatureMethod::SCHNORR>(
                  multisigVerifyData.keys, multisigVerifyData.sigs, validVerifySchnorrDummy),
              STANDARD_SCRIPT_VERIFY_FLAGS, 0, {}, *multisigVerifyData.checkers.schnorrChecker);

    // Test 45: CHECKMULTISIGVERIFY 1-of-1 with empty ECDSA signature
    TestData oneOfOneVerify(1, 1, TestData::OpCode::CHECKMULTISIGVERIFY);
    std::vector<TBCScriptValidation::SignatureMaterial> oneEmptySig(1);
    std::vector<uint8_t> oneOfOneDummy;
    CheckErrorForAllFlags("CHECKMULTISIGVERIFY: 1-of-1 with empty ECDSA signature",
                         BuildCheckmultisigverifyScript<TBCScriptValidation::SignatureMethod::ECDSA>(
                             oneOfOneVerify.keys, oneEmptySig, oneOfOneDummy),
                         SCRIPT_ERR_CHECKMULTISIGVERIFY, *oneOfOneVerify.checkers.ecdsaChecker);
}

BOOST_AUTO_TEST_SUITE_END()
