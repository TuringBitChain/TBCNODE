#include "primitives/transaction.h"
#include "validation.cpp"
#include "test/test_bitcoin.h"
#include <boost/test/tools/old/interface.hpp>
#include <boost/test/unit_test.hpp>
#include <boost/test/unit_test_suite.hpp>
#include <cstdint>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(filled_miner_bill_v2_tests, BasicTestingSetup)

CMutableTransaction getValidMutableTransaction() {
    CMutableTransaction mtx;
    mtx.nVersion = 10;
    mtx.nLockTime = 0;

    CTxIn txin;
    txin.prevout = COutPoint(uint256S("0x0000000000000000000000000000000000000000000000000000000000000000"), 0xffffffff);
    std::vector<uint8_t> scriptSigVec = ParseHex("03a0bb0d92000ac5b5b7cf79a1a3e2495861424f71b13209681c44029ecf7cfc6475f1c97001e7658f44009cfe883156ccac0ce06736e71ba9b07b689f07e89d7a8a3bf6");
    txin.scriptSig = CScript(scriptSigVec.begin(), scriptSigVec.end());
    txin.nSequence = 0xffffffff;
    mtx.vin.push_back(txin);
    
    CTxOut txout;
    txout.nValue = Amount(109377392);
    // std::vector<uint8_t> scriptPubKeyVec = ParseHex("76a914b3f89180086dfaa2ed10becff8f3b7051114fd0a88ac 6a 4c 6d 00 139310fe388ffa6f3eb911966a60793f8536846febea92b3ee7c435bca61dcfb ec34ff70aa12b8357ec488a1230bbc24a0bd4989aee41a4dc5e45126f57a155c2758caa0c766b00f824f2d93bb1a8a37324f45b8deb5e6626acafa08a2d3877b 03 40420f 23");
    std::vector<uint8_t> scriptPubKeyVec = ParseHex("76a914f2de6e590a078632a8f60c276d27a3eeb8b4156788ac6a4c6a00139310fe388ffa6f3eb911966a60793f8536846febea92b3ee7c435bca61dcfb28e6316d9d5e67485175931d2090a39a2fa68744a20e3e17ffc515b149c0ebb92a26dda6a25302a2b582d691dfb45e43820b7f4be93b2dbd0d0ecb5fd247d13e0340420f235a480000");
    txout.scriptPubKey = CScript(scriptPubKeyVec.begin(), scriptPubKeyVec.end());
    mtx.vout.push_back(txout);

    CTxOut txout2;
    txout2.nValue = Amount(203129445);
    std::vector<uint8_t> scriptPubKeyVec2 = ParseHex("76a9143b453ad6954e9ebc28e4427e6052682bbe57cd7988ac");
    txout2.scriptPubKey = CScript(scriptPubKeyVec2.begin(), scriptPubKeyVec2.end());
    mtx.vout.push_back(txout2);
    return mtx;
}

// These historical signatures were produced before the production manager
// keys were rotated in a5299339c and must no longer be accepted.
BOOST_AUTO_TEST_CASE(legacy_v2_cb_rejected_after_manager_key_rotation) {
    CMutableTransaction mtx = getValidMutableTransaction();
    CTransaction tx(mtx);
    BOOST_CHECK(!FilledMinerBillV2(tx, uint256S("000000000946664ab39a9591cbb3066fe5569da6ae8529142998b9186a1e9639")));
}

BOOST_AUTO_TEST_CASE(legacy_fixed_charge_cb_rejected_after_manager_key_rotation) {
    CMutableTransaction mtx = getValidMutableTransaction();
    std::vector<uint8_t> scriptPubKeyVec = ParseHex("76a914f2de6e590a078632a8f60c276d27a3eeb8b4156788ac6a4c6a01139310fe388ffa6f3eb911966a60793f8536846febea92b3ee7c435bca61dcfb3d7a2eca4fd7563022d76ee5049f2e111a117df47dd66d4e79353df9bed209ec73d5adf1040921d5dcfa831ff1e38c1694cc1538d258319aa336319b620e5fb80340420f235a480000");
    mtx.vout[0].scriptPubKey = CScript(scriptPubKeyVec.begin(), scriptPubKeyVec.end());

    CTransaction tx(mtx);
    BOOST_CHECK(!FilledMinerBillV2(tx, uint256S("000000000946664ab39a9591cbb3066fe5569da6ae8529142998b9186a1e9639")));
}

// Invalid pre block hash
BOOST_AUTO_TEST_CASE(invalid_v2_cb_wrong_pre_block_hash) {
    CMutableTransaction mtx = getValidMutableTransaction();
    CTransaction tx(mtx);
    BOOST_CHECK(!FilledMinerBillV2(tx, uint256S("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff")));
}

