#pragma once

#include <script/interpreter.h>
#include <script/script_error.h>
#include <script/script_flags.h>
#include <config.h>
#include <key.h>
#include <utilstrencodings.h>
#include <taskcancellation.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <string_view>
#include <vector>

namespace TBCScriptValidation {

/**
 * Shared key material: one random key and pubkeys in all formats (compressed, uncompressed,
 * hybrid, x-only).
 */
struct KeyMaterial {
    CKey key;
    std::vector<uint8_t> compressedPubkey;
    std::vector<uint8_t> uncompressedPubkey;
    std::vector<uint8_t> hybridPubkey;
    std::vector<uint8_t> xonlyPubkey;

    KeyMaterial();
};

struct SignatureMaterial {
    std::vector<uint8_t> ecdsaSig;
    std::vector<uint8_t> schnorrSig;
};

struct TransactionMaterial {
    CTransactionRef ecdsaTx;
    CTransactionRef schnorrTx;
};

struct TransactionSignatureCheckerMaterial {
    std::unique_ptr<TransactionSignatureChecker> ecdsaChecker;
    std::unique_ptr<TransactionSignatureChecker> schnorrChecker;
};

// Shared constants for script validation tests (e.g. CHECKSIG, CHECKMULTISIG, CHECKDATASIG).
inline const std::vector<uint8_t> failure = {};
inline const std::vector<uint8_t> success = {1};
inline std::array<uint32_t, 2> flagSet{{0, STANDARD_SCRIPT_VERIFY_FLAGS}};

void CheckPass(std::string_view testName,
               const CScript& script,
               uint32_t flags,
               size_t expStackSize,
               const std::vector<uint8_t>& expStackTop,
               const BaseSignatureChecker& checker);

void CheckError(std::string_view testName,
                const CScript& script,
                uint32_t flags,
                ScriptError expError,
                const BaseSignatureChecker& checker);

void CheckPassForAllFlags(std::string_view testName,
                          const CScript& script,
                          size_t expStackSize,
                          const std::vector<uint8_t>& expStackTop,
                          const BaseSignatureChecker& checker);

void CheckErrorForAllFlags(std::string_view testName,
                           const CScript& script,
                           ScriptError expError,
                           const BaseSignatureChecker& checker);

} // namespace TBCScriptValidation
