// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.

#include "validation/worker_validate.h"

#include "amount.h"
#include "consensus/validation.h"
#include "logging.h"
#include "mining/journal_builder.h"
#include "mining/journal_change_set.h"
#include "net/net.h"               // g_connman
#include "txmempool.h"             // mempool
#include "txn_validation_data.h"
#include "txn_validator.h"
#include "util.h"
#include "validation.h"            // maxTxFee
#include "validation/chainstate.h"      // g_chainstate snapshot
#include "validation/double_check.h"    // P3.1 4-item protocol
#include "validation/per_chain_worker.h"

namespace tbc {
namespace validation {

bool AcceptToMemoryPoolWorker(const WorkItem& item, std::string& err) {
    if (!item.tx) { err = "null tx"; return false; }
    if (!g_connman) { err = "no connman"; return false; }
    const auto& txValidator = g_connman->getTxnValidator();
    if (!txValidator) { err = "no txValidator"; return false; }

    // v2.6.1 trace：worker 真在跑（-debug=txnval 可开）
    const int64_t t0 = GetTimeMicros();
    LogPrint(BCLog::TXNVAL,
             "v2.6.1 worker: ENTER tx=%s source=%s size=%u\n",
             item.tx->GetId().ToString(),
             item.source == TxSource::p2p ? "p2p"
               : item.source == TxSource::rpc ? "rpc"
               : item.source == TxSource::reorg ? "reorg" : "other",
             (unsigned)item.tx->GetTotalSize());

    // P3.1 #1+#2: snapshot before（用于 doubleCheck tip / flags 是否漂）
    // C-2: 同时拍 reorg epoch，PTV 完成后再读一次比对
    const Chainstate::Snapshot snap1 = g_chainstate.Capture();
    const uint64_t reorg_epoch_before = g_reorg_epoch.load(std::memory_order_acquire);

    mining::CJournalChangeSetPtr changeSet {
        mempool.getJournalBuilder().getNewChangeSet(
            mining::JournalUpdateReason::NEW_TXN)
    };

    // P5.x: 按 source 推 priority。fee 用 item.nAbsurdFee；0 视为"不限制"
    //   - rpc: 用 caller 指定的 maxTxFee 或 0（allowhighfees=true）
    //   - p2p: caller 传 0（不卡 absurd fee 防误 ban）
    //   - file/finalised/reorg/wallet: caller 按入口语义决定
    const TxValidationPriority prio = (item.source == TxSource::p2p)
        ? TxValidationPriority::high
        : TxValidationPriority::normal;
    const Amount absurdFee = Amount(item.nAbsurdFee);
    const int64_t acceptTime = item.accept_time != 0
        ? item.accept_time
        : GetTime();

    const CValidationState status = txValidator->processValidation(
        std::make_shared<CTxInputData>(
            g_connman->GetTxIdTracker(),
            CTransactionRef(item.tx),
            item.source,
            prio,
            acceptTime,
            item.fLimitFree,
            absurdFee,
            item.pfrom),
        changeSet,
        true);

    // P3.1 #1+#2: snapshot after — tip/flags 漂了视为 race
    const Chainstate::Snapshot snap2 = g_chainstate.Capture();
    const uint64_t reorg_epoch_after = g_reorg_epoch.load(std::memory_order_acquire);
    const bool ok = status.IsValid();
    const DoubleCheckResult dc = DoubleCheckPostValidation(
        snap1, snap2, ok, reorg_epoch_before, reorg_epoch_after);

    if (dc != DoubleCheckResult::OK) {
        // race 检出：tip / flags 漂了，commit 已发生（PTV 内部完成），
        // 这里仅记录 + 上抛失败 race-retry。Phase B (post-Teranode-audit)：
        //   普通路径 → 直接 reject 客户端（sub-A3 已实施）。
        //   reorg-resubmit 路径 → ChainDispatcher worker_handler 检测到失败 +
        //   is_reorg_resubmit + race 错误 → push 回 g_reorg_stash，drain 周期重派。
        // M1 (post-Teranode-audit)：log 标签加 status.GetRejectReason() 让运维分清
        //   真双花 / 缺父 / fee 不足 vs 真 race tip-changed/flags-changed。
        //   IsRetryableRace=true → race；=false → status reject 真原因。
        LogPrint(BCLog::TXNVAL,
                 "v2.6.1 doubleCheck: tx=%s dc=%s status=%s (snap1.h=%d snap2.h=%d "
                 "tip1=%s tip2=%s flags1=0x%x flags2=0x%x)\n",
                 item.tx->GetId().ToString(),
                 DoubleCheckResultToString(dc),
                 ok ? "ok" : status.GetRejectReason().c_str(),
                 snap1.height, snap2.height,
                 snap1.tip_hash.ToString(), snap2.tip_hash.ToString(),
                 snap1.script_flags, snap2.script_flags);
        if (IsRetryableRace(dc)) {
            err = std::string("race-retry: ") + DoubleCheckResultToString(dc);
            return false;
        }
    }

    if (!ok) {
        err = status.GetRejectReason();
        LogPrint(BCLog::TXNVAL,
                 "v2.6.1 worker: REJECT tx=%s reason=%s elapsed_us=%d\n",
                 item.tx->GetId().ToString(), err.c_str(),
                 (int)(GetTimeMicros() - t0));
        return false;
    }
    LogPrint(BCLog::TXNVAL,
             "v2.6.1 worker: ACCEPT tx=%s elapsed_us=%d tip_h=%d\n",
             item.tx->GetId().ToString(),
             (int)(GetTimeMicros() - t0), snap2.height);
    return true;
}

} // namespace validation
} // namespace tbc