// Permission height overdue
BOOST_AUTO_TEST_CASE(invalid_v2_cb_permission_height_overdue) {
    CMutableTransaction mtx = getValidMutableTransaction();
    std::vector<uint8_t> scriptSigVecNew = ParseHex("76a914b3f89180086dfaa2ed10becff8f3b7051114fd0a88ac6a4c6d01139310fe388ffa6f3eb911966a60793f8536846febea92b3ee7c435bca61dcfb743c39c380f5a2416cea5d5c9bbda365b992cf26cd150b4172493dd5d9c08431443bd01600435418fde4455676045a7a1e7c57c598b41f6953c57f0bef1f2ca00320a10723");
    mtx.vout[0].scriptPubKey = CScript(scriptSigVecNew.begin(), scriptSigVecNew.end());
    CTransaction tx_permission_height_overdue(mtx);
    BOOST_CHECK(!FilledMinerBillV2(tx_permission_height_overdue, uint256S("000000000946664ab39a9591cbb3066fe5569da6ae8529142998b9186a1e9639")));
}

// Don't meet charge rate
BOOST_AUTO_TEST_CASE(invalid_v2_cb_dont_meet_charge_rate) {
    CMutableTransaction mtx = getValidMutableTransaction();
    mtx.vout[0].nValue = Amount(100000000);
    mtx.vout[1].nValue = Amount(1000000000);
    CTransaction tx(mtx);
    BOOST_CHECK(!FilledMinerBillV2(tx, uint256S("000000000946664ab39a9591cbb3066fe5569da6ae8529142998b9186a1e9639")));
}

// Charge address doesn't match ask
BOOST_AUTO_TEST_CASE(invalid_v2_cb_charge_address_doesnt_match_ask) {
    CMutableTransaction mtx = getValidMutableTransaction();
    std::vector<uint8_t> scriptPubKeyVec = ParseHex("76a9143b453ad6954e9ebc28e4427e6052682bbe57cd7988ac6a4c6d01139310fe388ffa6f3eb911966a60793f8536846febea92b3ee7c435bca61dcfb4712abee6b71711b110d667ec06dfcd828f1877b0e9b11b5b61875264dc8994f18bb8efc6262a5730a547658f4af7037b4bc2c3c7629e95b54bd37bd9ee463530340420f23");
    mtx.vout[0].scriptPubKey = CScript(scriptPubKeyVec.begin(), scriptPubKeyVec.end());
    CTransaction tx(mtx);
    BOOST_CHECK(!FilledMinerBillV2(tx, uint256S("000000000946664ab39a9591cbb3066fe5569da6ae8529142998b9186a1e9639")));
}

