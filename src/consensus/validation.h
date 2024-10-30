// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONSENSUS_VALIDATION_H
#define BITCOIN_CONSENSUS_VALIDATION_H

#include <string>

#include "primitives/block.h"

/** "reject" message codes */
static const uint8_t REJECT_MALFORMED = 0x01;
static const uint8_t REJECT_INVALID = 0x10;
static const uint8_t REJECT_OBSOLETE = 0x11;
static const uint8_t REJECT_DUPLICATE = 0x12;
static const uint8_t REJECT_NONSTANDARD = 0x40;
static const uint8_t REJECT_DUST = 0x41;
static const uint8_t REJECT_INSUFFICIENTFEE = 0x42;
static const uint8_t REJECT_CHECKPOINT = 0x43;
static const uint8_t REJECT_TOOBUSY = 0x44;

/** Index marker for when no witness commitment is present in a coinbase transaction. */
static constexpr int NO_WITNESS_COMMITMENT{-1};

/** Minimum size of a witness commitment structure. Defined in BIP 141. **/
static constexpr size_t MINIMUM_WITNESS_COMMITMENT{38};

/** Capture information about block/transaction validation */
class CValidationState {
private:
    enum mode_state {
        MODE_VALID,   //!< everything ok
        MODE_INVALID, //!< network rule violation (DoS value may be set)
        MODE_ERROR,   //!< run-time error
    } mode {MODE_VALID};
    int nDoS {0};
    std::string strDebugMessage {};
    std::string strRejectReason {};
    unsigned int chRejectCode {0};
    bool corruptionPossible {false};
    bool fMissingInputs {false};
    bool fDoubleSpendDetected {false};
    bool fMempoolConflictDetected {false};
    bool nonFinal {false};
    bool fValidationTimeoutExceeded {false};
    bool fStandardTx {false};
    bool fResubmitTx {false};

public:
    bool DoS(int level, bool ret = false, unsigned int chRejectCodeIn = 0,
             const std::string &strRejectReasonIn = "",
             bool corruptionIn = false,
             const std::string &strDebugMessageIn = "") {
        chRejectCode = chRejectCodeIn;
        strRejectReason = strRejectReasonIn;
        corruptionPossible = corruptionIn;
        strDebugMessage = strDebugMessageIn;
        if (mode == MODE_ERROR) {
            return ret;
        }
        nDoS += level;
        mode = MODE_INVALID;
        return ret;
    }

    bool Invalid(bool ret = false, unsigned int _chRejectCode = 0,
                 const std::string &_strRejectReason = "",
                 const std::string &_strDebugMessage = "") {
        return DoS(0, ret, _chRejectCode, _strRejectReason, false,
                   _strDebugMessage);
    }
    bool Error(const std::string &strRejectReasonIn) {
        if (mode == MODE_VALID) {
            strRejectReason = strRejectReasonIn;
        }

        mode = MODE_ERROR;
        return false;
    }

    bool IsValid() const { return mode == MODE_VALID; }
    bool IsInvalid() const { return mode == MODE_INVALID; }
    bool IsError() const { return mode == MODE_ERROR; }
    bool IsInvalid(int &nDoSOut) const {
        if (IsInvalid()) {
            nDoSOut = nDoS;
            return true;
        }
        return false;
    }
    bool IsMissingInputs() const { return fMissingInputs; }
    bool IsDoubleSpendDetected() const { return fDoubleSpendDetected; }
    bool IsMempoolConflictDetected() const { return fMempoolConflictDetected; }

    bool CorruptionPossible() const { return corruptionPossible; }
    bool IsNonFinal() const { return nonFinal; }
    bool IsValidationTimeoutExceeded() const { return fValidationTimeoutExceeded; };
    bool IsStandardTx() const { return fStandardTx; };
    bool IsResubmittedTx() const { return fResubmitTx; };

    void SetCorruptionPossible() { corruptionPossible = true; }
    void SetMissingInputs() { fMissingInputs = true; }
    void SetDoubleSpendDetected() { fDoubleSpendDetected = true; }
    void SetMempoolConflictDetected() { fMempoolConflictDetected = true; }
    void SetNonFinal(bool nf = true) { nonFinal = nf; }
    void SetValidationTimeoutExceeded() { fValidationTimeoutExceeded = true; };
    void SetStandardTx() { fStandardTx = true; };
    void SetResubmitTx(bool nf = true) { fResubmitTx = nf; };

    int GetNDoS() const { return nDoS; }
    unsigned int GetRejectCode() const { return chRejectCode; }
    std::string GetRejectReason() const { return strRejectReason; }
    std::string GetDebugMessage() const { return strDebugMessage; }
};

/** Compute at which vout of the block's coinbase transaction the witness commitment occurs, or -1 if not found */
inline int GetWitnessCommitmentIndex(std::shared_ptr<CBlock> block)
{
    int commitpos = NO_WITNESS_COMMITMENT;
    if (!block->vtx.empty()) {
        for (size_t o = 0; o < block->vtx[0]->vout.size(); o++) {
            const CTxOut& vout = block->vtx[0]->vout[o];
            if (vout.scriptPubKey.size() >= MINIMUM_WITNESS_COMMITMENT &&
                vout.scriptPubKey[0] == OP_RETURN &&
                vout.scriptPubKey[1] == 0x24 &&
                vout.scriptPubKey[2] == 0xaa &&
                vout.scriptPubKey[3] == 0x21 &&
                vout.scriptPubKey[4] == 0xa9 &&
                vout.scriptPubKey[5] == 0xed) {
                commitpos = o;
            }
        }
    }
    return commitpos;
}

#endif // BITCOIN_CONSENSUS_VALIDATION_H
