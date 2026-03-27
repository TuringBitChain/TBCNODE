#include "tbc_script_validation.h"

#include "script/script_num.h"

namespace TBCScriptValidation {

KeyMaterial::KeyMaterial() {
    key.MakeNewKey(true);

    CPubKey pubComp = key.GetPubKey();
    compressedPubkey.assign(pubComp.begin(), pubComp.end());

    CPubKey pubUncomp = pubComp;
    if (!pubUncomp.Decompress()) {
        BOOST_FAIL("Failed to decompress pubkey");
    }
    uncompressedPubkey.assign(pubUncomp.begin(), pubUncomp.end());

    CPubKey pubHybrid = pubUncomp;
    *const_cast<uint8_t*>(&pubHybrid[0]) = 0x06 | (pubHybrid[64] & 1);
    hybridPubkey.assign(pubHybrid.begin(), pubHybrid.end());

    XOnlyPubKey xonly = key.GetXOnlyPubKey();
    xonlyPubkey.assign(xonly.begin(), xonly.end());
}

CMutableTransaction BuildCreditingTransaction(const CScript& scriptPubKey, const Amount nValue) {
    CMutableTransaction txCredit;
    txCredit.nVersion = 1;
    txCredit.nLockTime = 0;
    txCredit.vin.resize(1);
    txCredit.vout.resize(1);
    txCredit.vin[0].prevout = COutPoint();
    txCredit.vin[0].scriptSig = CScript() << CScriptNum(0) << CScriptNum(0);
    txCredit.vin[0].nSequence = CTxIn::SEQUENCE_FINAL;
    txCredit.vout[0].scriptPubKey = scriptPubKey;
    txCredit.vout[0].nValue = nValue;
    return txCredit;
}

CMutableTransaction BuildSpendingTransaction(const CScript& scriptSig,
                                             const CMutableTransaction& txCredit) {
    CMutableTransaction txSpend;
    txSpend.nVersion = 1;
    txSpend.nLockTime = 0;
    txSpend.vin.resize(1);
    txSpend.vout.resize(1);
    txSpend.vin[0].prevout = COutPoint(txCredit.GetId(), 0);
    txSpend.vin[0].scriptSig = scriptSig;
    txSpend.vin[0].nSequence = CTxIn::SEQUENCE_FINAL;
    txSpend.vout[0].scriptPubKey = CScript();
    txSpend.vout[0].nValue = txCredit.vout[0].nValue;
    return txSpend;
}
namespace {
/**
 * Run a single script validation test and assert expected status, error and optional stack.
 * When expStatus is true, stack size and stack top are also asserted.
 */
void RunCase(std::string_view testName,
             const CScript& script,
             uint32_t flags,
             bool expStatus,
             ScriptError expError,
             size_t expStackSize,
             const std::vector<uint8_t>& expStackTop,
             const BaseSignatureChecker& checker) {
    const Config& config = GlobalConfig::GetConfig();
    ScriptError error;
    LimitedStack stack(UINT32_MAX);
    const auto status = EvalScript(
        config, false, task::CCancellationSource::Make()->GetToken(), stack,
        script, flags, checker, &error);

    BOOST_CHECK_MESSAGE(expStatus == status.value(),
                        testName << " - Status mismatch, expected: " << (expStatus ? "true" : "false")
                                  << ", actual: " << (status.value() ? "true" : "false"));

    BOOST_CHECK_MESSAGE(expError == error,
                        testName << " - Error code mismatch, expected: " << ScriptErrorString(expError)
                                  << ", actual: " << ScriptErrorString(error));

    if (expStatus) {
        BOOST_CHECK_MESSAGE(expStackSize == stack.size(),
                            testName << " - Stack size mismatch, expected: " << expStackSize
                                      << ", actual: " << stack.size());

        if (expStackSize > 0 && stack.size() > 0) {
            const auto& stack0 = stack.at(0);
            const auto& stack0Element = stack0.GetElement();
            BOOST_CHECK_MESSAGE(
                std::equal(stack0Element.begin(), stack0Element.end(),
                           expStackTop.begin(), expStackTop.end()),
                testName << " - Stack top value mismatch"
                          << ", expected (hex): " << HexStr(expStackTop)
                          << " (size: " << expStackTop.size() << ")"
                          << ", actual (hex): " << HexStr(stack0Element)
                          << " (size: " << stack0Element.size() << ")");
        } else if (expStackSize > 0 && stack.size() == 0) {
            BOOST_CHECK_MESSAGE(false,
                                testName << " - Stack is empty but expected stack size: " << expStackSize
                                          << ", expected top value (hex): " << HexStr(expStackTop));
        }
    }
}
} // namespace

void CheckPass(std::string_view testName,
               const CScript& script,
               uint32_t flags,
               size_t expStackSize,
               const std::vector<uint8_t>& expStackTop,
               const BaseSignatureChecker& checker) {
    RunCase(testName, script, flags, true, SCRIPT_ERR_OK, expStackSize, expStackTop, checker);
}

void CheckError(std::string_view testName,
                const CScript& script,
                uint32_t flags,
                ScriptError expError,
                const BaseSignatureChecker& checker) {
    RunCase(testName, script, flags, false, expError, 0, failure, checker);
}

void CheckPassForAllFlags(std::string_view testName,
                          const CScript& script,
                          size_t expStackSize,
                          const std::vector<uint8_t>& expStackTop,
                          const BaseSignatureChecker& checker) {
    for (uint32_t f : flagSet) {
        CheckPass(testName, script, f, expStackSize, expStackTop, checker);
    }
}

void CheckErrorForAllFlags(std::string_view testName,
                           const CScript& script,
                           ScriptError expError,
                           const BaseSignatureChecker& checker) {
    for (uint32_t f : flagSet) {
        CheckError(testName, script, f, expError, checker);
    }
}

} // namespace TBCScriptValidation
