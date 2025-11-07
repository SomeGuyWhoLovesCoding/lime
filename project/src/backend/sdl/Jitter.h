#pragma once
#include <cstdint>

#ifdef _WIN32
namespace Jitter {
    constexpr int CAP = 1024;
    extern int64_t samples[CAP];
    extern int index;

    // Call this instead of coolSleepUntil() on Windows.
    void sleepAndRecord(int64_t targetUs);

    // Optional: zero the buffer again
    void reset();
}
#endif
