// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P0.0a.3 + P0.0a.4: lock_hierarchy + sync.h hook 单元测试
// 仅在 DEBUG_LOCKORDER build 下生效（release build 这些 hook 是 noop，断言 trivially pass）

#include "sync.h"
#include "validation/lock_hierarchy.h"

#include <boost/test/unit_test.hpp>

using tbc::lock_hierarchy::LEVEL_CS_MAIN;
using tbc::lock_hierarchy::LEVEL_MEMPOOL_SMTX;
using tbc::lock_hierarchy::LEVEL_BATCHWRITE_MTX;
using tbc::lock_hierarchy::LEVEL_DEFAULT;

BOOST_AUTO_TEST_SUITE(lock_hierarchy_tests)

// LEVEL_* 常量单调（compile-time，已 static_assert，runtime 二次确认）
BOOST_AUTO_TEST_CASE(constants_monotonic) {
    BOOST_CHECK_LT(LEVEL_CS_MAIN, LEVEL_MEMPOOL_SMTX);
    BOOST_CHECK_LT(LEVEL_MEMPOOL_SMTX, LEVEL_BATCHWRITE_MTX);
    BOOST_CHECK_LT(LEVEL_BATCHWRITE_MTX, LEVEL_DEFAULT);
}

// 401 callsite 兼容性：两个未标 level 的 mutex 互相持锁 — 不报错
// （这是 H-G 非破坏性增量的核心保证）
BOOST_AUTO_TEST_CASE(default_level_no_conflict) {
    CCriticalSection cs1;
    CCriticalSection cs2;
    LOCK(cs1);
    LOCK(cs2);   // 两个 LEVEL_DEFAULT，不应触发 LOCK ORDER VIOLATION
    BOOST_CHECK(true);   // 走到这里说明没 abort
}

// 同 cs 重入（boost::recursive_mutex 语义）— 不报错
BOOST_AUTO_TEST_CASE(reentrant_same_mutex) {
    CCriticalSection cs;
    LOCK(cs);
    LOCK(cs);   // 重入应该 OK
    BOOST_CHECK(true);
}

// 注意：违反规则的死亡测试需要 BOOST_DEATH_TEST，标准 Boost.Test 不直接支持
// 标准做法是做 separate process spawning。这里跳过 abort 测试，依赖 functional 验证。

BOOST_AUTO_TEST_SUITE_END()
