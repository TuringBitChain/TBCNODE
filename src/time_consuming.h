#pragma once

#include <chrono>

class TimeConsuming
{
    typedef std::chrono::high_resolution_clock  Clock;
    typedef std::chrono::milliseconds           Milliseconds;
    typedef std::chrono::time_point<Clock>      TimePoint;

    public:
        explicit TimeConsuming()
        {
            timingBegin();
        };
        ~TimeConsuming() = default;

        void timingBegin()
        {
            start = std::chrono::high_resolution_clock::now();
        }

        void timingEnded()
        {
            auto end    = std::chrono::high_resolution_clock::now();
            timePeriod  = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        }

        int64_t obtainTimePeriod()
        {
            return timePeriod.count();
        }

    private:
        TimePoint                   start;
        Milliseconds                timePeriod;
};

constexpr int64_t count_seconds(std::chrono::seconds t) { return t.count(); }
constexpr int64_t count_milliseconds(std::chrono::milliseconds t) { return t.count(); }
constexpr int64_t count_microseconds(std::chrono::microseconds t) { return t.count(); }


/**
 * Convert milliseconds to a struct timeval for e.g. select.
 */
struct timeval MillisToTimeval(int64_t nTimeout);

/**
 * Convert milliseconds to a struct timeval for e.g. select.
 */
struct timeval MillisToTimeval(std::chrono::milliseconds ms);
