#include "primitives/transaction.h"
#include "validation.cpp"
#include "test/test_bitcoin.h"
#include <boost/test/tools/old/interface.hpp>
#include <boost/test/unit_test.hpp>
#include <boost/test/unit_test_suite.hpp>
#include <cstdint>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(filled_miner_bill_v2_tests, BasicTestingSetup)

CMutableTransaction getValidFixedChargeMutableTransaction() {
    CMutableTransaction mtx;
    mtx.nVersion = 10;
    mtx.nLockTime = 0;

    CTxIn txin;
    txin.prevout = COutPoint(uint256S("0x0000000000000000000000000000000000000000000000000000000000000000"), 0xffffffff);
    std::vector<uint8_t> scriptSigVec = ParseHex("03""ea950e"       // bip34
        "fee40217d737e8d9d0972c9acd14990e5fe3c08e2f49fe8ea35b6fe0a1bc1e9720249867e509399122e677bb7d0f908496136992d01e59cc29e2897afd73eb89");    // miner sig
    txin.scriptSig = CScript(scriptSigVec.begin(), scriptSigVec.end());
    txin.nSequence = 0xffffffff;
    mtx.vin.push_back(txin);
    
    CTxOut txout;
    txout.nValue = Amount(109378697);
    std::vector<uint8_t> scriptPubKeyVec = ParseHex("76a914b3f89180086dfaa2ed10becff8f3b7051114fd0a88ac"    // P2PKH
        "6a"        // OP_RETURN
        "4c""6a"    // OP_PUSHDATA1
        "01"        // ckeck charge address flag
        "fba5750748a4c66465641157226a35688e0f1ebf99718208627b0cfdb50934a7"      // miner pubkey
        "3e2689f8c0cb585a20b735048b30cfe62f703f0b8ea2155ef71c8fdeaa53599d82f9a1ff6063a8585756d00cc31d08376ffba7ec339b790fca434b94986bee34"      // manager sig
        "03"        // kyc permission height length
        "900510"    // kyc permission height
        "23"        // charge rate
        "4b520000");// country code
    txout.scriptPubKey = CScript(scriptPubKeyVec.begin(), scriptPubKeyVec.end());
    mtx.vout.push_back(txout);

    CTxOut txout2;
    txout2.nValue = Amount(203131868);
    std::vector<uint8_t> scriptPubKeyVec2 = ParseHex("76a9143b453ad6954e9ebc28e4427e6052682bbe57cd7988ac");
    txout2.scriptPubKey = CScript(scriptPubKeyVec2.begin(), scriptPubKeyVec2.end());
    mtx.vout.push_back(txout2);
    return mtx;
}

BOOST_AUTO_TEST_CASE(valid_fixed_charge_v2_cb) {
    CMutableTransaction mtx = getValidFixedChargeMutableTransaction();
    CTransaction tx(mtx);
    BOOST_CHECK(FilledMinerBillV2(tx, uint256S("000000000e55dfab3a5c742c898242a0c883f0b130c3d06cf232ae257ed75d48")));
}

BOOST_AUTO_TEST_CASE(malformed_v2_coinbase_outputs_are_rejected) {
    const auto checkRejected = [this](CMutableTransaction mtx,
                                      const std::string& rejectReason) {
        CBlock block;
        block.hashPrevBlock = uint256S(
            "000000000e55dfab3a5c742c898242a0"
            "c883f0b130c3d06cf232ae257ed75d48");
        block.vtx.push_back(MakeTransactionRef(std::move(mtx)));

        CValidationState state;
        bool valid = true;
        const BlockValidationOptions validationOptions(false, false);
        BOOST_CHECK_NO_THROW(
            valid = CheckBlock(
                testConfig, block, state, 927000, validationOptions));
        BOOST_CHECK(!valid);
        BOOST_CHECK(state.IsInvalid());
        BOOST_CHECK_EQUAL(state.GetRejectReason(), rejectReason);
    };

    CMutableTransaction negativeOutput =
        getValidFixedChargeMutableTransaction();
    negativeOutput.vout[1].nValue = Amount(-1);
    checkRejected(
        std::move(negativeOutput), "bad-txns-vout-negative");

    CMutableTransaction oversizedOutput =
        getValidFixedChargeMutableTransaction();
    oversizedOutput.vout[1].nValue = MAX_MONEY + Amount(1);
    checkRejected(
        std::move(oversizedOutput), "bad-txns-vout-toolarge");

    CMutableTransaction oversizedTotal =
        getValidFixedChargeMutableTransaction();
    oversizedTotal.vout[0].nValue = MAX_MONEY;
    oversizedTotal.vout[1].nValue = Amount(1);
    checkRejected(
        std::move(oversizedTotal), "bad-txns-txouttotal-toolarge");
}

