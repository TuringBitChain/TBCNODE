// Copyright (c) 2026 The TBC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "config.h"
#include "consensus/validation.h"
#include "crypto/common.h"
#include "crypto/sha256.h"
#include "key.h"
#include "net/net.h"
#include "primitives/transaction.h"
#include "script/interpreter.h"
#include "script/script.h"
#include "script/sighashtype.h"
#include "test/test_bitcoin.h"
#include "token_protection.h"
#include "txmempool.h"
#include "txn_double_spend_detector.h"
#include "txn_validator.h"
#include "uint256.h"
#include "utilstrencodings.h"
#include "validation.h"

#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <string>

#include <boost/test/unit_test.hpp>

using namespace token_protection;

namespace {

const std::vector<uint8_t> FTAPE_FLAG = {0x46, 0x54, 0x61, 0x70, 0x65};
const std::vector<uint8_t> NTAPE_FLAG = {0x4e, 0x54, 0x61, 0x70, 0x65};

std::vector<uint8_t> EncodeLE64Slots(const std::vector<int64_t>& values) {
    std::vector<uint8_t> data(values.size() * 8, 0);
    for (size_t i = 0; i < values.size(); ++i) {
        WriteLE64(data.data() + i * 8, static_cast<uint64_t>(values[i]));
    }
    return data;
}

std::vector<uint8_t> MakeFTapeAmountField(std::array<int64_t, 6> amounts) {
    return EncodeLE64Slots(
        std::vector<int64_t>(amounts.begin(), amounts.end()));
}

CScript MakeFTapeScript(std::array<int64_t, 6> amounts) {
    // Matches tape_style.md: OP_FALSE OP_RETURN <48B amount> ... "FTape"
    return CScript() << OP_FALSE << OP_RETURN << MakeFTapeAmountField(amounts)
                     << int64_t{6} << std::vector<uint8_t>{'T', 'S', 'T'}
                     << std::vector<uint8_t>{'T', 'S', 'T'} << FTAPE_FLAG;
}

//! poolnft layout: OP_FALSE OP_RETURN <64B> <24B amount> <32B> ... "NTape"
CScript MakePoolNftTapeScript(std::array<int64_t, 3> amounts,
                              const std::vector<uint8_t>& hashPrefix = {}) {
    std::vector<uint8_t> hash64(64, 0x11);
    if (!hashPrefix.empty()) {
        BOOST_REQUIRE(hashPrefix.size() <= hash64.size());
        std::copy(hashPrefix.begin(), hashPrefix.end(), hash64.begin());
    }
    std::vector<uint8_t> amount24 = EncodeLE64Slots(
        std::vector<int64_t>(amounts.begin(), amounts.end()));
    std::vector<uint8_t> txid32(32, 0x22);
    return CScript() << OP_FALSE << OP_RETURN << hash64 << amount24 << txid32
                     << int64_t{1} << int64_t{0} << int64_t{0} << int64_t{0}
                     << NTAPE_FLAG;
}

//! Shares the NTape flag but is not a poolnft layout (e.g. a plain NFT JSON tape)
CScript MakeNonPoolNftNTapeScript() {
    return CScript() << OP_FALSE << OP_RETURN
                     << std::vector<uint8_t>{'{', '}'} << NTAPE_FLAG;
}

CScript MakeContractScript(uint8_t tag) {
    return CScript() << std::vector<uint8_t>{tag, tag, tag} << OP_DROP
                     << OP_TRUE;
}

CTransactionRef MakePrevTxWithFTape(const CScript& contractScript,
                                    std::array<int64_t, 6> amounts) {
    CMutableTransaction prev;
    prev.nVersion = 1;
    prev.vin.resize(1);
    prev.vin[0].prevout = COutPoint();
    prev.vin[0].scriptSig = CScript() << OP_0;
    prev.vout.resize(2);
    prev.vout[0].nValue = Amount(1);
    prev.vout[0].scriptPubKey = contractScript;
    prev.vout[1].nValue = Amount(0);
    prev.vout[1].scriptPubKey = MakeFTapeScript(amounts);
    return MakeTransactionRef(prev);
}

CTransactionRef MakePrevTxWithPoolNftTape(
    std::array<int64_t, 3> amounts,
    const std::vector<uint8_t>& hashPrefix = {}) {
    CMutableTransaction prev;
    prev.nVersion = 1;
    prev.vin.resize(1);
    prev.vin[0].prevout = COutPoint();
    prev.vin[0].scriptSig = CScript() << OP_0;
    prev.vout.resize(2);
    prev.vout[0].nValue = Amount(1);
    prev.vout[0].scriptPubKey = MakeContractScript(0x01);
    prev.vout[1].nValue = Amount(0);
    prev.vout[1].scriptPubKey = MakePoolNftTapeScript(amounts, hashPrefix);
    return MakeTransactionRef(prev);
}

PrevTxFetcher MakeFetcher(const std::map<TxId, CTransactionRef>& prevById) {
    return [prevById](const TxId& txid) -> CTransactionRef {
        const auto it = prevById.find(txid);
        if (it == prevById.end()) {
            return nullptr;
        }
        return it->second;
    };
}

CScript GetP2PK(const CKey& key) {
    return CScript() << ToByteVector(key.GetPubKey()) << OP_CHECKSIG;
}

void SignP2PKInput(CMutableTransaction& tx, size_t inputIndex,
                   const CTransaction& fundTx, const CKey& key) {
    const uint32_t n = tx.vin[inputIndex].prevout.GetN();
    const CScript& scriptPubKey = fundTx.vout[n].scriptPubKey;
    std::vector<uint8_t> vchSig;
    const uint256 hash = SignatureHash(scriptPubKey, CTransaction(tx),
                                       inputIndex, SigHashType().withForkId(),
                                       fundTx.vout[n].nValue);
    BOOST_REQUIRE(key.Sign(hash, vchSig));
    vchSig.push_back(uint8_t(SIGHASH_ALL | SIGHASH_FORKID));
    tx.vin[inputIndex].scriptSig = CScript() << vchSig;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(token_protection_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(ftape_ntape_pattern_match) {
    const CScript ftape = MakeFTapeScript({1, 0, 0, 0, 0, 0});
    const CScript poolNft = MakePoolNftTapeScript({1, 2, 3});
    const CScript plainNft = MakeNonPoolNftNTapeScript();
    const CScript ordinary = CScript() << OP_TRUE;

    BOOST_CHECK(IsFTapeOutput(ftape));
    BOOST_CHECK(!IsNTapeOutput(ftape));
    BOOST_CHECK(IsNTapeOutput(poolNft));
    BOOST_CHECK(!IsFTapeOutput(poolNft));
    BOOST_CHECK(IsNTapeOutput(plainNft));
    BOOST_CHECK(!IsFTapeOutput(ordinary));
    BOOST_CHECK(!IsNTapeOutput(ordinary));

    // Real FT tape sample (devfiles/tape_style.md)
    const auto realFtapeHex = ParseHex(
        "006a300010a5d4e80000000000000000000000000000000000000000000000000000"
        "000000000000000000000000000000000001060354535403545354054654617065");
    const CScript realFtape(realFtapeHex.begin(), realFtapeHex.end());
    BOOST_CHECK(IsFTapeOutput(realFtape));

    // Missing flag / prefix is not OP_FALSE OP_RETURN
    BOOST_CHECK(!IsFTapeOutput(CScript() << OP_FALSE << OP_RETURN
                                         << MakeFTapeAmountField({1, 0, 0, 0, 0, 0})));
    BOOST_CHECK(!IsFTapeOutput(CScript() << OP_1 << OP_RETURN
                                         << MakeFTapeAmountField({1, 0, 0, 0, 0, 0})
                                         << FTAPE_FLAG));
}

BOOST_AUTO_TEST_CASE(parse_tape_amounts_non_negative_and_boundaries) {
    {
        const auto amounts =
            ParseTapeAmounts(MakeFTapeScript({100, 0, 0, 0, 0, 0}));
        BOOST_REQUIRE(amounts);
        BOOST_REQUIRE_EQUAL(amounts->size(), 6U);
        BOOST_CHECK_EQUAL((*amounts)[0], 100);
        BOOST_CHECK_EQUAL((*amounts)[1], 0);
    }

    // Boundaries: all zeros / max value in a single slot
    {
        const auto zero = ParseTapeAmounts(MakeFTapeScript({0, 0, 0, 0, 0, 0}));
        BOOST_REQUIRE(zero);
        for (const int64_t v : *zero) {
            BOOST_CHECK_EQUAL(v, 0);
        }

        const auto maxSlot = ParseTapeAmounts(
            MakeFTapeScript({std::numeric_limits<int64_t>::max(), 0, 0, 0, 0, 0}));
        BOOST_REQUIRE(maxSlot);
        BOOST_CHECK_EQUAL((*maxSlot)[0], std::numeric_limits<int64_t>::max());
    }

    // Any negative slot -> nullopt
    BOOST_CHECK(!ParseTapeAmounts(MakeFTapeScript({-1, 0, 0, 0, 0, 0})));
    BOOST_CHECK(!ParseTapeAmounts(MakeFTapeScript({0, 0, 0, 0, 0, -5})));

    // First push shorter than 48 bytes
    {
        const CScript shortTape =
            CScript() << OP_FALSE << OP_RETURN << std::vector<uint8_t>(47, 0x00)
                      << FTAPE_FLAG;
        BOOST_CHECK(!ParseTapeAmounts(shortTape));
    }

    // Real sample: first slot is 1000000000000
    {
        const auto realFtapeHex = ParseHex(
        "006a300010a5d4e80000000000000000000000000000000000000000000000000000"
            "000000000000000000000000000000000001060354535403545354054654617065");
        const CScript realFtape(realFtapeHex.begin(), realFtapeHex.end());
        const auto amounts = ParseTapeAmounts(realFtape);
        BOOST_REQUIRE(amounts);
        BOOST_CHECK_EQUAL((*amounts)[0], 1000000000000LL);
    }
}

BOOST_AUTO_TEST_CASE(compute_token_partial_hash) {
    const CScript a = MakeContractScript(0x01);
    const CScript b = MakeContractScript(0x02);
    BOOST_CHECK(ComputeTokenPartialHash(a) != ComputeTokenPartialHash(b));
    BOOST_CHECK(ComputeTokenPartialHash(a) == ComputeTokenPartialHash(a));

    // SHA256 after stripping the fixed 29-byte suffix (OP_RETURN + 21B push + 5B push)
    {
        const CScript prefix =
            CScript() << std::vector<uint8_t>{0xab, 0xcd} << OP_DROP << OP_TRUE;
        const std::vector<uint8_t> push21(21, 0xc3);
        const std::vector<uint8_t> push5 = {0x32, 0x43, 0x6f, 0x64, 0x65}; // "2Code"
        const CScript withSuffix =
            CScript(prefix) << OP_RETURN << push21 << push5;
        BOOST_REQUIRE_EQUAL(withSuffix.size() - prefix.size(), 29U);

        uint256 expected;
        CSHA256()
            .Write(prefix.data(), prefix.size())
            .Finalize(expected.begin());
        BOOST_CHECK(ComputeTokenPartialHash(withSuffix) == expected);

        // Different suffix content with the same length -> partial_hash unchanged
        const std::vector<uint8_t> push21b(21, 0x11);
        const std::vector<uint8_t> push5b = {0x33, 0x43, 0x6f, 0x64, 0x65};
        const CScript withOtherSuffix =
            CScript(prefix) << OP_RETURN << push21b << push5b;
        BOOST_CHECK(ComputeTokenPartialHash(withOtherSuffix) == expected);
    }
}

BOOST_AUTO_TEST_CASE(amount_conservation_mint_and_transfer) {
    const CScript contract = MakeContractScript(0x0a);
    const auto prevTx = MakePrevTxWithFTape(contract, {100, 0, 0, 0, 0, 0});
    const auto fetcher = MakeFetcher({{prevTx->GetId(), prevTx}});

    // mint: outputs carry a new token with no matching input token -> passes
    {
        CMutableTransaction dummyPrev;
        dummyPrev.vin.resize(1);
        dummyPrev.vout.resize(1);
        dummyPrev.vout[0].scriptPubKey = MakeContractScript(0xcc);
        const auto dummyRef = MakeTransactionRef(dummyPrev);

        CMutableTransaction mint;
        mint.vin.resize(1);
        mint.vin[0].prevout = COutPoint(dummyRef->GetId(), 0);
        mint.vout.resize(2);
        mint.vout[0].scriptPubKey = MakeContractScript(0xbb);
        mint.vout[1].scriptPubKey = MakeFTapeScript({50, 0, 0, 0, 0, 0});
        BOOST_CHECK(CheckTokenAmountConservation(
                        CTransaction(mint),
                        MakeFetcher({{dummyRef->GetId(), dummyRef}})) ==
                    TokenCheckResult::OK);
    }

    // transfer conservation: input 100 -> output 100
    {
        CMutableTransaction transfer;
        transfer.vin.resize(1);
        transfer.vin[0].prevout = COutPoint(prevTx->GetId(), 0);
        transfer.vout.resize(2);
        transfer.vout[0].scriptPubKey = contract;
        transfer.vout[1].scriptPubKey = MakeFTapeScript({100, 0, 0, 0, 0, 0});
        BOOST_CHECK(CheckTokenAmountConservation(CTransaction(transfer),
                                                 fetcher) ==
                    TokenCheckResult::OK);
    }

    // transfer violation: input 100 -> output 99
    {
        CMutableTransaction mismatch;
        mismatch.vin.resize(1);
        mismatch.vin[0].prevout = COutPoint(prevTx->GetId(), 0);
        mismatch.vout.resize(2);
        mismatch.vout[0].scriptPubKey = contract;
        mismatch.vout[1].scriptPubKey = MakeFTapeScript({99, 0, 0, 0, 0, 0});
        BOOST_CHECK(CheckTokenAmountConservation(CTransaction(mismatch),
                                                 fetcher) ==
                    TokenCheckResult::AMOUNT_MISMATCH);
    }

    // Negative amount in an output
    {
        CMutableTransaction neg;
        neg.vout.resize(2);
        neg.vout[0].scriptPubKey = contract;
        neg.vout[1].scriptPubKey = MakeFTapeScript({-1, 0, 0, 0, 0, 0});
        BOOST_CHECK(CheckTokenAmountConservation(CTransaction(neg),
                                                 MakeFetcher({})) ==
                    TokenCheckResult::NEGATIVE_AMOUNT);
    }

    // Input-side tape needed but the previous transaction is unavailable
    {
        CMutableTransaction missingPrev;
        missingPrev.vin.resize(1);
        missingPrev.vin[0].prevout = COutPoint(uint256S("0xdead"), 0);
        missingPrev.vout.resize(2);
        missingPrev.vout[0].scriptPubKey = contract;
        missingPrev.vout[1].scriptPubKey = MakeFTapeScript({1, 0, 0, 0, 0, 0});
        BOOST_CHECK(CheckTokenAmountConservation(CTransaction(missingPrev),
                                                 MakeFetcher({})) ==
                    TokenCheckResult::PREV_TX_UNAVAILABLE);
    }

    // No FTape output -> skipped
    {
        CMutableTransaction plain;
        plain.vout.resize(1);
        plain.vout[0].scriptPubKey = MakeContractScript(0x01);
        BOOST_CHECK(CheckTokenAmountConservation(CTransaction(plain),
                                                 MakeFetcher({})) ==
                    TokenCheckResult::OK);
    }
}

BOOST_AUTO_TEST_CASE(poolnft_tape_tamper_checks) {
    const auto oldAmounts = std::array<int64_t, 3>{10, 20, 30};
    const auto prevTx =
        MakePrevTxWithPoolNftTape(oldAmounts, {/*hashPrefix*/ {0x01}});
    const auto fetcher = MakeFetcher({{prevTx->GetId(), prevTx}});

    // Not a poolnft layout (plain NTape / FTape) -> skipped
    {
        CMutableTransaction tx;
        tx.vout.resize(2);
        tx.vout[1].scriptPubKey = MakeFTapeScript({1, 0, 0, 0, 0, 0});
        BOOST_CHECK(CheckPoolNftTape(CTransaction(tx), fetcher) ==
                    TokenCheckResult::OK);

        tx.vout[1].scriptPubKey = MakeNonPoolNftNTapeScript();
        BOOST_CHECK(CheckPoolNftTape(CTransaction(tx), fetcher) ==
                    TokenCheckResult::OK);
    }

    // Amount may change while other fields stay the same -> passes
    {
        CMutableTransaction tx;
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint(prevTx->GetId(), 0);
        tx.vout.resize(2);
        tx.vout[0].scriptPubKey = MakeContractScript(0x01);
        tx.vout[1].scriptPubKey =
            MakePoolNftTapeScript({11, 22, 33}, {/*hashPrefix*/ {0x01}});
        BOOST_CHECK(CheckPoolNftTape(CTransaction(tx), fetcher) ==
                    TokenCheckResult::OK);
    }

    // A non-amount field changed -> tampered
    {
        CMutableTransaction tx;
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint(prevTx->GetId(), 0);
        tx.vout.resize(2);
        tx.vout[0].scriptPubKey = MakeContractScript(0x01);
        tx.vout[1].scriptPubKey =
            MakePoolNftTapeScript(oldAmounts, {/*hashPrefix*/ {0x99}});
        BOOST_CHECK(CheckPoolNftTape(CTransaction(tx), fetcher) ==
                    TokenCheckResult::POOLNFT_TAPE_TAMPERED);
    }

    // Previous transaction unavailable
    {
        CMutableTransaction tx;
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint(uint256S("0xbeef"), 0);
        tx.vout.resize(2);
        tx.vout[1].scriptPubKey = MakePoolNftTapeScript(oldAmounts);
        BOOST_CHECK(CheckPoolNftTape(CTransaction(tx), MakeFetcher({})) ==
                    TokenCheckResult::PREV_TX_UNAVAILABLE);
    }

    // Previous transaction has no poolnft layout (pool creation / funding)
    // -> treated as mint, passes
    {
        CMutableTransaction funding;
        funding.vin.resize(1);
        funding.vout.resize(1);
        funding.vout[0].scriptPubKey = MakeContractScript(0x02);
        const auto fundingRef = MakeTransactionRef(funding);

        CMutableTransaction tx;
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint(fundingRef->GetId(), 0);
        tx.vout.resize(2);
        tx.vout[1].scriptPubKey = MakePoolNftTapeScript(oldAmounts);
        BOOST_CHECK(CheckPoolNftTape(
                        CTransaction(tx),
                        MakeFetcher({{fundingRef->GetId(), fundingRef}})) ==
                    TokenCheckResult::OK);
    }
}

BOOST_AUTO_TEST_CASE(check_token_transaction_and_reject_reasons) {
    BOOST_CHECK_EQUAL(
        std::string(TokenCheckResultToRejectReason(TokenCheckResult::OK)), "");
    BOOST_CHECK_EQUAL(
        std::string(TokenCheckResultToRejectReason(
            TokenCheckResult::NEGATIVE_AMOUNT)),
        "bad-txn-token-amount-mismatch");
    BOOST_CHECK_EQUAL(
        std::string(TokenCheckResultToRejectReason(
            TokenCheckResult::AMOUNT_MISMATCH)),
        "bad-txn-token-amount-mismatch");
    BOOST_CHECK_EQUAL(
        std::string(TokenCheckResultToRejectReason(
            TokenCheckResult::POOLNFT_TAPE_TAMPERED)),
        "bad-txn-poolnft-tape-tampered");
    BOOST_CHECK_EQUAL(
        std::string(TokenCheckResultToRejectReason(
            TokenCheckResult::PREV_TX_UNAVAILABLE)),
        "bad-txn-token-prev-tx-unavailable");

    // CheckTokenTransaction: conservation failure takes precedence over the
    // poolnft check
    {
        CMutableTransaction tx;
        tx.vout.resize(2);
        tx.vout[0].scriptPubKey = MakeContractScript(0x01);
        tx.vout[1].scriptPubKey = MakeFTapeScript({-1, 0, 0, 0, 0, 0});
        BOOST_CHECK(CheckTokenTransaction(CTransaction(tx), MakeFetcher({})) ==
                    TokenCheckResult::NEGATIVE_AMOUNT);
    }
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Admission path: verify reject reasons via CTxnValidator / TxnValidation
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_SUITE(token_protection_admission_tests, TestChain100Setup)

namespace {

std::shared_ptr<CTxnValidator> MakeValidator(Config& config) {
    return std::make_shared<CTxnValidator>(
        config, mempool, std::make_shared<CTxnDoubleSpendDetector>(),
        g_connman->GetTxIdTracker());
}

TxInputDataSPtr MakeTxInput(const CMutableTransaction& tx) {
    return std::make_shared<CTxInputData>(
        g_connman->GetTxIdTracker(), MakeTransactionRef(tx), TxSource::rpc,
        TxValidationPriority::normal, GetTime(), false, Amount(0));
}

CValidationState ValidateOne(CTxnValidator& validator,
                             const CMutableTransaction& tx) {
    mining::CJournalChangeSetPtr changeSet{nullptr};
    return validator.processValidation(MakeTxInput(tx), changeSet);
}

} // namespace

BOOST_AUTO_TEST_CASE(txnvalidation_rejects_negative_ftape_amount) {
    GlobalConfig& config = GlobalConfig::GetConfig();
    config.SetTokenProtectionEnabled(true);

    const CScript p2pk = GetP2PK(coinbaseKey);
    CMutableTransaction spend;
    spend.nVersion = 1;
    spend.vin.resize(1);
    spend.vin[0].prevout = COutPoint(coinbaseTxns[0].GetId(), 0);
    spend.vout.resize(2);
    spend.vout[0].nValue = 49 * COIN;
    spend.vout[0].scriptPubKey = p2pk;
    spend.vout[1].nValue = Amount(0);
    spend.vout[1].scriptPubKey = MakeFTapeScript({-1, 0, 0, 0, 0, 0});
    SignP2PKInput(spend, 0, coinbaseTxns[0], coinbaseKey);

    mempool.Clear();
    auto validator = MakeValidator(config);
    const CValidationState state = ValidateOne(*validator, spend);
    BOOST_CHECK(!state.IsValid());
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txn-token-amount-mismatch");
    BOOST_CHECK_EQUAL(state.GetRejectCode(), REJECT_NONSTANDARD);
}

BOOST_AUTO_TEST_CASE(txnvalidation_rejects_amount_mismatch_via_mempool_parent) {
    GlobalConfig& config = GlobalConfig::GetConfig();
    config.SetTokenProtectionEnabled(true);

    const CScript contract = GetP2PK(coinbaseKey);

    // Parent transaction: mint token -> should be admitted
    CMutableTransaction parent;
    parent.nVersion = 1;
    parent.vin.resize(1);
    parent.vin[0].prevout = COutPoint(coinbaseTxns[0].GetId(), 0);
    parent.vout.resize(2);
    parent.vout[0].nValue = 49 * COIN;
    parent.vout[0].scriptPubKey = contract;
    parent.vout[1].nValue = Amount(0);
    parent.vout[1].scriptPubKey = MakeFTapeScript({100, 0, 0, 0, 0, 0});
    SignP2PKInput(parent, 0, coinbaseTxns[0], coinbaseKey);

    mempool.Clear();
    auto validator = MakeValidator(config);
    const CValidationState parentState = ValidateOne(*validator, parent);
    BOOST_REQUIRE_MESSAGE(parentState.IsValid(), parentState.GetRejectReason());
    BOOST_REQUIRE_EQUAL(mempool.Size(), 1U);

    // Child transaction: spends the contract output, tape amount 99 != input 100
    CMutableTransaction child;
    child.nVersion = 1;
    child.vin.resize(1);
    child.vin[0].prevout = COutPoint(parent.GetId(), 0);
    child.vout.resize(2);
    child.vout[0].nValue = 48 * COIN;
    child.vout[0].scriptPubKey = contract;
    child.vout[1].nValue = Amount(0);
    child.vout[1].scriptPubKey = MakeFTapeScript({99, 0, 0, 0, 0, 0});
    SignP2PKInput(child, 0, CTransaction(parent), coinbaseKey);

    const CValidationState childState = ValidateOne(*validator, child);
    BOOST_CHECK(!childState.IsValid());
    BOOST_CHECK_EQUAL(childState.GetRejectReason(),
                      "bad-txn-token-amount-mismatch");
    BOOST_CHECK_EQUAL(childState.GetRejectCode(), REJECT_NONSTANDARD);
}

BOOST_AUTO_TEST_CASE(txnvalidation_rejects_poolnft_tape_tampered) {
    // TestChain100Setup only provides coinbaseTxns[0] when COINBASE_MATURITY=1.
    GlobalConfig& config = GlobalConfig::GetConfig();
    config.SetTokenProtectionEnabled(true);

    const CScript contract = GetP2PK(coinbaseKey);
    const auto amounts = std::array<int64_t, 3>{10, 20, 30};
    const std::vector<uint8_t> hashOk{0x01};
    const std::vector<uint8_t> hashTampered{0x99};

    // Parent transaction: pool creation (no poolnft ancestor) -> admitted
    CMutableTransaction parent;
    parent.nVersion = 1;
    parent.vin.resize(1);
    parent.vin[0].prevout = COutPoint(coinbaseTxns[0].GetId(), 0);
    parent.vout.resize(2);
    parent.vout[0].nValue = 49 * COIN;
    parent.vout[0].scriptPubKey = contract;
    parent.vout[1].nValue = Amount(0);
    parent.vout[1].scriptPubKey = MakePoolNftTapeScript(amounts, hashOk);
    SignP2PKInput(parent, 0, coinbaseTxns[0], coinbaseKey);

    mempool.Clear();
    auto validator = MakeValidator(config);
    const CValidationState parentState = ValidateOne(*validator, parent);
    BOOST_REQUIRE_MESSAGE(parentState.IsValid(), parentState.GetRejectReason());
    BOOST_REQUIRE_EQUAL(mempool.Size(), 1U);

    // Child transaction: a non-amount field changed -> rejected
    CMutableTransaction child;
    child.nVersion = 1;
    child.vin.resize(1);
    child.vin[0].prevout = COutPoint(parent.GetId(), 0);
    child.vout.resize(2);
    child.vout[0].nValue = 48 * COIN;
    child.vout[0].scriptPubKey = contract;
    child.vout[1].nValue = Amount(0);
    child.vout[1].scriptPubKey = MakePoolNftTapeScript(amounts, hashTampered);
    SignP2PKInput(child, 0, CTransaction(parent), coinbaseKey);

    const CValidationState childState = ValidateOne(*validator, child);
    BOOST_CHECK(!childState.IsValid());
    BOOST_CHECK_EQUAL(childState.GetRejectReason(),
                      "bad-txn-poolnft-tape-tampered");
    BOOST_CHECK_EQUAL(childState.GetRejectCode(), REJECT_NONSTANDARD);
}

BOOST_AUTO_TEST_CASE(txnvalidation_token_protection_can_be_disabled) {
    GlobalConfig& config = GlobalConfig::GetConfig();
    config.SetTokenProtectionEnabled(false);

    const CScript p2pk = GetP2PK(coinbaseKey);
    CMutableTransaction spend;
    spend.nVersion = 1;
    spend.vin.resize(1);
    spend.vin[0].prevout = COutPoint(coinbaseTxns[0].GetId(), 0);
    spend.vout.resize(2);
    spend.vout[0].nValue = 49 * COIN;
    spend.vout[0].scriptPubKey = p2pk;
    spend.vout[1].nValue = Amount(0);
    spend.vout[1].scriptPubKey = MakeFTapeScript({-1, 0, 0, 0, 0, 0});
    SignP2PKInput(spend, 0, coinbaseTxns[0], coinbaseKey);

    mempool.Clear();
    auto validator = MakeValidator(config);
    const CValidationState state = ValidateOne(*validator, spend);
    BOOST_CHECK(state.IsValid());
    BOOST_CHECK(state.GetRejectReason() != "bad-txn-token-amount-mismatch");

    config.SetTokenProtectionEnabled(true);
}

BOOST_AUTO_TEST_SUITE_END()
