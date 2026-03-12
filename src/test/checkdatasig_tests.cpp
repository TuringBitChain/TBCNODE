#include <policy/policy.h>
#include <script/interpreter.h>
#include <vector>

#include <boost/test/unit_test.hpp>
#include <test/test_bitcoin.h>

#include "config.h"
#include "script/script_flags.h"
#include "taskcancellation.h"

BOOST_FIXTURE_TEST_SUITE(checkdatasig_tests, BasicTestingSetup)

namespace {
const std::vector<uint8_t> failure = {};
const std::vector<uint8_t> success = {1};

std::array<uint32_t, 2> flagset{{0, STANDARD_SCRIPT_VERIFY_FLAGS}};

// Function to run test cases
void RunCase(const std::string& test_name,
             const CScript& script,
             uint32_t flags,
             bool exp_status,
             ScriptError exp_error,
             bool check_stack,
             size_t exp_stack_size,
             const std::vector<uint8_t>& exp_stack_top) {
    const Config& config = GlobalConfig::GetConfig();

    BaseSignatureChecker checker;

    ScriptError error;
    LimitedStack stack(UINT32_MAX);
    const auto status = EvalScript(
        config, false, task::CCancellationSource::Make()->GetToken(), stack,
        script, flags, checker, &error);

    BOOST_CHECK_MESSAGE(exp_status == status.value(),
                        test_name << " - Status mismatch, expected: " << (exp_status ? "true" : "false")
                                  << ", actual: " << (status.value() ? "true" : "false"));

    BOOST_CHECK_MESSAGE(exp_error == error,
                        test_name << " - Error code mismatch, expected: " << ScriptErrorString(exp_error)
                                  << ", actual: " << ScriptErrorString(error));

    if (check_stack) {
        BOOST_CHECK_MESSAGE(exp_stack_size == stack.size(),
                            test_name << " - Stack size mismatch, expected: " << exp_stack_size
                                      << ", actual: " << stack.size());

        if (exp_stack_size > 0 && stack.size() > 0) {
            const auto& stack_0 = stack.at(0);
            const auto& stack_0_element = stack_0.GetElement();
            BOOST_CHECK_MESSAGE(
                std::equal(stack_0_element.begin(), stack_0_element.end(),
                           exp_stack_top.begin(), exp_stack_top.end()),
                test_name << " - Stack top value mismatch"
                          << ", expected (hex): " << HexStr(exp_stack_top)
                          << " (size: " << exp_stack_top.size() << ")"
                          << ", actual (hex): " << HexStr(stack_0_element)
                          << " (size: " << stack_0_element.size() << ")");
        } else if (exp_stack_size > 0 && stack.size() == 0) {
            BOOST_CHECK_MESSAGE(false,
                                test_name << " - Stack is empty but expected stack size: " << exp_stack_size
                                          << ", expected top value (hex): " << HexStr(exp_stack_top));
        }
    }
}

void CheckPass(const std::string& test_name,
               const CScript& script,
               uint32_t flags,
               size_t exp_stack_size,
               const std::vector<uint8_t>& exp_stack_top) {
    RunCase(test_name, script, flags, true, SCRIPT_ERR_OK, true, exp_stack_size, exp_stack_top);
}

void CheckError(const std::string& test_name,
                const CScript& script,
                uint32_t flags,
                ScriptError exp_error) {
    RunCase(test_name, script, flags, false, exp_error, false, 0, failure);
}

void CheckPassForAllFlags(const std::string& test_name,
                          const CScript& script,
                          size_t exp_stack_size,
                          const std::vector<uint8_t>& exp_stack_top) {
    for (uint32_t flags : flagset) {
        CheckPass(test_name, script, flags, exp_stack_size, exp_stack_top);
    }
}

void CheckErrorForAllFlags(const std::string& test_name,
                           const CScript& script,
                           ScriptError exp_error) {
    for (uint32_t flags : flagset) {
        CheckError(test_name, script, flags, exp_error);
    }
}

// Test data structure for checkdatasig tests
struct TestData {
    // ECDSA test data
    std::vector<uint8_t> compressed_pubkey;
    std::vector<uint8_t> uncompressed_pubkey;
    std::vector<uint8_t> hybrid_pubkey;
    std::vector<uint8_t> ecdsa_message;
    std::vector<uint8_t> ecdsa_message_single_sha256;
    std::vector<uint8_t> ecdsa_comp_sig;
    std::vector<uint8_t> ecdsa_uncomp_sig;
    std::vector<uint8_t> ecdsa_hybrid_sig;