BOOST_AUTO_TEST_CASE(canonical_v2_script_parser_accepts_expected_template) {
    const CMutableTransaction mtx = getValidFixedChargeMutableTransaction();
    MinerBillV2ScriptData parsed;

    BOOST_REQUIRE(ParseCanonicalMinerBillV2Script(
        mtx.vout[0].scriptPubKey, parsed));
    BOOST_CHECK(parsed.isFixedChangeAddress);
    BOOST_CHECK_EQUAL(parsed.chargeAddressPubkeyHash.size(), 20U);
    BOOST_CHECK_EQUAL(parsed.pubkeyMiner.size(), 32U);
    BOOST_CHECK_EQUAL(parsed.sigManager.size(), 64U);
    BOOST_CHECK_EQUAL(parsed.permissionHeight.size(), 3U);
    BOOST_CHECK_EQUAL(parsed.chargeRate.size(), 1U);
    BOOST_CHECK_EQUAL(parsed.countryCode.size(), 4U);
    BOOST_CHECK_EQUAL(
        HexStr(
            parsed.chargeAddressPubkeyHash.begin(),
            parsed.chargeAddressPubkeyHash.end()),
        "b3f89180086dfaa2ed10becff8f3b7051114fd0a");
    BOOST_CHECK_EQUAL(
        HexStr(parsed.pubkeyMiner.begin(), parsed.pubkeyMiner.end()),
        "fba5750748a4c66465641157226a3568"
        "8e0f1ebf99718208627b0cfdb50934a7");
    BOOST_CHECK_EQUAL(
        HexStr(parsed.sigManager.begin(), parsed.sigManager.end()),
        "3e2689f8c0cb585a20b735048b30cfe6"
        "2f703f0b8ea2155ef71c8fdeaa53599d"
        "82f9a1ff6063a8585756d00cc31d0837"
        "6ffba7ec339b790fca434b94986bee34");
    BOOST_CHECK_EQUAL(
        HexStr(
            parsed.permissionHeight.begin(),
            parsed.permissionHeight.end()),
        "900510");
    BOOST_CHECK_EQUAL(
        HexStr(parsed.chargeRate.begin(), parsed.chargeRate.end()),
        "23");
    BOOST_CHECK_EQUAL(
        HexStr(parsed.countryCode.begin(), parsed.countryCode.end()),
        "4b520000");
}

BOOST_AUTO_TEST_CASE(canonical_v2_script_parser_rejects_executable_bypass) {
    CScript script = getValidFixedChargeMutableTransaction().vout[0].scriptPubKey;
    script[0] = OP_1;
    script[1] = OP_RETURN;

    MinerBillV2ScriptData parsed;
    BOOST_CHECK(!ParseCanonicalMinerBillV2Script(script, parsed));
}

