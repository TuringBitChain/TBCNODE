// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P3.3: AcceptToMemoryPoolWorker — worker 内验证 + commit 包装
//
// dispatcher worker 调这个函数处理 WorkItem：
//   1. 取 g_chainstate snapshot（tip / script_flags / genesis_h）
//   2. 调 PTV processValidation（沿用旧路径做 validation + mempool commit）
//   3. 提交后 doubleCheck（P3.1 真接入 4 项 race 兜底）
//   4. 失败 → 走 P3.2 Resubmit 策略
//
// 当前 P3.3 阶段：把 init.cpp 里的 handler lambda 抽出来 + 接 doubleCheck 的占位 hook。
//                P3.1 / P3.2 在此基础上把真正 race 检查 + token bucket 接进来。

#ifndef BITCOIN_VALIDATION_WORKER_VALIDATE_H
#define BITCOIN_VALIDATION_WORKER_VALIDATE_H

#include <string>

namespace tbc {
namespace validation {

struct WorkItem;

// 处理单个 WorkItem（同步）：返回 true=已进 mempool / chain；false=拒绝（err 含原因）。
// thread-safe：可以被多个 worker 并发调用，内部用 PTV cs_main / smtx 加锁。
bool AcceptToMemoryPoolWorker(const WorkItem& item, std::string& err);

} // namespace validation
} // namespace tbc

#endif // BITCOIN_VALIDATION_WORKER_VALIDATE_H
