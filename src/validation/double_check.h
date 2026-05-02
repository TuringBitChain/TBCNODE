// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P3.1: doubleCheck 协议（worker commit 前的 4 项 race 兜底）
//
// 协议（详 docs/plans/cs_main-refactor-detailed-design.md §6）：
//   #1 tip_hash 不变（snap2.tip_hash == snap.tip_hash） → race 触发 Resubmit(TipChanged)
//   #2 script_flags 不变 → Resubmit(FlagsChanged)
//   #3a in-flight 双花（dsDetector.IsSpentNL）→ reject(double-spend)
//   #3b chain UTXO 已花重读（GetCoinConcurrent + IsSpent，always 重读所有 input，H7）
//   #4 mempool 父仍存在（mempool.GetNL）∥ 已上链未花
//
// 当前 P3.1 阶段：协议常量 + H-D perInputScriptFlags 推导算法 + 单元测试
// 真集成（worker.ProcessItem 的 4 项调用）留 P4.1 ConnectBlock 锁次序改造时一并接入

#ifndef BITCOIN_VALIDATION_DOUBLE_CHECK_H
#define BITCOIN_VALIDATION_DOUBLE_CHECK_H

#include "coins.h"   // Coin
#include "validation/chainstate.h"

#include <cstdint>
#include <vector>

// 强制头文件（src/script/script_flags.h 中定义）
#ifndef SCRIPT_UTXO_AFTER_GENESIS
#define SCRIPT_UTXO_AFTER_GENESIS (1U << 22)
#endif

// 强制头文件（src/coins.h MEMPOOL_HEIGHT 暗含；validation.cpp:1113 抛 runtime_error）
constexpr int32_t MEMPOOL_HEIGHT_SENTINEL = 0x7FFFFFFF;   // TBC 习惯值，跟 src/coins.h 一致

