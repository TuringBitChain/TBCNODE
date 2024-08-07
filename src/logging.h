// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2017-2018 The Bitcoin developers
// Copyright (c) 2019 Bitcoin Association
// Distributed under the Open TBC software license, see the accompanying file LICENSE.

#ifndef BITCOIN_LOGGING_H
#define BITCOIN_LOGGING_H

#include <atomic>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>

#include "tinyformat.h"

static const bool DEFAULT_LOGTIMEMICROS = false;
static const bool DEFAULT_LOGIPS = false;
static const bool DEFAULT_LOGTIMESTAMPS = true;

extern bool fLogIPs;

namespace BCLog {

enum LogFlags : uint32_t {
    NONE = 0,
    NET = (1 << 0),
    TOR = (1 << 1),
    MEMPOOL = (1 << 2),
    HTTP = (1 << 3),
    BENCH = (1 << 4),
    ZMQ = (1 << 5),
    DB = (1 << 6),
    RPC = (1 << 7),
    ADDRMAN = (1 << 9),
    SELECTCOINS = (1 << 10),
    REINDEX = (1 << 11),
    CMPCTBLOCK = (1 << 12),
    RAND = (1 << 13),
    PRUNE = (1 << 14),
    PROXY = (1 << 15),
    MEMPOOLREJ = (1 << 16),
    LIBEVENT = (1 << 17),
    COINDB = (1 << 18),
    LEVELDB = (1 << 20),
    TXNPROP = (1 << 21),
    TXNSRC = (1 << 22),
    JOURNAL = (1 << 23),
    TXNVAL = (1 << 24),
    ALL = ~uint32_t(0),
};

class Logger {
private:
    FILE *fileout = nullptr;
    std::mutex mutexDebugLog;
    std::list<std::string> vMsgsBeforeOpenLog;

    /**
     * fStartedNewLine is a state variable that will suppress printing of the
     * timestamp when multiple calls are made that don't end in a newline.
     */
    std::atomic_bool fStartedNewLine{true};

    /**
     * Log categories bitfield. Leveldb/libevent need special handling if their
     * flags are changed at runtime.
     */
    std::atomic<typename std::underlying_type<LogFlags>::type> logCategories{0};

    std::string LogTimestampStr(const std::string &str);

public:
    bool fPrintToConsole = false;
    bool fPrintToDebugLog = true;

    bool fLogTimestamps = DEFAULT_LOGTIMESTAMPS;
    bool fLogTimeMicros = DEFAULT_LOGTIMEMICROS;

    std::atomic<bool> fReopenDebugLog{false};

    ~Logger();

    /** Send a string to the log output */
    int LogPrintStr(const std::string &str);

    bool OpenDebugLog();
    void ShrinkDebugFile();

    void EnableCategory(LogFlags category);
    void DisableCategory(LogFlags category);

    /** Return true if log accepts specified category */
    bool WillLogCategory(typename std::underlying_type<LogFlags>::type category) const;

    /** Default for whether ShrinkDebugFile should be run */
    bool DefaultShrinkDebugFile() const;
};

} // namespace BCLog

BCLog::Logger &GetLogger();

/** Return true if log accepts one of the specified categories */
static inline bool LogAcceptCategory(typename std::underlying_type<BCLog::LogFlags>::type categories) {
    return GetLogger().WillLogCategory(categories);
}

/** Returns a string with the supported log categories */
std::string ListLogCategories();

/** Return true if str parses as a log category and set the flag */
bool GetLogCategory(BCLog::LogFlags &flag, const std::string &str);

#define LogPrint(category, ...)                                                \
    do {                                                                       \
        if (LogAcceptCategory((category))) {                                   \
            GetLogger().LogPrintStr(tfm::format(__VA_ARGS__));                 \
        }                                                                      \
    } while (0)

#define LogPrintf(...)                                                         \
    do {                                                                       \
        GetLogger().LogPrintStr(tfm::format(__VA_ARGS__));                     \
    } while (0)

#endif // BITCOIN_LOGGING_H
