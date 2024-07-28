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