namespace tbc {
namespace validation {

// ============================================================================
// H-D + P-1 修复：perInputScriptFlags 推导（worker 阶段 3 用，不持 cs_main）
//
// 等价于 src/validation.cpp:3508-3514 GetInputScriptBlockHeight + 3597-3602 IsGenesisEnabled：
//   int h = (coin.height == MEMPOOL_HEIGHT) ? snap.height + 1 : coin.height;
//   if (h >= snap.genesisActivationHeight) flag = SCRIPT_UTXO_AFTER_GENESIS;
//
// MEMPOOL_HEIGHT 是哨兵常量（不是真高度），必须先替换才能跟 genesisActivationHeight 比
// ============================================================================
inline uint32_t DerivePerInputScriptFlags(int32_t coin_height,
                                          const Chainstate::Snapshot& snap) noexcept {
    int32_t h = (coin_height == MEMPOOL_HEIGHT_SENTINEL)
                    ? snap.height + 1
                    : coin_height;
    return (h >= snap.genesisActivationHeight) ? SCRIPT_UTXO_AFTER_GENESIS : 0u;
}

// 批量版本（worker ProcessItem 阶段 3 用）
inline std::vector<uint32_t>
DeriveAllPerInputScriptFlags(const std::vector<Coin>& input_coins,
                              const Chainstate::Snapshot& snap) {
    std::vector<uint32_t> flags;
    flags.reserve(input_coins.size());
    for (const Coin& c : input_coins) {
        flags.push_back(DerivePerInputScriptFlags(static_cast<int32_t>(c.GetHeight()), snap));
    }
    return flags;
}

// ============================================================================
// doubleCheck 协议常量
// ============================================================================
enum class DoubleCheckResult {
    OK,               // 4 项全过 → 可 commit
    TipChanged,       // #1 触发，Resubmit(TipChanged)
    FlagsChanged,     // #2 触发，Resubmit(FlagsChanged)
    DoubleSpend,      // #3a 触发，reject
    InputSpent,       // #3b 触发，reject(input-spent-or-missing)
    ParentEvicted,    // #4 触发，reject(parent-evicted)
    ReorgInProgress,  // C-2: epoch 跨越或半态，Resubmit(ReorgInProgress)
};

// snap1 vs snap2 比对（#1+#2，无 mempool 依赖，纯值比对）
inline DoubleCheckResult CheckSnapStable(const Chainstate::Snapshot& snap1,
                                          const Chainstate::Snapshot& snap2) noexcept {
    if (snap1.tip_hash != snap2.tip_hash) return DoubleCheckResult::TipChanged;
    if (snap1.script_flags != snap2.script_flags) return DoubleCheckResult::FlagsChanged;
    return DoubleCheckResult::OK;
}

// ============================================================================
// P3.1 真接入（v2.6.1）：worker 验证完成后的 doubleCheck 入口
//
// 调用次序（worker_validate.cpp::AcceptToMemoryPoolWorker 内）：
//   snap1 = Capture()
//   ok = processValidation(item)        // PTV 内部已持 cs_main + smtx 做 #3a/#3b/#4
//   snap2 = Capture()
//   res = DoubleCheckPostValidation(snap1, snap2, ok)
//   if (res != OK) → 走 P3.2 Resubmit / reject
//
// 当前实现：#1+#2 真比对（CheckSnapStable）；
//          #3a/#3b/#4 借 PTV processValidation 内部已实现的去重 + 双花检测
//          （PTV 内部加 smtx 排他写）。等 P4.1 把 commit 移出 PTV 时再把 #3a/#3b/#4
//          抽到这里前置实现。
// ============================================================================
inline DoubleCheckResult DoubleCheckPostValidation(
    const Chainstate::Snapshot& snap1,
    const Chainstate::Snapshot& snap2,
    bool processValidation_ok,
    uint64_t reorg_epoch_before = 0,
    uint64_t reorg_epoch_after = 0) noexcept
{
    // C-2: reorg epoch 检查（在 tip / flags 检查之前，因 reorg 期间 tip 也会动）
    //   epoch_before 奇数 → 进入时已经在 reorg；epoch_after 不等于 before 或奇数 → 期间发生 reorg
    if ((reorg_epoch_before & 1u) || (reorg_epoch_after & 1u)
        || reorg_epoch_before != reorg_epoch_after) {
        return DoubleCheckResult::ReorgInProgress;
    }
    // #1 + #2: tip / script_flags 是否漂了
    DoubleCheckResult snap_res = CheckSnapStable(snap1, snap2);
    if (snap_res != DoubleCheckResult::OK) {
        // tip 或 flags 漂了 — race。即使 processValidation 成功，
        // commit 用了过期 snapshot，需要 Resubmit 让新 snapshot 重验。
        //   Phase B (post-Teranode-audit)：reorg-resubmit 路径走 g_reorg_stash drain
        //   重派；普通路径直接 reject 客户端（sub-A3）。
        return snap_res;
    }
    // tip / flags 稳定。失败可能是真错（reject）也可能是 PTV 内部检测到的双花/缺父。
    // 当前：失败统一视作 reject（PTV 已经 ban 处理）；后续 P4.1 把 commit 移出后再细分。
    if (!processValidation_ok) {
        return DoubleCheckResult::InputSpent;  // 占位归类
    }
    return DoubleCheckResult::OK;
}

inline const char* DoubleCheckResultToString(DoubleCheckResult r) noexcept {
    switch (r) {
        case DoubleCheckResult::OK:              return "ok";
        case DoubleCheckResult::TipChanged:      return "tip-changed";
        case DoubleCheckResult::FlagsChanged:    return "flags-changed";
        case DoubleCheckResult::DoubleSpend:     return "double-spend";
        case DoubleCheckResult::InputSpent:      return "input-spent";
        case DoubleCheckResult::ParentEvicted:   return "parent-evicted";
        case DoubleCheckResult::ReorgInProgress: return "reorg-in-progress";
    }
    return "unknown";
}

// 是否为可重试的 race（reorg-resubmit 路径走 g_reorg_stash drain 重派）
inline bool IsRetryableRace(DoubleCheckResult r) noexcept {
    return r == DoubleCheckResult::TipChanged
        || r == DoubleCheckResult::FlagsChanged
        || r == DoubleCheckResult::ReorgInProgress;
}

} // namespace validation
} // namespace tbc

#endif // BITCOIN_VALIDATION_DOUBLE_CHECK_H
