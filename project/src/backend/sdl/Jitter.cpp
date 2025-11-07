#ifdef _WIN32
#include "Jitter.h"
#include <windows.h>

// Your existing high-precision timer
extern int64_t getTime();
extern void coolSleepUntil(int64_t);

namespace Jitter {
    int64_t samples[CAP] = {};  // signed microsecond error: actual - target
    int index = 0;

    void sleepAndRecord(int64_t targetUs) {
        // Ask the system to wake us at a precise moment
        coolSleepUntil(targetUs);

        // Measure actual wake moment
        int64_t actual = getTime();

        // Record the sin of the scheduler
        samples[index] = actual - targetUs;
        index = (index + 1) & (CAP - 1); // fast mod 1024 wrap
    }

    void reset() {
        for (int i = 0; i < CAP; i++) samples[i] = 0;
        index = 0;
    }
}
#endif
