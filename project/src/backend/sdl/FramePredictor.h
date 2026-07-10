// ============================================================================
// FramePredictor.h — frame rate predictor with background thread and
// atomic isTime flag for the main loop.
//
// DESIGN (simplified — no swap, no glFinish, no DWM/DRM/Choreographer):
//
// 1. INIT — caller passes a frameRate (Hz). The predictor captures
//    getTime10ns2() as the anchor and computes framePeriod10ns from
//    the rate. No calibration swaps, no vblank polling.
//
// 2. PREDICTION — a background thread walks forward by framePeriod10ns
//    each frame, sleeping until just before the next predicted frame
//    boundary, then spinning the last ~50µs for precision. When the
//    boundary arrives:
//      - Computes exact frameTime = thisFrame - lastFrame
//      - Sets isTime = true
//      - Waits for the main loop to consume (Consume clears the flag)
//
// 3. MAIN LOOP — spins on isTime(). When true:
//      int64_t ft = predictor.Consume();
//      dispatch UPDATE with ft
//      dispatch RENDER with ft
//
// 4. RESYNC — periodically (every ~5s) the main loop calls RequestResync().
//    On Windows the predictor thread re-queries DWM's qpcVBlank and corrects
//    the anchor if crystal drift has accumulated beyond ~500µs.
//
// 5. SETFRAMERATE — call SetFrameRate() to reset the predictor with a new
//    frame period. Useful when the game's target frame rate changes at
//    runtime. The anchor is re-captured to now.
//
// All times are in 10ns units (matching getTime10ns2()).
// ============================================================================

#pragma once

#include <atomic>
#include <thread>
#include <chrono>
#include <cstdint>
#include <cstdio>

#ifdef _WIN32
#  include <windows.h>
#  include <dwmapi.h>
#  pragma comment(lib, "dwmapi.lib")
#endif

#if HX_WINDOWS
static LARGE_INTEGER qpcFrequency2 = {};
#endif

inline int64_t getTime10ns2()
{
#ifdef HX_WINDOWS
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (now.QuadPart * 100000000LL) / qpcFrequency2.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 100000000LL + (ts.tv_nsec / 10LL);
#endif
}

class FramePredictor {
public:
    struct InitResult {
        bool        ok             = false;
        int64_t     anchor10ns     = 0;
        double      refreshRateHz  = 0;
        int64_t     framePeriod10ns = 0;
        const char* error          = nullptr;
    };

    // ---------------------------------------------------------------------
    // Init with a frame rate (Hz). Captures the current time as the anchor
    // and computes the frame period. No calibration, no swaps.
    //
    // Call this on the main thread. Then call Start() to launch the
    // predictor thread.
    // ---------------------------------------------------------------------
    static InitResult Init(double frameRate) {
        InitResult r;

        #ifdef HX_WINDOWS
        QueryPerformanceFrequency(&qpcFrequency2);
        #endif

        if (frameRate < 1.0 || frameRate > 1000.0) {
            r.ok = false;
            r.error = "frame rate out of sane range (1-1000 Hz)";
            return r;
        }

        r.refreshRateHz   = frameRate;
        r.framePeriod10ns = (int64_t)(100000000.0 / frameRate);
        r.anchor10ns      = getTime10ns2();
        r.ok              = true;
        return r;
    }

    // Start the predictor background thread. Returns false if already running.
    bool Start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            return false;

        frameCounter_    = 0;
        lastVblank10ns_  = anchor10ns_;
        nextVblank10ns_.store(anchor10ns_ + framePeriod10ns_, std::memory_order_release);