// Invalid miner sig
BOOST_AUTO_TEST_CASE(invalid_v2_cb_invalid_miner_sig) {
    CMutableTransaction mtx = getValidMutableTransaction();
    std::vector<uint8_t> scriptVec = ParseHex("03a0bb0dffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    mtx.vin[0].scriptSig = CScript(scriptVec.begin(), scriptVec.end());
    CTransaction tx(mtx);
    BOOST_CHECK(!FilledMinerBillV2(tx, uint256S("000000000946664ab39a9591cbb3066fe5569da6ae8529142998b9186a1e9639")));
}

// Invalid manager sig
BOOST_AUTO_TEST_CASE(invalid_v2_cb_invalid_manager_sig) {
    CMutableTransaction mtx = getValidMutableTransaction();
    std::vector<uint8_t> scriptPubKeyVec = ParseHex("76a914b3f89180086dfaa2ed10becff8f3b7051114fd0a88ac6a4c6d00139310fe388ffa6f3eb911966a60793f8536846febea92b3ee7c435bca61dcfbffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff0340420f23");
    mtx.vout[0].scriptPubKey = CScript(scriptPubKeyVec.begin(), scriptPubKeyVec.end());
    CTransaction tx(mtx);
    BOOST_CHECK(!FilledMinerBillV2(tx, uint256S("000000000946664ab39a9591cbb3066fe5569da6ae8529142998b9186a1e9639")));
}

BOOST_AUTO_TEST_CASE(coinbase_height_prefix_preserves_raw_miner_signature) {
    const std::vector<uint8_t> scriptBytes = ParseHex(
        "03bd8f0e"
        "4fabdda63ab134ac2d4c386d01abbf7dbec2824804afe22893604f9d1bc22edda"
        "9eb1ae46fb52498370bb4ac0ac92ea65eb1dbf58b6ca4c5302203f8ae7a8b25");
    const CScript scriptSig(scriptBytes.begin(), scriptBytes.end());

    CoinbaseHeightPrefix parsed;
    BOOST_REQUIRE(ParseCoinbaseHeightPrefix(scriptSig, parsed));
    BOOST_CHECK_EQUAL(parsed.height, 954301U);
    BOOST_CHECK_EQUAL(parsed.encodedHeightSize, 3U);
    BOOST_CHECK_EQUAL(parsed.nextOffset, 4U);
    BOOST_REQUIRE_EQUAL(scriptSig.size() - parsed.nextOffset, 64U);
    BOOST_CHECK_EQUAL(scriptSig[parsed.nextOffset], 0x4f);
    BOOST_CHECK_EQUAL(scriptSig.back(), 0x25);
}

BOOST_AUTO_TEST_CASE(coinbase_height_prefix_enforces_bip34_encoding) {
    const auto parse = [](const std::string& hex,
                          CoinbaseHeightPrefix& parsed) {
        const std::vector<uint8_t> bytes = ParseHex(hex);
        const CScript script(bytes.begin(), bytes.end());
        return ParseCoinbaseHeightPrefix(script, parsed);
    };

    CoinbaseHeightPrefix parsed;
    BOOST_CHECK(!parse("", parsed));
    BOOST_CHECK(!parse("03bd8f", parsed));              // Truncated height.
    BOOST_CHECK(!parse("06000000000000", parsed));      // More than five bytes.
    BOOST_CHECK(!parse("020100", parsed));              // Non-minimal number.
    BOOST_CHECK(!parse("0101", parsed));                // Must use OP_1.
    BOOST_CHECK(!parse("0181", parsed));                // Negative height.
    BOOST_CHECK(!parse("4c03bd8f0e", parsed));          // Non-minimal push.

    BOOST_REQUIRE(parse("050000008000", parsed));
    BOOST_CHECK_EQUAL(parsed.height, 0x80000000ULL);
    BOOST_CHECK_EQUAL(parsed.nextOffset, 6U);
}

BOOST_AUTO_TEST_CASE(coinbase_height_prefix_ignores_signature_suffix) {
    std::vector<uint8_t> scriptBytes = ParseHex(
        "03bd8f0e"
        "4fabdda63ab134ac2d4c386d01abbf7dbec2824804afe22893604f9d1bc22edda"
        "9eb1ae46fb52498370bb4ac0ac92ea65eb1dbf58b6ca4c5302203f8ae7a8b25");
    scriptBytes.push_back(0x4c);
    scriptBytes.push_back(0xff);
    const CScript scriptSig(scriptBytes.begin(), scriptBytes.end());

    CoinbaseHeightPrefix parsed;
    BOOST_REQUIRE(ParseCoinbaseHeightPrefix(scriptSig, parsed));
    BOOST_CHECK_EQUAL(parsed.height, 954301U);
    BOOST_CHECK_EQUAL(parsed.nextOffset, 4U);
    BOOST_CHECK_EQUAL(scriptSig[parsed.nextOffset], 0x4f);
    BOOST_CHECK_EQUAL(scriptSig.size() - parsed.nextOffset, 66U);
}

BOOST_AUTO_TEST_CASE(provided_kycv2_manager_signature_vector) {
    const std::vector<uint8_t> managerMessage = ParseHex(
        "8eef133f3f9e5ed919cdee02f39f86d1644c6a53683a625cd95743b37911e924"
        "40420f"
        "23"
        "4b520000");
    const std::string expectedMessageHashHex =
        "0b05c36bccca482786a0e87e00afdcc4c27ffdb78bb33da68258981b0d9bbf31";
    const uint256 managerMessageHash =
        Hash(managerMessage.begin(), managerMessage.end());
    BOOST_CHECK_EQUAL(
        HexStr(managerMessageHash.begin(), managerMessageHash.end()),
        expectedMessageHashHex);

    const XOnlyPubKey aggregateManagerPubkey(ParseHex(
        "563c222009a10d447b625bcddd3adfafa7c0c629ea3961c49629f2a3d822309e"));
    const std::vector<uint8_t> aggregateSignature = ParseHex(
        "4e5c260a75dce151044c17702cb8edfd044f63d11c6ea2bbe6f8a299b1364b5d"
        "cf1e1631791e55c144f3233fc8c9c6527d086d9b7db9e96c2b8e2484a987bee0");

    BOOST_REQUIRE(aggregateManagerPubkey.IsFullyValid());
    BOOST_REQUIRE_EQUAL(aggregateSignature.size(), 64U);
    BOOST_CHECK(aggregateManagerPubkey.VerifySchnorr(
        managerMessageHash, aggregateSignature));
}

BOOST_AUTO_TEST_CASE(v1_invalid_inputs_fail_closed) {
    const auto rejects = [](const CMutableTransaction& mtx) {
        bool result = true;
        const CTransaction tx(mtx);
        BOOST_CHECK_NO_THROW(result = FilledMinerBill(tx));
        BOOST_CHECK(!result);
    };

    CMutableTransaction empty;
    rejects(empty);

    CMutableTransaction inputOnly;
    inputOnly.vin.emplace_back();
    rejects(inputOnly);

    CMutableTransaction outputOnly;
    outputOnly.vout.emplace_back(Amount(0), CScript());
    rejects(outputOnly);

    CMutableTransaction emptyScripts;
    emptyScripts.vin.emplace_back();
    emptyScripts.vout.emplace_back(Amount(0), CScript());
    rejects(emptyScripts);
}

BOOST_AUTO_TEST_SUITE_END()
