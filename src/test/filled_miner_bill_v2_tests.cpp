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
    std::vector<uint8_t> scriptPubKeyVec = ParseHex("76a914b3f89180086dfaa2ed10becff8f3b7051114fd0a88ac6a4c6d00139310fe388ffa6f3eb911966a60793f8536846febea92b3ee7c435bca61dcfbec34ff70aa12b8357ec488a1230bbc24a0bd4989aee41a4dc5e45126f57a155c2758caa0c766b00f824f2d93bb1a8a37324f45b8deb5e6626acafa08a2d3877b0340420f23");
    txout.scriptPubKey = CScript(scriptPubKeyVec.begin(), scriptPubKeyVec.end());
    mtx.vout.push_back(txout);

    CTxOut txout2;
    txout2.nValue = Amount(203129445);
    std::vector<uint8_t> scriptPubKeyVec2 = ParseHex("76a9143b453ad6954e9ebc28e4427e6052682bbe57cd7988ac");
    txout2.scriptPubKey = CScript(scriptPubKeyVec2.begin(), scriptPubKeyVec2.end());
    mtx.vout.push_back(txout2);
    return mtx;
}

// Valid cb with non-fixed charge address
BOOST_AUTO_TEST_CASE(valid_v2_cb_without_fixed_charge) {
    CMutableTransaction mtx = getValidMutableTransaction();
    CTransaction tx(mtx);
    BOOST_CHECK(FilledMinerBillV2(tx, uint256S("000000000946664ab39a9591cbb3066fe5569da6ae8529142998b9186a1e9639")));
}

// Valid cb with fixed charge address
BOOST_AUTO_TEST_CASE(valid_v2_cb_with_fixed_charge) {
    CMutableTransaction mtx = getValidMutableTransaction();
    std::vector<uint8_t> scriptPubKeyVec = ParseHex("76a914b3f89180086dfaa2ed10becff8f3b7051114fd0a88ac6a4c6d01139310fe388ffa6f3eb911966a60793f8536846febea92b3ee7c435bca61dcfb4712abee6b71711b110d667ec06dfcd828f1877b0e9b11b5b61875264dc8994f18bb8efc6262a5730a547658f4af7037b4bc2c3c7629e95b54bd37bd9ee463530340420f23");
    mtx.vout[0].scriptPubKey = CScript(scriptPubKeyVec.begin(), scriptPubKeyVec.end());

    CTransaction tx(mtx);
    BOOST_CHECK(FilledMinerBillV2(tx, uint256S("000000000946664ab39a9591cbb3066fe5569da6ae8529142998b9186a1e9639")));
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
BOOST_AUTO_TEST_SUITE_END()