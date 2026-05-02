// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 Lock hierarchy constants.
//
// 全局锁层级（强制低 level 先取，违反则 DEBUG_LOCKORDER abort 或 release 静态注解失败）：
//
//   cs_main (boost::recursive_mutex, level 0)
//     > mempool.smtx (std::shared_mutex, level 1)
//       > pcoinsTip.batchWriteMtx (std::shared_mutex, level 2)
//         > pcoinsTip.metaMtx (std::shared_mutex, level 3)
//           > inflight_shard_mtx (std::shared_mutex, level 4)
//             > worker.queue_mtx (std::mutex, level 5)
//
// H-G 非破坏性增量：现有 401 处 LOCK 调用宏未标 level，视为 LEVEL_DEFAULT
// (INT_MAX/2)，跟所有标了 level 的 mutex 都满足"低 level 先取"约束。
// 详见 docs/plans/lock-hierarchy.md / docs/plans/tasks/P0.0a.3-lock-hierarchy-doc.md

#ifndef BITCOIN_VALIDATION_LOCK_HIERARCHY_H
#define BITCOIN_VALIDATION_LOCK_HIERARCHY_H

#include <climits>

namespace tbc {
namespace lock_hierarchy {

constexpr int LEVEL_CS_MAIN          = 0;
constexpr int LEVEL_MEMPOOL_SMTX     = 1;
constexpr int LEVEL_BATCHWRITE_MTX   = 2;
// M5 (post-Teranode-audit)：CCoinsViewCache 内部 mutex（mCoinsViewCacheMtx）。
//   现行调用方向都是 batchWriteMtx → mCoinsViewCacheMtx 一致，加 level 2.5
//   防未来反向新代码绕开。
constexpr int LEVEL_COINS_CACHE_INTERNAL = 2;   // 跟 batchWriteMtx 同级（实际嵌套调用，非互斥关系）
constexpr int LEVEL_META_MTX         = 3;
constexpr int LEVEL_INFLIGHT_SHARD   = 4;
constexpr int LEVEL_WORKER_QUEUE     = 5;

// H-G: 现有未标注 mutex 默认 level（叶子，跟所有标 level mutex 不冲突）
constexpr int LEVEL_DEFAULT          = INT_MAX / 2;

// 静态断言保证层级单调
static_assert(LEVEL_CS_MAIN < LEVEL_MEMPOOL_SMTX,
              "lock hierarchy: cs_main must be lower than mempool.smtx");
static_assert(LEVEL_MEMPOOL_SMTX < LEVEL_BATCHWRITE_MTX,
              "lock hierarchy: mempool.smtx must be lower than batchWriteMtx");
static_assert(LEVEL_BATCHWRITE_MTX < LEVEL_META_MTX,
              "lock hierarchy: batchWriteMtx must be lower than metaMtx");
static_assert(LEVEL_META_MTX < LEVEL_INFLIGHT_SHARD,
              "lock hierarchy: metaMtx must be lower than inflight_shard_mtx");
static_assert(LEVEL_INFLIGHT_SHARD < LEVEL_WORKER_QUEUE,
              "lock hierarchy: inflight_shard_mtx must be lower than worker.queue_mtx");
static_assert(LEVEL_WORKER_QUEUE < LEVEL_DEFAULT,
              "lock hierarchy: all named levels must be lower than LEVEL_DEFAULT");

} // namespace lock_hierarchy
} // namespace tbc

// v2.6.1 P4.1 (架构 C-5)：std::shared_*_mutex 的 lock_hierarchy 跟踪 RAII 桥接
//   见 lock_hierarchy_scoped.h（独立头避免与 sync.h 循环包含）
#endif // BITCOIN_VALIDATION_LOCK_HIERARCHY_H
