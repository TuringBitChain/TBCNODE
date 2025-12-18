#include "uint256.h"
#include "utilstrencodings.h"
#include "x_only_pubkey.h"
#include "test/test_bitcoin.h"

#include <boost/test/tools/old/interface.hpp>
#include <boost/test/unit_test.hpp>
#include <cstdint>
#include <string>
#include <sys/types.h>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(x_only_pubkey_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(test_xonlypubkey_isvalid) {
    XOnlyPubKey pubkey;
    BOOST_CHECK(!pubkey.IsValid());
}

BOOST_AUTO_TEST_CASE(test_xonlypubkey_isfullyvalid) {
    // Test with an invalid x-only pubkey (all zeros - this should fail IsValid)
    XOnlyPubKey pubkeyInvalid;
    BOOST_CHECK(!pubkeyInvalid.IsFullyValid());

    // Use a 30 bytes wrong pubkey
    string pubkeyShortStr = "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e";
    std::vector<uint8_t> pubkeyShortVec = ParseHex(pubkeyShortStr);
    XOnlyPubKey pubkeyShort(pubkeyShortVec);
    BOOST_CHECK(!pubkeyShort.IsFullyValid());

    // Use a known valid x-only pubkey for secp256k1
    string validXonlyStr = "da8e35245f85e40e9ca309ccac9d5dadc7c6b4a9c2b260b858f5c3100ccf7b3d";
    std::vector<uint8_t> validXonlyVec = ParseHex(validXonlyStr);
    XOnlyPubKey pubkeyValid(validXonlyVec);
    BOOST_CHECK(pubkeyValid.IsFullyValid());
}

// Test with a already known valid x-only pubkey and signature.
BOOST_AUTO_TEST_CASE(test_xonlypubkey_verifyschnorr_valid) {    
    string pubkeyValidStr = "56e6a101555d001886174f07911419a663d0c55fad2b274e9693bf408ac4b3b3";
    string msgHashValidStr = "2b18447dcef8cec8f1b265151c8c41b9b45d2050620b43dbd33bd19f1b52518f";
    string signatureValidStr = "15c6e1db8fac01783eb74753caeb22a6c8753a3e260f2bbd8170947bccf7652cb022fcca4e83601083a8bf408bfbceb0f8188cd023f3d34f708bb15af15653c2";
    XOnlyPubKey pubkeyValid(ParseHex(pubkeyValidStr));
    std::vector<uint8_t> sigValid = ParseHex(signatureValidStr);
    uint256 msgHashValid = uint256(ParseHex(msgHashValidStr));
    BOOST_CHECK(pubkeyValid.VerifySchnorr(msgHashValid, sigValid));
}

BOOST_AUTO_TEST_CASE(test_xonlypubkey_verifyschnorr_invalid_sig) {
    string pubkeyValidStr = "56e6a101555d001886174f07911419a663d0c55fad2b274e9693bf408ac4b3b3";
    string msgHashValidStr = "2b18447dcef8cec8f1b265151c8c41b9b45d2050620b43dbd33bd19f1b52518f";
    string signatureValidStr = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    XOnlyPubKey pubkeyValid(ParseHex(pubkeyValidStr));
    std::vector<uint8_t> sigValid = ParseHex(signatureValidStr);
    uint256 msgHashValid = uint256(ParseHex(msgHashValidStr));
    BOOST_CHECK(!pubkeyValid.VerifySchnorr(msgHashValid, sigValid));
}

BOOST_AUTO_TEST_CASE(test_xonlypubkey_verifyschnorr_invalid_msg) {
    string pubkeyValidStr = "56e6a101555d001886174f07911419a663d0c55fad2b274e9693bf408ac4b3b3";
    string msgHashValidStr = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    string signatureValidStr = "15c6e1db8fac01783eb74753caeb22a6c8753a3e260f2bbd8170947bccf7652cb022fcca4e83601083a8bf408bfbceb0f8188cd023f3d34f708bb15af15653c2";
    XOnlyPubKey pubkeyValid(ParseHex(pubkeyValidStr));
    std::vector<uint8_t> sigValid = ParseHex(signatureValidStr);
    uint256 msgHashValid = uint256(ParseHex(msgHashValidStr));
    BOOST_CHECK(!pubkeyValid.VerifySchnorr(msgHashValid, sigValid));
}

BOOST_AUTO_TEST_SUITE_END()