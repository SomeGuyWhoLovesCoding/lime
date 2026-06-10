#include <system/CFFI.h>
#include <ui/AsyncKeyEvent.h>
#include <chrono>

#ifdef _WIN32
#include <intrin.h>
#pragma intrinsic(__rdtsc)
#endif

namespace lime {


	ValuePointer* AsyncKeyEvent::callback = 0;
	ValuePointer* AsyncKeyEvent::eventObject = 0;

	static int id_keyCode;
	static int id_state;
	static int id_timestamp;
	static bool init = false;


	AsyncKeyEvent::AsyncKeyEvent () {

		keyCode = 0;
		state = 0;
		timestamp = 0.0;

	}


	void AsyncKeyEvent::Dispatch (AsyncKeyEvent* event) {

		if (AsyncKeyEvent::callback) {

			if (AsyncKeyEvent::eventObject->IsCFFIValue ()) {

				if (!init) {

					id_keyCode = val_id ("keyCode");
					id_state = val_id ("state");
					id_timestamp = val_id ("timestamp");
					init = true;

				}

				value object = (value)AsyncKeyEvent::eventObject->Get ();

				alloc_field (object, id_keyCode, alloc_int (event->keyCode));
				alloc_field (object, id_state, alloc_int (event->state));
				alloc_field (object, id_timestamp, alloc_float (event->timestamp));

			} else {

				AsyncKeyEvent* eventObject = (AsyncKeyEvent*)AsyncKeyEvent::eventObject->Get ();

				eventObject->keyCode = event->keyCode;
				eventObject->state = event->state;
				eventObject->timestamp = event->timestamp;

			}

			AsyncKeyEvent::callback->Call ();

		}

	}

	inline static int64_t getTime10ns()
	{
#ifdef HX_WINDOWS
		LARGE_INTEGER now;
		LARGE_INTEGER qpcFrequency = {};
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&qpcFrequency);
		return (now.QuadPart * 100000000.0f) / qpcFrequency.QuadPart;
#else
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return ts.tv_sec * 100000000.0f + (ts.tv_nsec / 10LL);
#endif
	}
    
    // CPU cycle counter - much faster than QueryPerformanceCounter
    static inline uint64_t rdtsc() {
        #ifdef _WIN32
        return __rdtsc();
        #elif defined(__x86_64__) || defined(__i386__)
        unsigned int lo, hi;
        __asm__ __volatile__("rdtsc" : "=a" (lo), "=d" (hi));
        return ((uint64_t)hi << 32) | lo;
        #elif defined(__aarch64__)
        uint64_t value;
        __asm__ __volatile__("mrs %0, cntvct_el0" : "=r" (value));
        return value;
        #else
        return std::chrono::steady_clock::now().time_since_epoch().count();
        #endif
    }
    
    // CPU frequency calibration (run once at startup)
    static uint64_t cpuFrequency = 0;
    
    static void calibrateCpuFrequency() {
        #ifdef _WIN32
        LARGE_INTEGER freq, start, end;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
        uint64_t startCycles = rdtsc();
        Sleep(100);
        QueryPerformanceCounter(&end);
        uint64_t endCycles = rdtsc();
        double seconds = (end.QuadPart - start.QuadPart) / (double)freq.QuadPart;
        cpuFrequency = (uint64_t)((endCycles - startCycles) / seconds);
        #else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t startCycles = rdtsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t endCycles = rdtsc();
        double seconds = ts.tv_sec + ts.tv_nsec / 1e9;
        cpuFrequency = (uint64_t)((endCycles - startCycles) / seconds);
        #endif
    }
    
    double AsyncKeyEvent::Timestamp() {
        if (cpuFrequency == 0) calibrateCpuFrequency();
        
        uint64_t rdtscValue = rdtsc();
        
        // Convert to nanoseconds using 64-bit split multiplication
        // rdtscValue * 1,000,000,000 / cpuFrequency
        const uint64_t NANOSECONDS = 1000000000ULL;
        
        // Split into 32-bit parts for multiplication
        uint32_t low = (uint32_t)(rdtscValue & 0xFFFFFFFF);
        uint32_t high = (uint32_t)(rdtscValue >> 32);
        
        // Multiply each part
        uint64_t lowPart = (uint64_t)low * NANOSECONDS;
        uint64_t highPart = (uint64_t)high * NANOSECONDS;
        
        // Combine: highPart << 32 + lowPart (but without shifting > 63 bits)
        // We'll handle division first
        
        // Divide highPart by cpuFrequency first to keep numbers manageable
        if (cpuFrequency == 0) return 0.0;
        
        uint64_t highQuotient = highPart / cpuFrequency;
        uint64_t highRemainder = highPart % cpuFrequency;
        
        // Combine remainder with lowPart: (highRemainder << 32) + lowPart
        uint64_t combinedLow = (highRemainder << 32) + (lowPart >> 32);
        uint64_t combinedLowRemainder = lowPart & 0xFFFFFFFF;
        
        uint64_t lowQuotient = combinedLow / cpuFrequency;
        uint64_t lowRemainder = combinedLow % cpuFrequency;
        
        // Final quotient: highQuotient << 32 + lowQuotient
        double seconds = (double)(highQuotient << 32) / NANOSECONDS;
        seconds += (double)lowQuotient / NANOSECONDS;
        
        return seconds;
    }


}