BOOST_AUTO_TEST_CASE(canonical_v2_script_parser_rejects_noncanonical_forms) {
    const CScript canonical =
        getValidFixedChargeMutableTransaction().vout[0].scriptPubKey;
    MinerBillV2ScriptData parsed;

    const std::vector<size_t> opcodeOffsets{0, 1, 2, 23, 24, 25, 26};
    for (const size_t offset : opcodeOffsets) {
        CScript mutated = canonical;
        mutated[offset] = OP_NOP;
        BOOST_CHECK(!ParseCanonicalMinerBillV2Script(mutated, parsed));
    }

    CScript wrongPayloadLength = canonical;
    --wrongPayloadLength[27];
    BOOST_CHECK(!ParseCanonicalMinerBillV2Script(
        wrongPayloadLength, parsed));

    CScript invalidFixedFlag = canonical;
    invalidFixedFlag[28] = 2;
    BOOST_CHECK(!ParseCanonicalMinerBillV2Script(invalidFixedFlag, parsed));

    CScript invalidPermissionHeightLength = canonical;
    invalidPermissionHeightLength[125] = 9;
    BOOST_CHECK(!ParseCanonicalMinerBillV2Script(
        invalidPermissionHeightLength, parsed));

    CScript trailingData = canonical;
    trailingData.push_back(OP_0);
    BOOST_CHECK(!ParseCanonicalMinerBillV2Script(trailingData, parsed));

    CScript truncated = canonical;
    truncated.pop_back();
    BOOST_CHECK(!ParseCanonicalMinerBillV2Script(truncated, parsed));
}

// BOOST_AUTO_TEST_CASE(coinbase_kyc_version_uses_candidate_block_height) {
//     const CTransaction tx(getValidFixedChargeMutableTransaction());
//     const uint256 previousBlockHash = uint256S(
//         "000000000946664ab39a9591cbb3066f"
//         "e5569da6ae8529142998b9186a1e9639");

//     CValidationState v2State;
//     BOOST_CHECK(!CheckCoinbase(
//         tx, v2State, MAX_TX_SIGOPS_COUNT_BEFORE_GENESIS,
//         MAX_TX_SIZE_CONSENSUS_BEFORE_GENESIS, true,
//         previousBlockHash, 927000));
//     BOOST_CHECK_EQUAL(
//         v2State.GetRejectReason(), "bad-miner-bill-v2");

//     CValidationState v1State;
//     BOOST_CHECK(!CheckCoinbase(
//         tx, v1State, MAX_TX_SIGOPS_COUNT_BEFORE_GENESIS,
//         MAX_TX_SIZE_CONSENSUS_BEFORE_GENESIS, true,
//         previousBlockHash, 926999));
//     BOOST_CHECK_EQUAL(v1State.GetRejectReason(), "bad-miner-bill");

// }

// Invalid pre block hash
BOOST_AUTO_TEST_CASE(invalid_v2_cb_wrong_pre_block_hash) {
    CMutableTransaction mtx = getValidFixedChargeMutableTransaction();
    CTransaction tx(mtx);
    BOOST_CHECK(!FilledMinerBillV2(tx, uint256S("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff")));
}

// Permission height overdue
BOOST_AUTO_TEST_CASE(invalid_v2_cb_permission_height_overdue) {
    CMutableTransaction mtx = getValidFixedChargeMutableTransaction();
    std::vector<uint8_t> scriptSigVecNew = ParseHex("03""910510"       // 1050001
        "fee40217d737e8d9d0972c9acd14990e5fe3c08e2f49fe8ea35b6fe0a1bc1e9720249867e509399122e677bb7d0f908496136992d01e59cc29e2897afd73eb89");    // miner sig
    mtx.vin[0].scriptSig = CScript(scriptSigVecNew.begin(), scriptSigVecNew.end());
    CTransaction tx_permission_height_overdue(mtx);
    BOOST_CHECK(!FilledMinerBillV2(tx_permission_height_overdue, uint256S("000000000e55dfab3a5c742c898242a0c883f0b130c3d06cf232ae257ed75d48")));
}

// Don't meet charge rate
BOOST_AUTO_TEST_CASE(invalid_v2_cb_dont_meet_charge_rate) {
    CMutableTransaction mtx = getValidFixedChargeMutableTransaction();
    mtx.vout[0].nValue = Amount(100000000);
    mtx.vout[1].nValue = Amount(1000000000);
    CTransaction tx(mtx);
    BOOST_CHECK(!FilledMinerBillV2(tx, uint256S("000000000e55dfab3a5c742c898242a0c883f0b130c3d06cf232ae257ed75d48")));
}

