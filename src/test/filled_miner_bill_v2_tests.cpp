#include "primitives/transaction.h"
#include "validation.cpp"
#include "test/test_bitcoin.h"
#include <boost/test/tools/old/interface.hpp>
#include <boost/test/unit_test.hpp>
#include <boost/test/unit_test_suite.hpp>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(filled_miner_bill_v2, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(valid_v2_cb_without_fixed_charge) {
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
    std::vector<uint8_t> scriptPubKeyVec = ParseHex("76a914b3f89180086dfaa2ed10becff8f3b7051114fd0a88ac6a4c6d00139310fe388ffa6f3eb911966a60793f8536846febea92b3ee7c435bca61dcfb ec34ff70aa12b8357ec488a1230bbc24a0bd4989aee41a4dc5e45126f57a155c2758caa0c766b00f824f2d93bb1a8a37324f45b8deb5e6626acafa08a2d3877b0340420f23");
    txout.scriptPubKey = CScript(scriptPubKeyVec.begin(), scriptPubKeyVec.end());
    mtx.vout.push_back(txout);

    CTxOut txout2;
    txout2.nValue = Amount(203129445);
    std::vector<uint8_t> scriptPubKeyVec2 = ParseHex("76a9143b453ad6954e9ebc28e4427e6052682bbe57cd7988ac");
    txout2.scriptPubKey = CScript(scriptPubKeyVec2.begin(), scriptPubKeyVec2.end());
    mtx.vout.push_back(txout2);

    CTransaction tx(mtx);
    std::cout << "tx: " << tx.ToString() << endl;
    BOOST_CHECK(FilledMinerBillV2(tx, uint256S("000000000946664ab39a9591cbb3066fe5569da6ae8529142998b9186a1e9639")));
}

BOOST_AUTO_TEST_CASE(valid_v2_cb_with_fixed_charge) {
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
    // std::vector<uint8_t> scriptPubKeyVec = ParseHex("76a914b3f89180086dfaa2ed10becff8f3b7051114fd0a88ac 6a 4c 6d 01 139310fe388ffa6f3eb911966a60793f8536846febea92b3ee7c435bca61dcfb 4712abee6b71711b110d667ec06dfcd828f1877b0e9b11b5b61875264dc8994f18bb8efc6262a5730a547658f4af7037b4bc2c3c7629e95b54bd37bd9ee46353 03 40420f 23");
    std::vector<uint8_t> scriptPubKeyVec = ParseHex("76a914b3f89180086dfaa2ed10becff8f3b7051114fd0a88ac6a4c6d01139310fe388ffa6f3eb911966a60793f8536846febea92b3ee7c435bca61dcfb4712abee6b71711b110d667ec06dfcd828f1877b0e9b11b5b61875264dc8994f18bb8efc6262a5730a547658f4af7037b4bc2c3c7629e95b54bd37bd9ee463530340420f23");
    txout.scriptPubKey = CScript(scriptPubKeyVec.begin(), scriptPubKeyVec.end());
    mtx.vout.push_back(txout);

    CTxOut txout2;
    txout2.nValue = Amount(203129445);
    std::vector<uint8_t> scriptPubKeyVec2 = ParseHex("76a9143b453ad6954e9ebc28e4427e6052682bbe57cd7988ac");
    txout2.scriptPubKey = CScript(scriptPubKeyVec2.begin(), scriptPubKeyVec2.end());
    mtx.vout.push_back(txout2);

    CTransaction tx(mtx);
    std::cout << "tx: " << tx.ToString() << endl;
    BOOST_CHECK(FilledMinerBillV2(tx, uint256S("000000000946664ab39a9591cbb3066fe5569da6ae8529142998b9186a1e9639")));
}

BOOST_AUTO_TEST_SUITE_END()