    // Schnorr test data
    std::vector<uint8_t> schnorr_pubkey;
    std::vector<uint8_t> schnorr_message;
    std::vector<uint8_t> schnorr_message_single_sha256;
    std::vector<uint8_t> schnorr_sig;

    TestData() {
        // ========== ECDSA test data setup ==========
        // Generate compressed pubkey
        CKey key_comp;
        key_comp.MakeNewKey(true);
        CPubKey pubkey_comp = key_comp.GetPubKey();
        compressed_pubkey = std::vector<uint8_t>(pubkey_comp.begin(), pubkey_comp.end());

        // Generate uncompressed pubkey
        CKey key_uncomp;
        key_uncomp.MakeNewKey(false);
        CPubKey pubkey_uncomp = key_uncomp.GetPubKey();
        uncompressed_pubkey = std::vector<uint8_t>(pubkey_uncomp.begin(), pubkey_uncomp.end());

        // Generate hybrid pubkey (from uncompressed pubkey)
        CPubKey pubkey_hybrid = pubkey_uncomp;
        // Convert to hybrid format: 0x06 for even y, 0x07 for odd y
        *const_cast<uint8_t *>(&pubkey_hybrid[0]) = 0x06 | (pubkey_hybrid[64] & 1);
        hybrid_pubkey = std::vector<uint8_t>(pubkey_hybrid.begin(), pubkey_hybrid.end());

        // Create a message
        ecdsa_message = {0x01, 0x02, 0x03, 0x04};

        // single SHA256 hash
        uint256 ecdsa_message_single_sha256_hash;
        CHash256().Write(ecdsa_message.data(), ecdsa_message.size()).SingleFinalize(ecdsa_message_single_sha256_hash.begin());
        ecdsa_message_single_sha256 = std::vector<uint8_t>(ecdsa_message_single_sha256_hash.begin(), ecdsa_message_single_sha256_hash.end());

        // double SHA256 hash
        uint256 ecdsa_message_double_sha256_hash;
        CHash256().Write(ecdsa_message.data(), ecdsa_message.size()).Finalize(ecdsa_message_double_sha256_hash.begin());

        // Sign double SHA256 hash with compressed key
        if (!key_comp.Sign(ecdsa_message_double_sha256_hash, ecdsa_comp_sig)) {
            BOOST_FAIL("Failed to generate ECDSA signature for compressed key");
        }

        // Sign double SHA256 hash with uncompressed key
        if (!key_uncomp.Sign(ecdsa_message_double_sha256_hash, ecdsa_uncomp_sig)) {
            BOOST_FAIL("Failed to generate ECDSA signature for uncompressed key");
        }

        // Sign double SHA256 hash with hybrid key (uses same key as uncompressed)
        if (!key_uncomp.Sign(ecdsa_message_double_sha256_hash, ecdsa_hybrid_sig)) {
            BOOST_FAIL("Failed to generate ECDSA signature for hybrid key");
        }

        // ========== Schnorr test data setup ==========
        std::string schnorr_pubkey_str = "4acaff36ad050361ba617c7597cbab7e9145b77da06f07cf32681139bbf599c8";
        std::string schnorr_msg_str = "68656c6c6f";
        std::string schnorr_msg_single_sha256_str = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";
        std::string schnorr_sig_str = "0a75f4eed0e32143ff8fe1500824e17168d224e01441c51cd47602084ba14e36023a495d06c7f7bef067e7ced1afef92244e383e284bd45657f41fb676096450";

        schnorr_pubkey = ParseHex(schnorr_pubkey_str);
        schnorr_message = ParseHex(schnorr_msg_str);
        schnorr_message_single_sha256 = ParseHex(schnorr_msg_single_sha256_str);
        schnorr_sig = ParseHex(schnorr_sig_str);
    }
};
} // namespace

