// Copyright (c) 2019 The Bitcoin SV developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "enum_cast.h"
#include <chrono>

enum class PTVTaskScheduleStrategy
{
    UNKNOWN,
    // Legacy chain detector (can't handle graphs, can't handle txn out-of-order).
    CHAIN_DETECTOR,
    // Schedules txn validation in topological order within one batch of transactions.
    TOPO_SORT
};

// Enable enum_cast for PTVTaskScheduleStrategy
inline const enumTableT<PTVTaskScheduleStrategy>& enumTable(PTVTaskScheduleStrategy)
{
    static enumTableT<PTVTaskScheduleStrategy> table
    {
        { PTVTaskScheduleStrategy::UNKNOWN,        "UNKNOWN" },
        { PTVTaskScheduleStrategy::CHAIN_DETECTOR, "CHAIN_DETECTOR" },
        { PTVTaskScheduleStrategy::TOPO_SORT,      "TOPO_SORT" }
    };
    return table;
}

/** A default ratio for max number of standard transactions per thread. */
static constexpr uint64_t DEFAULT_MAX_STD_TXNS_PER_THREAD_RATIO = 1000;
/** A default ratio for max number of non-standard transactions per thread. */
static constexpr uint64_t DEFAULT_MAX_NON_STD_TXNS_PER_THREAD_RATIO = 1000;
/** The maximum wall time for standard transaction validation before we terminate the task */
static constexpr std::chrono::milliseconds DEFAULT_MAX_STD_TXN_VALIDATION_DURATION =
	std::chrono::milliseconds{10};
/** The maximum wall time for non-standard transaction validation before we terminate the task */
static constexpr std::chrono::milliseconds DEFAULT_MAX_NON_STD_TXN_VALIDATION_DURATION =
	std::chrono::seconds{1};
/** Default for which scheduler to use. */
static constexpr PTVTaskScheduleStrategy DEFAULT_PTV_TASK_SCHEDULE_STRATEGY = PTVTaskScheduleStrategy::TOPO_SORT;