        thread_ = std::thread([this]{ PredictorLoop(); });
        return true;
    }

    // Stop the predictor background thread. Joins.
    void Stop() {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable())
            thread_.join();
    }

    // ---------------------------------------------------------------------
    // Reset the predictor with a new frame rate. Stops the thread if
    // running, re-captures the anchor, recomputes the period, restarts.
    //
    // Safe to call from the main loop at any time. The next isTime() will
    // fire one frame period after this call returns.
    // ---------------------------------------------------------------------
    void SetFrameRate(double frameRate) {
        if (frameRate < 1.0 || frameRate > 1000.0) return;

        bool wasRunning = running_.load(std::memory_order_acquire);
        if (wasRunning) Stop();

        refreshRateHz_   = frameRate;
        framePeriod10ns_ = (int64_t)(100000000.0 / frameRate);
        anchor10ns_      = getTime10ns2();
        frameCounter_    = 0;
        lastVblank10ns_  = anchor10ns_;
        nextVblank10ns_.store(anchor10ns_ + framePeriod10ns_, std::memory_order_release);

        if (wasRunning) Start();
    }

    // Main-loop interface. Acquire-load keeps the flag visible promptly.
    bool isTime() const noexcept {
        return isTime_.load(std::memory_order_acquire);
    }

    // Consume the pending frame. Returns the exact frame time in 10ns
    // units, or 0 if no frame was pending.
    int64_t Consume() noexcept {
        if (!isTime_.exchange(false, std::memory_order_acq_rel)) {
            return 0;
        }
        return lastFrameTime10ns_.load(std::memory_order_acquire);
    }

    // Configure the predictor with values from Init(). Call after Init()
    // returns ok=true, before Start().
    void Configure(const InitResult& ir) {
        anchor10ns_      = ir.anchor10ns;
        framePeriod10ns_ = ir.framePeriod10ns;
        refreshRateHz_   = ir.refreshRateHz;
    }

    // Request a DWM re-anchoring on the next predictor iteration.
    void RequestResync() noexcept {
        resyncRequested_.store(true, std::memory_order_release);
    }

    // Diagnostics
    int64_t  NextVblank10ns()   const noexcept { return nextVblank10ns_.load(std::memory_order_acquire); }
    int64_t  Anchor10ns()       const noexcept { return anchor10ns_; }
    int64_t  FramePeriod10ns()  const noexcept { return framePeriod10ns_; }
    double   RefreshRateHz()    const noexcept { return refreshRateHz_; }
    uint64_t FramesDispatched() const noexcept { return framesDispatched_.load(std::memory_order_acquire); }
    uint64_t FramesMissed()     const noexcept { return framesMissed_.load(std::memory_order_acquire); }
    uint64_t ResyncsDone()      const noexcept { return resyncsDone_.load(std::memory_order_acquire); }

private:
    void PredictorLoop();
    bool DoResync();

    // --- Thread state ---
    std::atomic<bool> running_{false};
    std::thread       thread_;

    // --- Communication with main loop ---
    std::atomic<bool>      isTime_{false};
    std::atomic<int64_t>   lastFrameTime10ns_{0};
    std::atomic<int64_t>   nextVblank10ns_{0};
    std::atomic<bool>      resyncRequested_{false};

    // --- Counters ---
    std::atomic<uint64_t>  framesDispatched_{0};
    std::atomic<uint64_t>  framesMissed_{0};
    std::atomic<uint64_t>  resyncsDone_{0};

    // --- Predictor-thread-private (no sync needed) ---
    int64_t anchor10ns_       = 0;
    int64_t framePeriod10ns_  = 0;
    double  refreshRateHz_    = 0;
    int64_t frameCounter_     = 0;
    int64_t lastVblank10ns_   = 0;

    // --- Constants ---
    static constexpr int64_t SPIN_WINDOW_10NS      = 5000;    // 50µs spin window
    static constexpr int64_t RESYNC_THRESHOLD_10NS = 50000;  // 500µs drift tolerance
};

// ---------------------------------------------------------------------------
// IMPLEMENTATION
// ---------------------------------------------------------------------------

inline void FramePredictor::PredictorLoop() {
    while (running_.load(std::memory_order_acquire)) {
        // --- Compute next frame boundary on the absolute timeline ---
        int64_t nextVblank = anchor10ns_ + (frameCounter_ + 1) * framePeriod10ns_;
        nextVblank10ns_.store(nextVblank, std::memory_order_release);

        // --- Handle resync request ---
        if (resyncRequested_.exchange(false, std::memory_order_acq_rel)) {
            if (DoResync()) {
                nextVblank = anchor10ns_ + (frameCounter_ + 1) * framePeriod10ns_;
                nextVblank10ns_.store(nextVblank, std::memory_order_release);
            }
        }

        // --- Sleep until SPIN_WINDOW before nextVblank ---
        int64_t now = getTime10ns2();
        int64_t sleepUntil = nextVblank - SPIN_WINDOW_10NS;

        if (now < sleepUntil) {
#ifdef _WIN32
            static HANDLE hTimer = []{
                return CreateWaitableTimerEx(nullptr, nullptr,
                    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                    TIMER_MODIFY_STATE | SYNCHRONIZE);
            }();
            int64_t sleep_ns = (sleepUntil - now) * 10;
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)(sleep_ns / 100);
            if (due.QuadPart == 0) due.QuadPart = -1;
            if (hTimer && SetWaitableTimer(hTimer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(hTimer, INFINITE);
            } else {
                DWORD ms = (DWORD)(sleep_ns / 1000000LL);
                if (ms > 0) Sleep(ms); else Sleep(0);
            }
#else
            struct timespec ts;
            int64_t sleep_ns = (sleepUntil - now) * 10;
            ts.tv_sec  = sleep_ns / 1000000000LL;
            ts.tv_nsec = sleep_ns % 1000000000LL;
            nanosleep(&ts, nullptr);
#endif
        }

        // --- Spin the last SPIN_WINDOW_10NS for precision ---
        while (getTime10ns2() < nextVblank) {
#if defined(_MSC_VER)
            _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
            __asm__ __volatile__("yield" ::: "memory");
#endif
            if (!running_.load(std::memory_order_relaxed)) return;
        }

        // --- Frame boundary arrived. Compute exact frame time. ---
        int64_t actualNow = getTime10ns2();
        int64_t frameTime = actualNow - lastVblank10ns_;
        lastVblank10ns_ = actualNow;

        // --- Publish to main loop ---
        lastFrameTime10ns_.store(frameTime, std::memory_order_release);
        isTime_.store(true, std::memory_order_release);

        frameCounter_++;
        framesDispatched_.fetch_add(1, std::memory_order_acq_rel);

        // --- Wait for main loop to consume ---
        while (isTime_.load(std::memory_order_acquire) &&
               running_.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }

        // --- Miss detection ---
        int64_t afterConsume = getTime10ns2();
        int64_t nextPredicted = anchor10ns_ + (frameCounter_ + 1) * framePeriod10ns_;
        if (afterConsume > nextPredicted) {
            int64_t framesBehind = (afterConsume - nextPredicted) / framePeriod10ns_;
            frameCounter_ += framesBehind;
            framesMissed_.fetch_add((uint64_t)framesBehind, std::memory_order_acq_rel);
        }
    }
}

