#include "time_consuming.h"

struct timeval MillisToTimeval(int64_t nTimeout)
{
    struct timeval timeout;
    timeout.tv_sec  = nTimeout / 1000;
    timeout.tv_usec = (nTimeout % 1000) * 1000;
    return timeout;
}

struct timeval MillisToTimeval(std::chrono::milliseconds ms)
{
    return MillisToTimeval(count_milliseconds(ms));
}
