// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.

#include "validation/tx_stash.h"

namespace tbc {
namespace validation {

// 全局实例（P2.4 引入）。
//   Phase B (post-Teranode-audit)：删除 g_race_stash + g_resubmit_limiter
//   （sub-A3 后无 caller — race retry 路径已改为直接 reject，
//   reorg-resubmit 失败改 push 回 g_reorg_stash 让 drain 重派）。
ReorgStash g_reorg_stash;

} // namespace validation
} // namespace tbc