inline bool FramePredictor::DoResync() {
#ifdef _WIN32
    DWM_TIMING_INFO ti = {};
    ti.cbSize = sizeof(ti);
    if (FAILED(DwmGetCompositionTimingInfo(nullptr, &ti)))
        return false;

    static LARGE_INTEGER qpcFreq = []{
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();

    // --- Query the monitor's actual vblank interval ONCE and cache it. ---
    // If our predictor's framePeriod doesn't match the monitor's refresh
    // rate (within RATE_MATCH_TOLERANCE), re-anchoring to DWM's vblank
    // would yank us onto the WRONG grid — the whole point of decoupling
    // update from render is to let the update grid run at its own rate.
    // So we skip resync entirely when the rates don't match.
    //
    // We measure the monitor's period by sampling qpcVBlank twice with
    // one DwmGetCompositionTimingInfo call between them, then taking the
    // delta. Cached in a static so we only do this once per session.
    static int64_t monitorPeriod10ns = 0;
    static bool monitorPeriodMeasured = false;
    constexpr double RATE_MATCH_TOLERANCE = 0.02;  // 2%

    if (!monitorPeriodMeasured) {
        // Take two samples separated by at least one vblank.
        // Use t1's qpcVBlank as the baseline; if the next call shows a
        // different qpcVBlank, the delta is one refresh period.
        QPC_TIME v1 = ti.qpcVBlank;
        QPC_TIME v2 = v1;
        int tries = 0;
        while (v2 == v1 && tries < 1000) {
            if (FAILED(DwmGetCompositionTimingInfo(nullptr, &ti))) break;
            v2 = ti.qpcVBlank;
            tries++;
            _mm_pause();
        }
        if (v2 != v1) {
            int64_t deltaQpc = (int64_t)(v2 - v1);
            monitorPeriod10ns = (deltaQpc * 100000000LL) / qpcFreq.QuadPart;
        }
        monitorPeriodMeasured = true;

        if (monitorPeriod10ns > 0) {
            double monitorHz = 100000000.0 / (double)monitorPeriod10ns;
            double ourHz     = 100000000.0 / (double)framePeriod10ns_;
            double ratio     = ourHz / monitorHz;
            printf("[FramePredictor] monitor ~%.2f Hz, predictor %.2f Hz, ratio %.3f — resync %s\n",
                   monitorHz, ourHz, ratio,
                   (ratio > 1.0 - RATE_MATCH_TOLERANCE && ratio < 1.0 + RATE_MATCH_TOLERANCE)
                       ? "ENABLED" : "DISABLED (rates don't match)");
        }
    }

    // --- If rates don't match, do NOT re-anchor. ---
    if (monitorPeriod10ns > 0) {
        double ratio = (double)framePeriod10ns_ / (double)monitorPeriod10ns;
        // ratio == 1.0 means rates match; allow 2% tolerance
        if (ratio < 1.0 - RATE_MATCH_TOLERANCE || ratio > 1.0 + RATE_MATCH_TOLERANCE) {
            return false;  // different grids — leave the predictor alone
        }
    }

    // --- Rates match (or we couldn't measure the monitor) — correct drift. ---
    int64_t qpcVBlank10ns = (int64_t)((ti.qpcVBlank * 100000000LL) / qpcFreq.QuadPart);

    int64_t ourLastVblank = anchor10ns_ + frameCounter_ * framePeriod10ns_;
    int64_t drift = qpcVBlank10ns - ourLastVblank;
    if (std::abs(drift) > RESYNC_THRESHOLD_10NS) {
        anchor10ns_ = qpcVBlank10ns - frameCounter_ * framePeriod10ns_;
        resyncsDone_.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }
    return false;
#else
    return false;
#endif
}
