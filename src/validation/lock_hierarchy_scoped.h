// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P4.1 (架构 C-5)：std::shared_*_mutex 不被 sync.h LOCK 宏覆盖，
//   提供 RAII 桥接：拿锁前 EnterCritical(level)，走锁后 LeaveCritical()。
//   DEBUG_LOCKORDER 编译时会按 level 单调检测，违反则 abort。
//   release build 下 EnterCritical/LeaveCritical 是 no-op，零开销。

#ifndef BITCOIN_VALIDATION_LOCK_HIERARCHY_SCOPED_H
#define BITCOIN_VALIDATION_LOCK_HIERARCHY_SCOPED_H

#include "lock_hierarchy.h"
#include "../sync.h"

namespace tbc {
namespace lock_hierarchy {

class ScopedLevel {
public:
    ScopedLevel(const char* name, const char* file, int line, void* mtx, int level)
    {
        EnterCritical(name, file, line, mtx, /*fTry=*/false, level);
    }
    ~ScopedLevel() { LeaveCritical(); }
    ScopedLevel(const ScopedLevel&) = delete;
    ScopedLevel& operator=(const ScopedLevel&) = delete;
};

} // namespace lock_hierarchy
} // namespace tbc

// 标记一个 std::shared_*_mutex 进入 lock_hierarchy 跟踪
//   配合 std::unique_lock<...> 一起用：
//     LOCK_LEVEL_TRACK(mempool.smtx, LEVEL_MEMPOOL_SMTX);
//     std::unique_lock<std::shared_timed_mutex> m_lock(mempool.smtx);
#define LOCK_LEVEL_TRACK_CAT_(a, b) a##b
#define LOCK_LEVEL_TRACK_CAT(a, b)  LOCK_LEVEL_TRACK_CAT_(a, b)
#define LOCK_LEVEL_TRACK(mtx, lvl)                                            \
    tbc::lock_hierarchy::ScopedLevel LOCK_LEVEL_TRACK_CAT(_lh_, __LINE__)(    \
        #mtx, __FILE__, __LINE__, (void*)&(mtx), (lvl))

#endif // BITCOIN_VALIDATION_LOCK_HIERARCHY_SCOPED_H
