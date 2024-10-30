// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTILTIME_H
#define BITCOIN_UTILTIME_H

#include <cstdint>
#include <string>
#include <chrono>

/** Mockable clock in the context of tests, otherwise the system clock */
struct NodeClock : public std::chrono::system_clock {
    using time_point = std::chrono::time_point<NodeClock>;
    /** Return current system time or mocked time, if set */
    static time_point now() noexcept;
    static std::time_t to_time_t(const time_point&) = delete; // unused
    static time_point from_time_t(std::time_t) = delete;      // unused
};

/**
 * GetTimeMicros() and GetTimeMillis() both return the system time, but in
 * different units. GetTime() returns the sytem time in seconds, but also
 * supports mocktime, where the time can be specified by the user, eg for
 * testing (eg with the setmocktime rpc, or -mocktime argument).
 *
 * TODO: Rework these functions to be type-safe (so that we don't inadvertently
 * compare numbers with different units, or compare a mocktime to system time).
 */

int64_t GetTime();
int64_t GetTimeMillis();
int64_t GetTimeMicros();
// Like GetTime(), but not mockable
int64_t GetSystemTimeInSeconds();
int64_t GetLogTimeMicros();
void SetMockTime(int64_t nMockTimeIn);
void MilliSleep(int64_t n);

std::string DateTimeStrFormat(const char *pszFormat, int64_t nTime);

/**
 * Return the current time point cast to the given precision. Only use this
 * when an exact precision is needed, otherwise use T::clock::now() directly.
 */
template <typename T>
T Now()
{
    return std::chrono::time_point_cast<typename T::duration>(T::clock::now());
}
/** DEPRECATED, see GetTime */
template <typename T>
T GetTime()
{
    return Now<std::chrono::time_point<NodeClock, T>>().time_since_epoch();
}

#endif // BITCOIN_UTILTIME_H