// Charge address doesn't match ask
BOOST_AUTO_TEST_CASE(invalid_v2_cb_charge_address_doesnt_match_ask) {
    CMutableTransaction mtx = getValidFixedChargeMutableTransaction();
    std::vector<uint8_t> scriptPubKeyVec = ParseHex("76a9143b453ad6954e9ebc28e4427e6052682bbe57cd7988ac"    // P2PKH
        "6a"        // OP_RETURN
        "4c""6a"    // OP_PUSHDATA1
        "01"        // ckeck charge address flag
        "fba5750748a4c66465641157226a35688e0f1ebf99718208627b0cfdb50934a7"      // miner pubkey
        "3e2689f8c0cb585a20b735048b30cfe62f703f0b8ea2155ef71c8fdeaa53599d82f9a1ff6063a8585756d00cc31d08376ffba7ec339b790fca434b94986bee34"      // manager sig
        "03"        // kyc permission height length
        "900510"    // kyc permission height
        "23"        // charge rate
        "4b520000");// country code
    mtx.vout[0].scriptPubKey = CScript(scriptPubKeyVec.begin(), scriptPubKeyVec.end());
    CTransaction tx(mtx);
    BOOST_CHECK(!FilledMinerBillV2(tx, uint256S("000000000e55dfab3a5c742c898242a0c883f0b130c3d06cf232ae257ed75d48")));
}

// Invalid miner sig
BOOST_AUTO_TEST_CASE(invalid_v2_cb_invalid_miner_sig) {
    // GetLogger().fPrintToConsole = true;
    CMutableTransaction mtx = getValidFixedChargeMutableTransaction();
    std::vector<uint8_t> scriptVec = ParseHex("03""ea950e"
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    mtx.vin[0].scriptSig = CScript(scriptVec.begin(), scriptVec.end());
    CTransaction tx(mtx);
    BOOST_CHECK(!FilledMinerBillV2(tx, uint256S("000000000e55dfab3a5c742c898242a0c883f0b130c3d06cf232ae257ed75d48")));
}

// Invalid manager sig
BOOST_AUTO_TEST_CASE(invalid_v2_cb_invalid_manager_sig) {
    // GetLogger().fPrintToConsole = true;
    CMutableTransaction mtx = getValidFixedChargeMutableTransaction();
    std::vector<uint8_t> scriptPubKeyVec = ParseHex("76a9143b453ad6954e9ebc28e4427e6052682bbe57cd7988ac"    // P2PKH
        "6a"        // OP_RETURN
        "4c""6a"    // OP_PUSHDATA1
        "01"        // ckeck charge address flag
        "fba5750748a4c66465641157226a35688e0f1ebf99718208627b0cfdb50934a7"      // miner pubkey
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"      // manager sig
        "03"        // kyc permission height length
        "900510"    // kyc permission height
        "23"        // charge rate
        "4b520000");// country code
    mtx.vout[0].scriptPubKey = CScript(scriptPubKeyVec.begin(), scriptPubKeyVec.end());
    CTransaction tx(mtx);
    BOOST_CHECK(!FilledMinerBillV2(tx, uint256S("000000000e55dfab3a5c742c898242a0c883f0b130c3d06cf232ae257ed75d48")));
}

BOOST_AUTO_TEST_CASE(coinbase_height_prefix_preserves_raw_miner_signature) {
    std::vector<uint8_t> scriptBytes = ParseHex("03""ea950e"       // bip34
        "fee40217d737e8d9d0972c9acd14990e5fe3c08e2f49fe8ea35b6fe0a1bc1e9720249867e509399122e677bb7d0f908496136992d01e59cc29e2897afd73eb89");    // miner sig
    const CScript scriptSig(scriptBytes.begin(), scriptBytes.end());

    CoinbaseHeightPrefix parsed;
    BOOST_REQUIRE(ParseCoinbaseHeightPrefix(scriptSig, parsed));
    BOOST_CHECK_EQUAL(parsed.height, 955882U);
    BOOST_CHECK_EQUAL(parsed.encodedHeightSize, 3U);
    BOOST_CHECK_EQUAL(parsed.nextOffset, 4U);
    BOOST_REQUIRE_EQUAL(scriptSig.size() - parsed.nextOffset, 64U);
    BOOST_CHECK_EQUAL(scriptSig[parsed.nextOffset], 0xfe);
    BOOST_CHECK_EQUAL(scriptSig.back(), 0x89);
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