BOOST_AUTO_TEST_CASE(checkdatasig_test) {
    TestData data;

    // Flag format: [data_conversion_method, sig_func] where:
    //   data_conversion_method: 0x01=SINGLE_SHA256, 0x02=DOUBLE_SHA256
    //   sig_func: 0x00=NONE, 0x01=ECDSA, 0x02=SCHNORR
    
    // ========================================================================
    // SECTION 1: ECDSA Signature Tests
    // ========================================================================

    // Test 1: ECDSA with compressed pubkey - valid signature (double sha256 method)
    CheckPassForAllFlags("ECDSA: Valid compressed pubkey (double sha256 method)",
                         CScript() << data.ecdsa_comp_sig << data.ecdsa_message << data.compressed_pubkey
                                   << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
                         1, success);

    // Test 2: ECDSA with uncompressed pubkey - valid signature (double sha256 method)
    CheckPassForAllFlags("ECDSA: Valid uncompressed pubkey (double sha256 method)",
                         CScript() << data.ecdsa_uncomp_sig << data.ecdsa_message << data.uncompressed_pubkey
                                   << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
                         1, success);

    // Test 3: ECDSA with hybrid pubkey - valid signature (double sha256 method)
    CheckPass("ECDSA: Valid hybrid pubkey (double sha256 method)",
              CScript() << data.ecdsa_hybrid_sig << data.ecdsa_message << data.hybrid_pubkey
                        << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
              0, 1, success);

    CheckError("ECDSA: Valid hybrid pubkey (double sha256 method)",
               CScript() << data.ecdsa_hybrid_sig << data.ecdsa_message << data.hybrid_pubkey
                         << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_LEGACY_PUBKEY);

    // Test 4: ECDSA with compressed pubkey - valid signature (single sha256 method)
    CheckPassForAllFlags("ECDSA: Valid compressed pubkey (single sha256 method)",
                         CScript() << data.ecdsa_comp_sig << data.ecdsa_message_single_sha256 << data.compressed_pubkey
                                   << std::vector<uint8_t>{0x01, 0x01} << OP_CHECKDATASIG,
                         1, success);

    // Test 5: ECDSA with uncompressed pubkey - valid signature (single sha256 method)
    CheckPassForAllFlags("ECDSA: Valid uncompressed pubkey (single sha256 method)",
                         CScript() << data.ecdsa_uncomp_sig << data.ecdsa_message_single_sha256 << data.uncompressed_pubkey
                                   << std::vector<uint8_t>{0x01, 0x01} << OP_CHECKDATASIG,
                         1, success);

    // Test 6: ECDSA with hybrid pubkey - valid signature (single sha256 method)
    CheckPass("ECDSA: Valid hybrid pubkey (single sha256 method)",
              CScript() << data.ecdsa_hybrid_sig << data.ecdsa_message_single_sha256 << data.hybrid_pubkey
                        << std::vector<uint8_t>{0x01, 0x01} << OP_CHECKDATASIG,
              0, 1, success);

    CheckError("ECDSA: Valid hybrid pubkey (single sha256 method)",
               CScript() << data.ecdsa_hybrid_sig << data.ecdsa_message_single_sha256 << data.hybrid_pubkey
                         << std::vector<uint8_t>{0x01, 0x01} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_LEGACY_PUBKEY);

    // Test 7: ECDSA signature with mismatched message
    std::vector<uint8_t> mismatched_ecdsa_message{0x05, 0x06, 0x07, 0x08};
    CheckPass("ECDSA: Mismatched message",
              CScript() << data.ecdsa_comp_sig << mismatched_ecdsa_message << data.compressed_pubkey
                        << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
              0, 1, failure);

    CheckError("ECDSA: Mismatched message",
               CScript() << data.ecdsa_comp_sig << mismatched_ecdsa_message << data.compressed_pubkey
                         << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_NULLFAIL);

    // Test 8: ECDSA signature with invalid signature
    std::vector<uint8_t> invalid_ecdsa_sig{0x05, 0x06, 0x07, 0x08};
    CheckPass("ECDSA: Invalid signature",
              CScript() << invalid_ecdsa_sig << data.ecdsa_message << data.compressed_pubkey
                        << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
              0, 1, failure);

    CheckError("ECDSA: Invalid signature",
               CScript() << invalid_ecdsa_sig << data.ecdsa_message << data.compressed_pubkey
                         << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_DER);

    // Test 9: ECDSA - Invalid DER signature encoding
    // Use a signature with incorrect length field (length says 0x44 but actual length is shorter)
    std::vector<uint8_t> invalid_der_sig = ParseHex(
        "3044022057292e2d4dfe775becdd0a9e6547997c728cdf35390f6a017da56d654d374e49"
        "02206b643be2fc53763b4e284845bfea2c597d2dc7759941dce937636c9d341b71");
    invalid_der_sig.pop_back();
    CheckError("ECDSA: Invalid DER signature encoding",
               CScript() << invalid_der_sig << data.ecdsa_message << data.compressed_pubkey
                         << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_DER);

    // Test 10: ECDSA - Signature with high S value
    // DER signature with high S: 0x304502...022100... (S value > order/2)
    std::vector<uint8_t> high_s_sig = ParseHex(
        "304502203e4516da7253cf068effec6b95c41221c0cf3a8e6ccb8cbf1725b562e9afde2c"
        "022100ab1e3da73d67e32045a20e0b999e049978ea8d6ee5480d485fcf2ce0d03b2ef0");
    CheckError("ECDSA: Signature with high S value",
               CScript() << high_s_sig << data.ecdsa_message << data.compressed_pubkey
                         << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_HIGH_S);

    // Test 11: ECDSA - Invalid pubkey
    std::vector<uint8_t> invalid_ecdsa_pubkey(32, 0x11);
    CheckPass("ECDSA: Invalid pubkey",
              CScript() << data.ecdsa_comp_sig << data.ecdsa_message << invalid_ecdsa_pubkey
                        << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
              0, 1, failure);

    CheckError("ECDSA: Invalid pubkey",
               CScript() << data.ecdsa_comp_sig << data.ecdsa_message << invalid_ecdsa_pubkey
                         << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_LEGACY_PUBKEY);

    // Test 12: ECDSA - Uncompressed pubkey with COMPRESSED_PUBKEYTYPE flags
    CheckError("ECDSA: Uncompressed pubkey rejected with COMPRESSED_PUBKEYTYPE flags",
               CScript() << data.ecdsa_uncomp_sig << data.ecdsa_message << data.uncompressed_pubkey
                         << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS | SCRIPT_VERIFY_COMPRESSED_PUBKEYTYPE, SCRIPT_ERR_NONCOMPRESSED_PUBKEY);

    // Test 13: ECDSA - Hybrid pubkey with COMPRESSED_PUBKEYTYPE flags
    CheckError("ECDSA: Hybrid pubkey rejected with COMPRESSED_PUBKEYTYPE flags",
               CScript() << data.ecdsa_hybrid_sig << data.ecdsa_message << data.hybrid_pubkey
                         << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS | SCRIPT_VERIFY_COMPRESSED_PUBKEYTYPE, SCRIPT_ERR_LEGACY_PUBKEY);

    // ========================================================================
    // SECTION 2: Schnorr Signature Tests
    // ========================================================================

    // Test 14: Valid Schnorr signature verification (double sha256 method)
    CheckPassForAllFlags("Schnorr: Valid signature (double sha256 method)",
                         CScript() << data.schnorr_sig << data.schnorr_message << data.schnorr_pubkey
                                   << std::vector<uint8_t>{0x02, 0x02} << OP_CHECKDATASIG,
                         1, success);

    // Test 15: Valid Schnorr signature verification (single sha256 method)
    CheckPassForAllFlags("Schnorr: Valid signature (single sha256 method)",
                         CScript() << data.schnorr_sig << data.schnorr_message_single_sha256 << data.schnorr_pubkey
                                   << std::vector<uint8_t>{0x01, 0x02} << OP_CHECKDATASIG,
                         1, success);

    // Test 16: Schnorr - Mismatched message
    std::vector<uint8_t> mismatched_schnorr_message{0x05, 0x06, 0x07, 0x08};
    CheckPass("Schnorr: Mismatched message",
              CScript() << data.schnorr_sig << mismatched_schnorr_message << data.schnorr_pubkey
                        << std::vector<uint8_t>{0x02, 0x02} << OP_CHECKDATASIG,
              0, 1, failure);

    CheckError("Schnorr: Mismatched message",
               CScript() << data.schnorr_sig << mismatched_schnorr_message << data.schnorr_pubkey
                         << std::vector<uint8_t>{0x02, 0x02} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_NULLFAIL);

    // Test 17: Schnorr - Invalid signature
    std::vector<uint8_t> invalid_schnorr_sig(64, 0xff);
    CheckPass("Schnorr: Invalid signature bytes",
              CScript() << invalid_schnorr_sig << data.schnorr_message << data.schnorr_pubkey
                        << std::vector<uint8_t>{0x02, 0x02} << OP_CHECKDATASIG,
              0, 1, failure);

    CheckError("Schnorr: Invalid signature bytes",
               CScript() << invalid_schnorr_sig << data.schnorr_message << data.schnorr_pubkey
                         << std::vector<uint8_t>{0x02, 0x02} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_NULLFAIL);

    // Test 18: Schnorr - Invalid signature length
    std::vector<uint8_t> short_schnorr_sig(32, 0x11);
    CheckErrorForAllFlags("Schnorr: Invalid signature length",
                          CScript() << short_schnorr_sig << data.schnorr_message << data.schnorr_pubkey
                                    << std::vector<uint8_t>{0x02, 0x02} << OP_CHECKDATASIG,
                          SCRIPT_ERR_SCHNORR_SIG_SIZE);

    // Test 19: Schnorr - Invalid pubkey size
    std::vector<uint8_t> bad_schnorr_pubkey(33, 0x11);
    CheckErrorForAllFlags("Schnorr: Invalid pubkey size",
                          CScript() << data.schnorr_sig << data.schnorr_message << bad_schnorr_pubkey
                                    << std::vector<uint8_t>{0x02, 0x02} << OP_CHECKDATASIG,
                          SCRIPT_ERR_XONLY_PUBKEY_SIZE);

    // ========================================================================
    // SECTION 3: None Signature Method Tests
    // ========================================================================

    std::vector<uint8_t> empty_sig;
    // Test 20: NONE method with empty signature - compressed pubkey
    CheckPassForAllFlags("NONE: Empty signature with compressed pubkey",
                         CScript() << empty_sig << data.ecdsa_message << data.compressed_pubkey
                                   << std::vector<uint8_t>{0x02, 0x00} << OP_CHECKDATASIG,
                         1, failure);

    // Test 21: NONE method with empty signature - uncompressed pubkey
    CheckPassForAllFlags("NONE: Empty signature with uncompressed pubkey",
                         CScript() << empty_sig << data.ecdsa_message << data.uncompressed_pubkey
                                   << std::vector<uint8_t>{0x02, 0x00} << OP_CHECKDATASIG,
                         1, failure);

    // Test 22: NONE method with empty signature - x-only pubkey
    CheckPassForAllFlags("NONE: Empty signature with x-only pubkey",
                         CScript() << empty_sig << data.schnorr_message << data.schnorr_pubkey
                                   << std::vector<uint8_t>{0x02, 0x00} << OP_CHECKDATASIG,
                         1, failure);

    // Test 23: NONE method with non-empty signature
    CheckErrorForAllFlags("NONE: Non-empty signature should fail",
                          CScript() << data.ecdsa_comp_sig << data.ecdsa_message << data.compressed_pubkey
                                    << std::vector<uint8_t>{0x02, 0x00} << OP_CHECKDATASIG,
                          SCRIPT_ERR_EMPTY_SIG_SIZE);

    // Test 24: NONE method with hybrid pubkey
    CheckPass("NONE: Empty signature with hybrid pubkey",
              CScript() << empty_sig << data.ecdsa_message << data.hybrid_pubkey
                        << std::vector<uint8_t>{0x02, 0x00} << OP_CHECKDATASIG,
              0, 1, failure);

    CheckError("NONE: Empty signature with hybrid pubkey",
               CScript() << empty_sig << data.ecdsa_message << data.hybrid_pubkey
                         << std::vector<uint8_t>{0x02, 0x00} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_PUBKEY_NOT_XONLY_OR_LEGACY);

    // Test 25: NONE method with uncompressed pubkey and COMPRESSED_PUBKEYTYPE flags
    CheckError("NONE: Uncompressed pubkey rejected with COMPRESSED_PUBKEYTYPE flags",
               CScript() << empty_sig << data.ecdsa_message << data.uncompressed_pubkey
                         << std::vector<uint8_t>{0x02, 0x00} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS | SCRIPT_VERIFY_COMPRESSED_PUBKEYTYPE, SCRIPT_ERR_PUBKEY_NOT_XONLY_OR_COMPRESSED);

    // ========================================================================
    // SECTION 4: Flag Validation Tests
    // ========================================================================

    // Test 26: Invalid message type flag
    CheckErrorForAllFlags("Flag: Invalid message type 0x00",
                          CScript() << data.schnorr_sig << data.schnorr_message << data.schnorr_pubkey
                                    << std::vector<uint8_t>{0x00, 0x02} << OP_CHECKDATASIG,
                          SCRIPT_ERR_CHECKDATASIG_FLAG);

    // Test 27: Invalid sig_func
    CheckErrorForAllFlags("Flag: Invalid sig_func 0x03",
                          CScript() << data.schnorr_sig << data.schnorr_message << data.schnorr_pubkey
                                    << std::vector<uint8_t>{0x01, 0x03} << OP_CHECKDATASIG,
                          SCRIPT_ERR_CHECKDATASIG_FLAG);

    // Test 28: Flag length too short
    CheckErrorForAllFlags("Flag: Length too short",
                          CScript() << data.schnorr_sig << data.schnorr_message << data.schnorr_pubkey
                                    << 0x01 << OP_CHECKDATASIG,
                          SCRIPT_ERR_CHECKDATASIG_FLAG);

    // Test 29: Flag length too long
    CheckErrorForAllFlags("Flag: Length too long",
                          CScript() << data.schnorr_sig << data.schnorr_message << data.schnorr_pubkey
                                    << std::vector<uint8_t>{0x01, 0x02, 0x03} << OP_CHECKDATASIG,
                          SCRIPT_ERR_CHECKDATASIG_FLAG);

    // ========================================================================
    // SECTION 5: Data Conversion Method MisMatch Tests
    // ========================================================================

    // Test 30: Flag is Double SHA256 (0x02) but message passed is already SHA256 hashed
    CheckPass("Data Conversion Method Mismatch: Double SHA256 flag but SHA256 hashed message passed",
              CScript() << data.schnorr_sig << data.schnorr_message_single_sha256 << data.schnorr_pubkey
                        << std::vector<uint8_t>{0x02, 0x02} << OP_CHECKDATASIG,
              0, 1, failure);

    CheckError("Data Conversion Method Mismatch: Double SHA256 flag but SHA256 hashed message passed",
               CScript() << data.schnorr_sig << data.schnorr_message_single_sha256 << data.schnorr_pubkey
                         << std::vector<uint8_t>{0x02, 0x02} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_NULLFAIL);

    // Test 31: Flag is Single SHA256 (0x01) but raw message passed
    CheckPass("Data Conversion Method Mismatch: Single SHA256 flag but raw message passed",
              CScript() << data.schnorr_sig << data.schnorr_message << data.schnorr_pubkey
                        << std::vector<uint8_t>{0x01, 0x02} << OP_CHECKDATASIG,
              0, 1, failure);

    CheckError("Data Conversion Method Mismatch: Single SHA256 flag but raw message passed",
               CScript() << data.schnorr_sig << data.schnorr_message << data.schnorr_pubkey
                         << std::vector<uint8_t>{0x01, 0x02} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_NULLFAIL);

    // ========================================================================
    // SECTION 6: Sig Function MisMatch Tests
    // ========================================================================

    // Test 32: ECDSA with Schnorr flag
    CheckErrorForAllFlags("Sig Function Mismatch: ECDSA with Schnorr flag",
                          CScript() << data.ecdsa_comp_sig << data.ecdsa_message << data.compressed_pubkey
                                    << std::vector<uint8_t>{0x02, 0x02} << OP_CHECKDATASIG,
                          SCRIPT_ERR_SCHNORR_SIG_SIZE);

    // Test 33: Schnorr with ECDSA flag
    CheckPass("Sig Function Mismatch: Schnorr with ECDSA flag",
              CScript() << data.schnorr_sig << data.ecdsa_message << data.schnorr_pubkey
                        << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
              0, 1, failure);

    CheckError("Sig Function Mismatch: Schnorr with ECDSA flag",
               CScript() << data.schnorr_sig << data.ecdsa_message << data.schnorr_pubkey
                         << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_DER);

    // ========================================================================
    // SECTION 7: Signature Mismatch Tests
    // ========================================================================

    // Test 34: Use Schnorr signature with ECDSA
    CheckPass("Signature Mismatch: Schnorr sig with ECDSA",
              CScript() << data.schnorr_sig << data.ecdsa_message << data.compressed_pubkey
                        << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
              0, 1, failure);

    CheckError("Signature Mismatch: Schnorr sig with ECDSA",
               CScript() << data.schnorr_sig << data.ecdsa_message << data.compressed_pubkey
                         << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
               STANDARD_SCRIPT_VERIFY_FLAGS, SCRIPT_ERR_SIG_DER);

    // Test 35: Use ECDSA signature with Schnorr
    CheckErrorForAllFlags("Signature Mismatch: ECDSA sig with Schnorr",
                          CScript() << data.ecdsa_comp_sig << data.ecdsa_message << data.schnorr_pubkey
                                    << std::vector<uint8_t>{0x02, 0x02} << OP_CHECKDATASIG,
                          SCRIPT_ERR_SCHNORR_SIG_SIZE);

    // ========================================================================
    // SECTION 8: Pubkey Mismatch Tests
    // ========================================================================

    // Test 36: Use Schnorr key with ECDSA signature
    RunCase("Pubkey Mismatch: Schnorr pubkey with ECDSA",
            CScript() << data.ecdsa_comp_sig << data.ecdsa_message << data.schnorr_pubkey
                      << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIG,
            STANDARD_SCRIPT_VERIFY_FLAGS, false, SCRIPT_ERR_LEGACY_PUBKEY, false, 0, failure);

    // Test 37: Use ECDSA compressed key with Schnorr signature
    RunCase("Pubkey Mismatch: ECDSA pubkey with Schnorr",
            CScript() << data.schnorr_sig << data.schnorr_message << data.compressed_pubkey
                      << std::vector<uint8_t>{0x02, 0x02} << OP_CHECKDATASIG,
            STANDARD_SCRIPT_VERIFY_FLAGS, false, SCRIPT_ERR_XONLY_PUBKEY_SIZE, false, 0, failure);

    // ========================================================================
    // SECTION 10: Stack and Parameter Validation Tests
    // ========================================================================

    // Test 38: Insufficient stack parameters (empty stack)
    CheckErrorForAllFlags("Stack: Insufficient parameters (0 elements)",
                          CScript() << OP_CHECKDATASIG,
                          SCRIPT_ERR_INVALID_STACK_OPERATION);

    // Test 39: Insufficient stack parameters (1 element)
    CheckErrorForAllFlags("Stack: Insufficient parameters (1 element)",
                          CScript() << data.schnorr_sig << OP_CHECKDATASIG,
                          SCRIPT_ERR_INVALID_STACK_OPERATION);

    // Test 40: Insufficient stack parameters (2 elements)
    CheckErrorForAllFlags("Stack: Insufficient parameters (2 elements)",
                          CScript() << data.schnorr_sig << data.schnorr_message << OP_CHECKDATASIG,
                          SCRIPT_ERR_INVALID_STACK_OPERATION);

    // Test 41: Insufficient stack parameters (3 elements)
    CheckErrorForAllFlags("Stack: Insufficient parameters (3 elements)",
                          CScript() << data.schnorr_sig << data.schnorr_message << data.schnorr_pubkey << OP_CHECKDATASIG,
                          SCRIPT_ERR_INVALID_STACK_OPERATION);

    // ========================================================================
    // SECTION 11: OP_CHECKDATASIGVERIFY Tests
    // ========================================================================

    // Test 42: CHECKDATASIGVERIFY success - ECDSA (stack should be empty)
    CheckPassForAllFlags("CHECKDATASIGVERIFY: ECDSA success",
                         CScript() << data.ecdsa_comp_sig << data.ecdsa_message << data.compressed_pubkey
                                   << std::vector<uint8_t>{0x02, 0x01} << OP_CHECKDATASIGVERIFY,
                         0, {});

    // Test 43: CHECKDATASIGVERIFY success - Schnorr (stack should be empty)
    CheckPassForAllFlags("CHECKDATASIGVERIFY: Schnorr success",
                         CScript() << data.schnorr_sig << data.schnorr_message << data.schnorr_pubkey
                                   << std::vector<uint8_t>{0x02, 0x02} << OP_CHECKDATASIGVERIFY,
                         0, {});

    // Test 44: CHECKDATASIGVERIFY failure - NONE method
    CheckErrorForAllFlags("CHECKDATASIGVERIFY: NONE method fails",
                          CScript() << empty_sig << data.ecdsa_message << data.compressed_pubkey
                                    << std::vector<uint8_t>{0x02, 0x00} << OP_CHECKDATASIGVERIFY,
                          SCRIPT_ERR_CHECKDATASIGVERIFY);
}

BOOST_AUTO_TEST_SUITE_END()
