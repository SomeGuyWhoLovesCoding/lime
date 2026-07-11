// ============================================================================
// FramePredictor.h — synchronous frame rate predictor.
//
// DESIGN (synchronous — no background thread, no atomics):
//
// 1. INIT — caller passes a frameRate (Hz). The predictor blocks until the
//    next real vblank and uses that timestamp as the anchor. No calibration
//    swaps, no glFinish.
//
// 2. PREDICTION — the main loop calls PredictorRun() each frame. It sleeps
//    until just before the next predicted frame boundary, spins the last
//    ~50µs for precision, then returns the exact frame time:
//      - Computes exact frameTime = thisFrame - lastFrame
//      - Advances frame counter
//      - Returns frameTime (10ns units)
//
// 3. RESYNC — call RequestResync() from the main loop periodically. The
//    next PredictorRun() call re-queries the platform's vblank source and
//    corrects the anchor if crystal drift > ~500µs. Only fires when the
//    predictor's rate matches the monitor's rate.
//
// 4. SETFRAMERATE — call SetFrameRate() to reset with a new frame rate.
//    Re-anchors to the next vblank.
//
// All times are in 10ns units (matching getTime10ns2()).
// ============================================================================

#pragma once

#include <atomic>
#include <thread>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cmath>

#ifdef _WIN32
#  include <windows.h>
#  include <dwmapi.h>
#  pragma comment(lib, "dwmapi.lib")
#elif defined(__linux__) && !defined(__ANDROID__)
#  include <fcntl.h>
#  include <unistd.h>
#  include <poll.h>
#  include <dlfcn.h>
#  include <mutex>
#  include <condition_variable>
#  include <cstring>
#elif defined(__ANDROID__)
#  include <android/choreographer.h>
#  include <mutex>
#  include <condition_variable>
#endif

#if HX_WINDOWS
static LARGE_INTEGER qpcFrequency2 = {};
#endif

// 1 second = 100000000 ticks of 10ns
static constexpr int64_t TICKS_PER_SECOND_10NS = 100000000LL;

inline int64_t getTime10ns2()
{
#ifdef HX_WINDOWS
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (qpcFrequency2.QuadPart == 0)
        QueryPerformanceFrequency(&qpcFrequency2);
    int64_t wholeSec = now.QuadPart / qpcFrequency2.QuadPart;
    int64_t rem      = now.QuadPart % qpcFrequency2.QuadPart;
    return wholeSec * TICKS_PER_SECOND_10NS + (rem * TICKS_PER_SECOND_10NS) / qpcFrequency2.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * TICKS_PER_SECOND_10NS + (ts.tv_nsec / 10LL);
#endif
}

// ============================================================================
// VblankSource — cross-platform vblank measurement.
//
// Provides two functions:
//   GetMonitorPeriod10ns() — the monitor's actual refresh interval
//   GetCurrentVblank10ns() — timestamp of the most recent vblank (may block)
//
// Platform implementations:
//   Windows : DWM (DwmGetCompositionTimingInfo)
//   Linux   : DRM (drmWaitVBlank on /dev/dri/card0)
//   Android : AChoreographer (postFrameCallback + condition variable)
// ============================================================================

namespace VblankSource {

#ifdef _WIN32

inline int64_t GetMonitorPeriod10ns() {
    DWM_TIMING_INFO ti = {};
    ti.cbSize = sizeof(ti);
    if (FAILED(DwmGetCompositionTimingInfo(nullptr, &ti)))
        return 0;
    if (ti.qpcRefreshPeriod == 0) return 0;
    if (qpcFrequency2.QuadPart == 0)
        QueryPerformanceFrequency(&qpcFrequency2);
    return (int64_t)((ti.qpcRefreshPeriod * TICKS_PER_SECOND_10NS) / qpcFrequency2.QuadPart);
}

inline int64_t GetCurrentVblank10ns() {
    DWM_TIMING_INFO ti = {};
    ti.cbSize = sizeof(ti);
    if (FAILED(DwmGetCompositionTimingInfo(nullptr, &ti)))
        return 0;
    static LARGE_INTEGER freq = []{
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f;
    }();
    return (int64_t)((ti.qpcVBlank * TICKS_PER_SECOND_10NS) / freq.QuadPart);
}

// Blocks until the next vblank. Polls DWM's qpcVBlank until it advances.
// At 60Hz this is at most ~17 Sleep(1) calls. Returns the vblank timestamp
// in 10ns units, or 0 on failure.
inline int64_t WaitForNextVblank10ns() {
    if (qpcFrequency2.QuadPart == 0)
        QueryPerformanceFrequency(&qpcFrequency2);
    DwmFlush();
    return getTime10ns2();
}

#elif defined(__linux__) && !defined(__ANDROID__)

// ============================================================================
// Linux: Wayland (primary) + DRM (fallback) implementation.
//
// Wayland uses wl_surface_frame callbacks, dispatched by wl_display_dispatch.
// The callback's `time` parameter is CLOCK_MONOTONIC in milliseconds, which
// matches getTime10ns2() after conversion.
//
// DRM uses drmWaitVBlank (kernel-level blocking).
//
// SetWaylandSurface() must be called once at startup (from the main thread)
// with the wl_surface and wl_display pointers. If not called, or if
// libwayland-client is unavailable, falls back to DRM.
// ============================================================================

// --- Minimal Wayland type definitions (no libwayland-dev dependency) ---
struct wl_surface;
struct wl_callback;
struct wl_callback_listener {
    void (*done)(void* data, struct wl_callback* callback, uint32_t time);
};
typedef struct wl_callback* (*wl_surface_frame_t)(struct wl_surface*);
typedef int (*wl_callback_add_listener_t)(struct wl_callback*, const struct wl_callback_listener*, void*);
typedef void (*wl_callback_destroy_t)(struct wl_callback*);
typedef int (*wl_display_dispatch_t)(void* display);
typedef int (*wl_display_flush_t)(void* display);
typedef int (*wl_display_get_fd_t)(void* display);

// --- Wayland state ---
struct WaylandState {
    void* lib_handle = nullptr;
    void* display = nullptr;
    wl_surface* surface = nullptr;
    wl_callback_listener listener = {};
    wl_surface_frame_t p_frame = nullptr;
    wl_callback_add_listener_t p_add_listener = nullptr;
    wl_callback_destroy_t p_destroy = nullptr;
    wl_display_dispatch_t p_dispatch = nullptr;
    wl_display_flush_t p_flush = nullptr;
    wl_display_get_fd_t p_get_fd = nullptr;
    std::mutex mtx;
    int64_t lastFrameTime10ns = 0;
    bool fired = false;
    bool loaded = false;
    bool available = false;
};

inline WaylandState& GetWaylandState() {
    static WaylandState state;
    return state;
}

static void WaylandFrameCallback(void* data, struct wl_callback* callback, uint32_t time) {
    (void)data;
    WaylandState& s = GetWaylandState();
    // time is milliseconds on CLOCK_MONOTONIC — convert to 10ns units
    int64_t time10ns = (int64_t)time * 100000LL;
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        s.lastFrameTime10ns = time10ns;
        s.fired = true;
    }
    // One-shot callback — destroy it
    if (s.p_destroy) s.p_destroy(callback);
}

inline bool InitWayland() {
    WaylandState& s = GetWaylandState();
    if (s.loaded) return s.available;
    s.loaded = true;

    s.lib_handle = dlopen("libwayland-client.so.0", RTLD_LAZY);
    if (!s.lib_handle) return false;

    s.p_frame        = (wl_surface_frame_t)dlsym(s.lib_handle, "wl_surface_frame");
    s.p_add_listener = (wl_callback_add_listener_t)dlsym(s.lib_handle, "wl_callback_add_listener");
    s.p_destroy      = (wl_callback_destroy_t)dlsym(s.lib_handle, "wl_callback_destroy");
    s.p_dispatch     = (wl_display_dispatch_t)dlsym(s.lib_handle, "wl_display_dispatch");
    s.p_flush        = (wl_display_flush_t)dlsym(s.lib_handle, "wl_display_flush");
    s.p_get_fd       = (wl_display_get_fd_t)dlsym(s.lib_handle, "wl_display_get_fd");

    if (!s.p_frame || !s.p_add_listener || !s.p_destroy || !s.p_dispatch || !s.p_flush || !s.p_get_fd) {
        dlclose(s.lib_handle);
        s.lib_handle = nullptr;
        return false;
    }

    s.listener.done = WaylandFrameCallback;
    return true;
}

// Called by the app (from the main thread) to provide the Wayland surface
// and display. Must be called before FramePredictor::Init().
inline void SetWaylandSurface(void* surface, void* display) {
    if (!InitWayland()) return;
    WaylandState& s = GetWaylandState();
    s.surface = (wl_surface*)surface;
    s.display = display;
    s.available = (s.surface && s.display);
    if (s.available) {
        printf("[FramePredictor] Wayland vblank source initialized\n");
    }
}

inline bool IsWaylandAvailable() {
    return GetWaylandState().available;
}

// Blocks until the next Wayland frame callback fires, with a 200ms timeout.
// MUST be called from the main thread (Wayland display is not thread-safe).
// Returns 0 on timeout/failure (caller falls back to DRM or getTime10ns2).
inline int64_t WaitWaylandFrame10ns() {
    WaylandState& s = GetWaylandState();
    if (!s.available) return 0;

    {
        std::lock_guard<std::mutex> lk(s.mtx);
        s.fired = false;
    }

    // Post a one-shot frame callback
    wl_callback* cb = s.p_frame(s.surface);
    if (!cb) return 0;
    s.p_add_listener(cb, &s.listener, nullptr);
    s.p_flush(s.display);

    // --- Wait for the callback with a 200ms timeout ---
    // Use poll() on the display fd instead of blocking wl_display_dispatch.
    // Without this timeout, wl_display_dispatch blocks FOREVER if the
    // surface isn't mapped yet or the compositor hasn't sent a frame event.
    int fd = s.p_get_fd(s.display);
    int64_t deadline = getTime10ns2() + 20000000;  // 200ms in 10ns units

    while (!s.fired) {
        int64_t now = getTime10ns2();
        if (now > deadline) break;  // timed out

        int remaining_ms = (int)((deadline - now) / 100000);  // 10ns -> ms
        if (remaining_ms <= 0) remaining_ms = 1;

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, remaining_ms);
        if (pr < 0) break;   // error
        if (pr == 0) break;  // timed out

        // Data available — dispatch events (non-blocking since we know
        // data is ready, but wl_display_dispatch may still block if
        // the event isn't ours. Use wl_display_dispatch_pending first
        // to process already-queued events, then dispatch for new ones.)
        if (s.p_dispatch(s.display) < 0) break;
    }

    std::lock_guard<std::mutex> lk(s.mtx);
    if (!s.fired) return 0;  // timed out
    return s.lastFrameTime10ns;
}

// Non-blocking: returns the timestamp of the most recent Wayland callback.
// Safe to call from any thread.
inline int64_t GetLastWaylandVblank10ns() {
    WaylandState& s = GetWaylandState();
    std::lock_guard<std::mutex> lk(s.mtx);
    return s.lastFrameTime10ns;
}

// --- DRM dynamic loading (no compile-time libdrm-dev dependency) ---

// Minimal type stubs matching libdrm's stable ABI.
// Only the fields we access are defined; layouts match xf86drmMode.h.

constexpr int DRM_MODE_CONNECTED = 1;
constexpr int DRM_VBLANK_RELATIVE = 0x1;
constexpr int DRM_VBLANK_HIGH_CRTC_SHIFT = 1;

struct DrmModeRes {
    int count_fbs;
    uint32_t* fbs;
    int count_crtcs;
    uint32_t* crtcs;
    int count_connectors;
    uint32_t* connectors;
    int count_encoders;
    uint32_t* encoders;
    uint32_t min_width, max_width;
    uint32_t min_height, max_height;
};

struct DrmModeConnector {
    uint32_t connector_id;
    uint32_t encoder_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    int connection;       // enum drmModeConnection
    int count_modes;
    void* modes;          // drmModeModeInfo* — we don't access this
    int count_props;
    uint32_t* props;
    uint64_t* prop_values;
    int count_encoders;
    uint32_t* encoders;
    uint32_t mm_width, mm_height;
    int subpixel;
};

struct DrmModeEncoder {
    uint32_t encoder_id;
    uint32_t encoder_type;
    uint32_t crtc_id;
    uint32_t possible_crtcs;
};

// drmVBlank request union — matches the bitfield layout
struct DrmVBlankRequest {
    union {
        int type;
        struct {
            unsigned type_     : 1;
            unsigned sequence_ : 1;
            unsigned signal_   : 30;
        };
    };
};

struct DrmVBlank {
    DrmVBlankRequest request;
    unsigned long sequence;
    unsigned long signal;
    unsigned int high_crtc;
};

// Function pointer types
typedef DrmModeRes* (*DrmModeGetResources_t)(int fd);
typedef int (*DrmModeFreeResources_t)(DrmModeRes* ptr);
typedef DrmModeConnector* (*DrmModeGetConnector_t)(int fd, uint32_t connectorId);
typedef int (*DrmModeFreeConnector_t)(DrmModeConnector* ptr);
typedef DrmModeEncoder* (*DrmModeGetEncoder_t)(int fd, uint32_t encoderId);
typedef int (*DrmModeFreeEncoder_t)(DrmModeEncoder* ptr);
typedef int (*DrmWaitVBlank_t)(int fd, DrmVBlank* vbl);

struct DrmState {
    int fd = -1;
    uint32_t crtcId = 0;
    void* lib_handle = nullptr;
    DrmModeGetResources_t  p_getResources  = nullptr;
    DrmModeFreeResources_t p_freeResources = nullptr;
    DrmModeGetConnector_t  p_getConnector  = nullptr;
    DrmModeFreeConnector_t p_freeConnector = nullptr;
    DrmModeGetEncoder_t    p_getEncoder    = nullptr;
    DrmModeFreeEncoder_t   p_freeEncoder   = nullptr;
    DrmWaitVBlank_t        p_waitVBlank    = nullptr;
    bool loaded = false;
    bool available = false;
};

inline DrmState& GetDrmState() {
    static DrmState state;
    return state;
}

inline bool LoadDrmDynamically() {
    DrmState& s = GetDrmState();
    if (s.loaded) return s.available;
    s.loaded = true;

    s.lib_handle = dlopen("libdrm.so.2", RTLD_LAZY);
    if (!s.lib_handle) {
        // Try unversioned fallback
        s.lib_handle = dlopen("libdrm.so", RTLD_LAZY);
    }
    if (!s.lib_handle) return false;

    s.p_getResources  = (DrmModeGetResources_t)dlsym(s.lib_handle, "drmModeGetResources");
    s.p_freeResources = (DrmModeFreeResources_t)dlsym(s.lib_handle, "drmModeFreeResources");
    s.p_getConnector  = (DrmModeGetConnector_t)dlsym(s.lib_handle, "drmModeGetConnector");
    s.p_freeConnector = (DrmModeFreeConnector_t)dlsym(s.lib_handle, "drmModeFreeConnector");
    s.p_getEncoder    = (DrmModeGetEncoder_t)dlsym(s.lib_handle, "drmModeGetEncoder");
    s.p_freeEncoder   = (DrmModeFreeEncoder_t)dlsym(s.lib_handle, "drmModeFreeEncoder");
    s.p_waitVBlank    = (DrmWaitVBlank_t)dlsym(s.lib_handle, "drmWaitVBlank");

    if (!s.p_getResources || !s.p_freeResources || !s.p_getConnector ||
        !s.p_freeConnector || !s.p_getEncoder || !s.p_freeEncoder || !s.p_waitVBlank) {
        dlclose(s.lib_handle);
        s.lib_handle = nullptr;
        return false;
    }

    s.available = true;
    return true;
}

inline bool InitDrm() {
    DrmState& s = GetDrmState();
    if (s.fd >= 0) return true;
    if (!LoadDrmDynamically()) return false;

    for (int i = 0; i < 4; ++i) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        s.fd = open(path, O_RDWR | O_CLOEXEC);
        if (s.fd < 0) continue;

        DrmModeRes* res = s.p_getResources(s.fd);
        if (!res) { close(s.fd); s.fd = -1; continue; }

        for (int c = 0; c < res->count_connectors; ++c) {
            DrmModeConnector* conn = s.p_getConnector(s.fd, res->connectors[c]);
            if (!conn) continue;
            if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
                DrmModeEncoder* enc = s.p_getEncoder(s.fd, conn->encoder_id);
                if (enc) {
                    s.crtcId = enc->crtc_id;
                    s.p_freeEncoder(enc);
                    s.p_freeConnector(conn);
                    s.p_freeResources(res);
                    return true;
                }
                if (res->count_crtcs > 0) {
                    s.crtcId = res->crtcs[0];
                    s.p_freeConnector(conn);
                    s.p_freeResources(res);
                    return true;
                }
            }
            s.p_freeConnector(conn);
        }
        s.p_freeResources(res);
        close(s.fd);
        s.fd = -1;
    }
    return false;
}

inline int64_t WaitVblank10ns() {
    DrmState& s = GetDrmState();
    if (s.fd < 0 && !InitDrm()) return 0;
    if (s.crtcId == 0) return 0;

    DrmVBlank vbl;
    memset(&vbl, 0, sizeof(vbl));
    // The request union's `type` member is an int that packs:
    //   bit 0     = DRM_VBLANK_RELATIVE / ABSOLUTE
    //   bit 1+    = high CRTC bits (DRM_VBLANK_HIGH_CRTC_SHIFT = 1)
    // The sequence and signal fields are separate members of drmVBlank,
    // NOT part of the request union.
    vbl.request.type = DRM_VBLANK_RELATIVE;
    vbl.request.type |= (int)(s.crtcId << DRM_VBLANK_HIGH_CRTC_SHIFT);
    vbl.sequence = 1;    // wait for 1 vblank
    vbl.signal   = 0;

    if (s.p_waitVBlank(s.fd, &vbl) != 0)
        return 0;

    return getTime10ns2();
}

// --- Unified Linux functions: Wayland first, DRM fallback ---

inline int64_t GetMonitorPeriod10ns() {
    if (IsWaylandAvailable()) {
        int64_t t1 = WaitWaylandFrame10ns();
        if (t1 > 0) {
            int64_t t2 = WaitWaylandFrame10ns();
            if (t2 > 0) return t2 - t1;
        }
        // Fall through to DRM on failure
    }
    int64_t t1 = WaitVblank10ns();
    if (t1 == 0) return 0;
    int64_t t2 = WaitVblank10ns();
    if (t2 == 0) return 0;
    return t2 - t1;
}

inline int64_t WaitForNextVblank10ns() {
    if (IsWaylandAvailable()) {
        int64_t t = WaitWaylandFrame10ns();
        if (t > 0) return t;
        // Fall through to DRM on failure
    }
    return WaitVblank10ns();
}

// For DoResync (called from predictor thread):
// On Wayland, returns the last callback timestamp (non-blocking, may be
// up to one frame stale). On DRM, blocks until the next vblank.
inline int64_t GetCurrentVblank10ns() {
    if (IsWaylandAvailable()) {
        return GetLastWaylandVblank10ns();
    }
    return WaitVblank10ns();
}

#elif defined(__ANDROID__)

// --- Android AChoreographer implementation ---

struct ChoreoState {
    std::mutex mtx;
    std::condition_variable cv;
    int64_t frameTimeNanos = 0;
    bool fired = false;
};

inline ChoreoState& GetChoreoState() {
    static ChoreoState state;
    return state;
}

static void ChoreoCallback(long frameTimeNanos, void* data) {
    (void)data;
    ChoreoState& s = GetChoreoState();
    std::lock_guard<std::mutex> lk(s.mtx);
    s.frameTimeNanos = frameTimeNanos;
    s.fired = true;
    s.cv.notify_one();
}

// Blocks until the next choreographer frame callback fires.
// Returns the frameTimeNanos from the callback (in 10ns units).
inline int64_t WaitChoreoFrame10ns() {
    AChoreographer* choreo = AChoreographer_getInstance();
    if (!choreo) return 0;

    ChoreoState& s = GetChoreoState();
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        s.fired = false;
    }
    AChoreographer_postFrameCallback(choreo, ChoreoCallback, nullptr);
    std::unique_lock<std::mutex> lk(s.mtx);
    s.cv.wait(lk, []{ return GetChoreoState().fired; });
    return s.frameTimeNanos / 10;  // nanos -> 10ns units
}

inline int64_t GetMonitorPeriod10ns() {
    int64_t t1 = WaitChoreoFrame10ns();
    if (t1 == 0) return 0;
    int64_t t2 = WaitChoreoFrame10ns();
    if (t2 == 0) return 0;
    return t2 - t1;
}

inline int64_t GetCurrentVblank10ns() {
    return WaitChoreoFrame10ns();
}

// On Android, GetCurrentVblank10ns already blocks until the next
// choreographer frame (= vsync), so WaitForNextVblank10ns is the same.
inline int64_t WaitForNextVblank10ns() {
    return WaitChoreoFrame10ns();
}

#else

// --- Fallback: no vblank source available ---

inline int64_t GetMonitorPeriod10ns() { return 0; }
inline int64_t GetCurrentVblank10ns() { return 0; }
inline int64_t WaitForNextVblank10ns() { return 0; }

#endif

} // namespace VblankSource
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
    // Init with a frame rate (Hz). BLOCKS until the next real vblank and
    // uses that timestamp as the anchor. This ensures the predictor's grid
    // is phase-locked to the monitor's vblank -- critical when running at
    // a multiple of the monitor rate (e.g., 120Hz update on 60Hz monitor:
    // every 2nd frame lands exactly on vblank).
    //
    // Call this on the main thread. Then call Configure() -- no Start()
    // needed (synchronous design).
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

        // --- Anchor to a real vblank ---
        int64_t vblankAnchor = VblankSource::WaitForNextVblank10ns();
        if (vblankAnchor > 0) {
            r.anchor10ns = vblankAnchor;
        } else {
            r.anchor10ns = getTime10ns2();  // fallback
            printf("[FramePredictor] vblank source unavailable, using getTime10ns2() as anchor\n");
        }
        r.ok = true;
        return r;
    }

    // Configure the predictor with values from Init(). Call after Init()
    // returns ok=true. Resets internal state for a fresh start.
    void Configure(const InitResult& ir) {
        anchor10ns_      = ir.anchor10ns;
        framePeriod10ns_ = ir.framePeriod10ns;
        refreshRateHz_   = ir.refreshRateHz;
        frameCounter_    = 0;
        lastVblank10ns_  = anchor10ns_;
        framesDispatched_ = 0;
        framesMissed_     = 0;
        resyncsDone_      = 0;
        resyncRequested_  = false;
        nextVblank10ns_   = anchor10ns_ + framePeriod10ns_;
    }

    // ---------------------------------------------------------------------
    // Synchronous predictor run. Called from the main loop each frame.
    //
    // Blocks until the next predicted frame boundary, then returns the
    // exact frame time in 10ns units. Returns 0 on error.
    //
    // The main loop should:
    //   1. Call PredictorRun()
    //   2. If frameTime > 0, dispatch UPDATE + RENDER with frameTime
    //   3. Loop back to step 1
    // ---------------------------------------------------------------------
    int64_t PredictorRun() {
        // --- Compute next frame boundary on the absolute timeline ---
        int64_t nextVblank = anchor10ns_ + (frameCounter_ + 1) * framePeriod10ns_;
        nextVblank10ns_ = nextVblank;

        // --- Handle resync request ---
        if (resyncRequested_) {
            resyncRequested_ = false;
            if (DoResync()) {
                nextVblank = anchor10ns_ + (frameCounter_ + 1) * framePeriod10ns_;
                nextVblank10ns_ = nextVblank;
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
                // Finite timeout prevents permanent freeze if timer fails
                DWORD timeoutMs = (DWORD)(sleep_ns / 1000000LL) + 50;
                WaitForSingleObject(hTimer, timeoutMs);
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
        }

        // --- Frame boundary arrived. Compute exact frame time. ---
        int64_t actualNow = getTime10ns2();
        int64_t frameTime = actualNow - lastVblank10ns_;
        lastVblank10ns_ = actualNow;

        frameCounter_++;
        framesDispatched_++;

        // --- Miss detection: did we fall behind? ---
        int64_t nextPredicted = anchor10ns_ + (frameCounter_ + 1) * framePeriod10ns_;
        if (actualNow > nextPredicted) {
            int64_t framesBehind = (actualNow - nextPredicted) / framePeriod10ns_;

            if (framesBehind > 10) {
                // Absurd gap (minimize, breakpoint, suspend, etc.)
                // Don't try to skip millions of frames — just re-anchor
                // to right now and keep going.

                // dispatched=17007 missed=63087970 resyncs=56 i mean seriously? that's not a flaw. it's an honest help me
                anchor10ns_      = actualNow - (frameCounter_ + 1) * framePeriod10ns_;
                lastVblank10ns_  = actualNow;
                frameTime        = framePeriod10ns_;  // pretend it was a normal frame
            } else {
                frameCounter_ += framesBehind;
                framesMissed_ += (uint64_t)framesBehind;
            }
        }

        #ifdef HX_WINDOWS
        {
            DWM_TIMING_INFO ti = {};
            ti.cbSize = sizeof(ti);
            if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.qpcVBlank != 0) {
                static LARGE_INTEGER freq = []{
                    LARGE_INTEGER f;
                    QueryPerformanceFrequency(&f);
                    return f;
                }();

                // --- Guard: 1:1 rate check ---
                // Monitor period in 10ns from DWM's reported refresh rate.
                // Skip PLL at 2:1 (120fps/60Hz) where phase doesn't matter.
                int64_t monitorPeriod10ns = (int64_t)(
                    (ti.rateRefresh.uiDenominator * TICKS_PER_SECOND_10NS) /
                    ti.rateRefresh.uiNumerator
                );
                if (monitorPeriod10ns > 0) {
                    int64_t periodDiff = framePeriod10ns_ > monitorPeriod10ns
                        ? framePeriod10ns_ - monitorPeriod10ns
                        : monitorPeriod10ns - framePeriod10ns_;

                    if (periodDiff <= monitorPeriod10ns / 10) {
                        // --- Phase comparison ---
                        // Overflow-safe QPC → 10ns (same two-step as getTime10ns2)
                        int64_t qpcVal   = (int64_t)ti.qpcVBlank;
                        int64_t wholeSec = qpcVal / freq.QuadPart;
                        int64_t rem      = qpcVal % freq.QuadPart;
                        int64_t realVblank10ns = wholeSec * TICKS_PER_SECOND_10NS
                            + (rem * TICKS_PER_SECOND_10NS) / freq.QuadPart;

                        int64_t ourVblank10ns = anchor10ns_
                            + frameCounter_ * framePeriod10ns_;
                        int64_t phaseError    = ourVblank10ns - realVblank10ns;

                        // --- Phase bias: target being ~1ms EARLY ---
                        // DWM composites slightly before vblank. If we present
                        // right at vblank, DWM already grabbed the old frame.
                        // We want phaseError to be slightly negative (early).
                        int64_t targetLead    = (TICKS_PER_SECOND_10NS / 1000) * 1; // 1ms in 10ns units
                        int64_t excessPhase   = phaseError + targetLead;

                        // --- Guard: unidirectional only ---
                        // Only correct when we're LATER than our target lead.
                        // Never shift anchor forward (being early is harmless).
                        if (excessPhase > 0) {
                            // --- Guard: cap per-frame correction to 5% of period ---
                            int64_t maxCorrection = framePeriod10ns_ / 20;
                            int64_t correction = excessPhase / 8;
                            if (correction > maxCorrection)
                                correction = maxCorrection;

                            anchor10ns_ -= correction;

                            // --- Guard: next target must stay in the future ---
                            int64_t nextTarget = anchor10ns_
                                + (frameCounter_ + 1) * framePeriod10ns_;
                            if (nextTarget <= actualNow) {
                                anchor10ns_ += correction; // revert
                            }
                        }
                    }
                }
            }
        }
        #endif

        return frameTime;
    }

    // ---------------------------------------------------------------------
    // Reset the predictor with a new frame rate. RE-ANCHORS to the next
    // vblank, recomputes the period, resets counters.
    // ---------------------------------------------------------------------
    void SetFrameRate(double frameRate) {
        if (frameRate < 1.0 || frameRate > 1000.0) return;

        refreshRateHz_   = frameRate;
        framePeriod10ns_ = (int64_t)(100000000.0 / frameRate);

        // Re-anchor to a real vblank (same as Init)
        int64_t vblankAnchor = VblankSource::WaitForNextVblank10ns();
        if (vblankAnchor > 0) {
            anchor10ns_ = vblankAnchor;
        } else {
            anchor10ns_ = getTime10ns2();  // fallback
        }
        frameCounter_    = 0;
        lastVblank10ns_  = anchor10ns_;
        nextVblank10ns_  = anchor10ns_ + framePeriod10ns_;
    }

    // Request a vblank re-anchoring on the next PredictorRun() call.
    void RequestResync() noexcept {
        resyncRequested_ = true;
    }

    // Diagnostics
    int64_t  NextVblank10ns()   const noexcept { return nextVblank10ns_; }
    int64_t  Anchor10ns()       const noexcept { return anchor10ns_; }
    int64_t  FramePeriod10ns()  const noexcept { return framePeriod10ns_; }
    double   RefreshRateHz()    const noexcept { return refreshRateHz_; }
    uint64_t FramesDispatched() const noexcept { return framesDispatched_; }
    uint64_t FramesMissed()     const noexcept { return framesMissed_; }
    uint64_t ResyncsDone()      const noexcept { return resyncsDone_; }

private:
    bool DoResync();

    // --- State (all accessed from main thread only -- no atomics needed) ---
    int64_t  anchor10ns_       = 0;
    int64_t  framePeriod10ns_  = 0;
    double   refreshRateHz_    = 0;
    int64_t  frameCounter_     = 0;
    int64_t  lastVblank10ns_   = 0;
    int64_t  nextVblank10ns_   = 0;
    bool     resyncRequested_  = false;

    // --- Counters ---
    uint64_t framesDispatched_ = 0;
    uint64_t framesMissed_     = 0;
    uint64_t resyncsDone_      = 0;

    // --- Constants ---
    static constexpr int64_t SPIN_WINDOW_10NS      = 5000;    // 50us spin window
    static constexpr int64_t RESYNC_THRESHOLD_10NS = 50000;  // 500us drift tolerance
};

// ---------------------------------------------------------------------------
// IMPLEMENTATION
// ---------------------------------------------------------------------------

inline bool FramePredictor::DoResync() {
    // --- Read the monitor's actual refresh period (cached after first call). ---
    static int64_t monitorPeriod10ns = 0;
    static bool monitorPeriodMeasured = false;
    constexpr double RATE_MATCH_TOLERANCE = 0.02;  // 2%

    if (!monitorPeriodMeasured) {
        monitorPeriod10ns = VblankSource::GetMonitorPeriod10ns();
        monitorPeriodMeasured = true;

        if (monitorPeriod10ns > 0) {
            double monitorHz = 100000000.0 / (double)monitorPeriod10ns;
            double ourHz     = 100000000.0 / (double)framePeriod10ns_;
            double ratio     = ourHz / monitorHz;
            printf("[FramePredictor] monitor ~%.2f Hz, predictor %.2f Hz, ratio %.3f -- resync %s\n",
                   monitorHz, ourHz, ratio,
                   (ratio > 1.0 - RATE_MATCH_TOLERANCE && ratio < 1.0 + RATE_MATCH_TOLERANCE)
                       ? "ENABLED" : "DISABLED (rates don't match)");
        } else {
            printf("[FramePredictor] could not read monitor refresh period -- resync ENABLED by default\n");
        }
    }

    // --- If rates don't match, do NOT re-anchor. ---
    if (monitorPeriod10ns > 0) {
        double ratio = (double)framePeriod10ns_ / (double)monitorPeriod10ns;
        if (ratio < 1.0 - RATE_MATCH_TOLERANCE || ratio > 1.0 + RATE_MATCH_TOLERANCE) {
            return false;
        }
    }

    // --- Rates match (or we couldn't measure) -- correct drift. ---
    int64_t currentVblank10ns = VblankSource::WaitForNextVblank10ns();
    if (currentVblank10ns == 0)
        return false;

    int64_t ourLastVblank = anchor10ns_ + frameCounter_ * framePeriod10ns_;
    int64_t drift = currentVblank10ns - ourLastVblank;
    if (std::abs(drift) > RESYNC_THRESHOLD_10NS) {
        anchor10ns_ = currentVblank10ns - frameCounter_ * framePeriod10ns_;
        resyncsDone_++;
        return true;
    }
    return false;
}


// ============================================================================
// VsyncCounter — non-blocking per-frame vsync polling for the main loop.
//
// Unlike VblankSource (which blocks until the next vblank for the
// FramePredictor), VsyncCounter uses event-driven detection:
//   Windows  : DWM qpcVBlank polling
//   Linux    : Wayland callback flag, or DRM event-based (DRM_VBLANK_EVENT)
//   Android  : AChoreographer callback flag
//   Fallback : timer-based
//
// All times in 10ns units (matching getTime10ns2()).
//
// Usage (from the main loop, once per frame):
//   // One-time, Linux only — call before first Poll():
//   VsyncCounter::SetWaylandInfo(surface, display);
//
//   // Each frame:
//   auto r = VsyncCounter::Poll(now10ns, RENDER_PERIOD_10NS, windowX, windowY);
//   if (r.shouldRender) { dispatch UPDATE/RENDER with r.frameTime10ns }
//
// On frame-rate change:
//   VsyncCounter::NotifyFrameRateChange();
// ============================================================================

namespace VsyncCounter {

struct Result {
    bool shouldRender = false;
    int64_t frameTime10ns = 0;
};

// ---------------------------------------------------------------------------
// Internal state (file-scope statics inside an anonymous namespace)
// ---------------------------------------------------------------------------
namespace {

int64_t vc_lastRenderTime_ = 0;
int64_t vc_nextRenderTime10ns_ = 0;
bool   vc_firstFrame_ = true;
int64_t vc_lag_ = 0;           // time of last NotifyFrameRateChange()

// ---- Timer fallback ----
inline Result TimerFallback(int64_t now10ns, int64_t renderPeriod10ns) {
    Result r;
    if (vc_firstFrame_) {
        vc_lastRenderTime_ = now10ns;
        vc_nextRenderTime10ns_ = now10ns + renderPeriod10ns;
        vc_firstFrame_ = false;
        return r;
    }
    int64_t vsyncThreshold = renderPeriod10ns / 4;
    int64_t timeSinceLastRender = now10ns - vc_lastRenderTime_;
    int64_t elapsed = now10ns - vc_lag_;
    int64_t threshold = std::max<int64_t>(elapsed, renderPeriod10ns / 2);

    if (now10ns >= (vc_nextRenderTime10ns_ - threshold)) {
        if (timeSinceLastRender >= vsyncThreshold) {
            r.shouldRender = true;
            r.frameTime10ns = now10ns - vc_lastRenderTime_;
            vc_lastRenderTime_ = now10ns;
            vc_nextRenderTime10ns_ += renderPeriod10ns;
        }
    }
    return r;
}

// =========================================================================
#ifdef _WIN32
// =========================================================================
// Windows: DWM qpcVBlank polling

int64_t  vc_lastQpcVBlank_ = 0;
int64_t  vc_predictedNextVblank10ns_ = 0;

} // anonymous namespace

inline Result Poll(int64_t now10ns, int64_t renderPeriod10ns,
                   int windowX = 0, int windowY = 0) {
    Result r;
    int64_t vsyncThreshold = renderPeriod10ns / 4;
    int64_t timeSinceLastRender = now10ns - vc_lastRenderTime_;

    if (vc_firstFrame_) {
        vc_lastRenderTime_ = now10ns;
        vc_nextRenderTime10ns_ = now10ns + renderPeriod10ns;
        vc_firstFrame_ = false;
    }

    DWM_TIMING_INFO ti = {};
    ti.cbSize = sizeof(ti);
    HRESULT hr = DwmGetCompositionTimingInfo(nullptr, &ti);

    if (SUCCEEDED(hr)) {
        if (vc_lastQpcVBlank_ == 0 || vc_lastQpcVBlank_ != (int64_t)ti.qpcVBlank) {
            if (timeSinceLastRender >= vsyncThreshold) {
                r.shouldRender = true;
                if (vc_lastQpcVBlank_ != 0) {
                    int64_t qpcDelta = (int64_t)(ti.qpcVBlank - vc_lastQpcVBlank_);
                    r.frameTime10ns = (qpcDelta * TICKS_PER_SECOND_10NS) / qpcFrequency2.QuadPart;
                } else {
                    r.frameTime10ns = renderPeriod10ns;
                }
                vc_lastQpcVBlank_ = (int64_t)ti.qpcVBlank;
                vc_nextRenderTime10ns_ = now10ns + renderPeriod10ns;

                int64_t vblank10ns = (int64_t)((ti.qpcVBlank * TICKS_PER_SECOND_10NS) / qpcFrequency2.QuadPart);
                vc_predictedNextVblank10ns_ = vblank10ns + r.frameTime10ns;
            }
        } else if (now10ns >= vc_nextRenderTime10ns_ + renderPeriod10ns) {
            if (timeSinceLastRender >= vsyncThreshold) {
                r.shouldRender = true;
                r.frameTime10ns = now10ns - vc_lastRenderTime_;
                vc_lastRenderTime_ = now10ns;
                vc_nextRenderTime10ns_ = now10ns + renderPeriod10ns;
            }
        }
    } else {
        r = TimerFallback(now10ns, renderPeriod10ns);
    }

    if (r.shouldRender) {
        vc_lastRenderTime_ = now10ns;
    }
    return r;
}

inline int64_t GetPredictedNextVblank10ns() { return vc_predictedNextVblank10ns_; }

// =========================================================================
#elif defined(__linux__) && !defined(__ANDROID__)
// =========================================================================
// Linux: Wayland (non-blocking callback) -> DRM event-based -> timer fallback

// --- Minimal Wayland types (no libwayland-dev at compile time) ---
struct wl_surface_vc;
struct wl_callback_vc;

struct wl_callback_listener_vc {
    void (*done)(void *data, struct wl_callback_vc *callback, uint32_t time);
};

typedef struct wl_callback_vc* (*wl_surface_frame_vc_t)(struct wl_surface_vc*);
typedef int  (*wl_callback_add_listener_vc_t)(struct wl_callback_vc*, const struct wl_callback_listener_vc*, void*);
typedef void (*wl_callback_destroy_vc_t)(struct wl_callback_vc*);

// --- Wayland state (separate from VblankSource's blocking Wayland) ---
static void*  wl_vc_lib = nullptr;
static wl_surface_frame_vc_t        p_vc_wl_frame = nullptr;
static wl_callback_add_listener_vc_t p_vc_wl_add_listener = nullptr;
static wl_callback_destroy_vc_t     p_vc_wl_destroy = nullptr;

static bool   wl_vc_loaded = false;
static bool   wl_vc_available = false;
static struct wl_surface_vc* wl_vc_surface = nullptr;
static struct wl_callback_vc* wl_vc_callback = nullptr;
static bool   wl_vc_fired = false;
static int64_t wl_vc_lastTime10ns = 0;
static struct wl_callback_listener_vc wl_vc_listener = {};

// --- DRM event-based types and state ---

// drmDevice - we only need nodes[DRM_NODE_PRIMARY] (index 0).
// In all libdrm 2.4.x versions, nodes[] is the very first field.
#define VC_DRM_NODE_PRIMARY 0
#define VC_DRM_NODE_MAX     6

struct VcDrmDevice {
    char *nodes[VC_DRM_NODE_MAX];
    // Remaining fields not accessed - padding for ABI safety
    void *_pad[20];
};

// drmModeCrtc - we need mode_valid, width, height, x, y.
// Layout matches xf86drmMode.h (all uint32_t/int, no padding issues).
struct VcDrmModeCrtc {
    uint32_t crtc_id;
    uint32_t buffer_id;
    uint32_t x, y;
    uint32_t width, height;
    int mode_valid;
    void *_mode_pad[32]; // drmModeModeInfo is large
    int gamma_size;
    uint32_t *gamma;
};

// drmVBlank - matches the kernel's union drm_wait_vblank ABI exactly.
// On x86-64: request.type(int,4) request.sequence(uint,4) request.signal(ulong,8)
struct VcDrmVBlank {
    union {
        struct {
            int type;
            unsigned int sequence;
            unsigned long signal;
        } request;
        struct {
            int type;
            unsigned int sequence;
            long tv_sec;
            long tv_usec;
        } reply;
    };
};

// drmEventContext - we only use vblank_handler (version 1 compatible).
struct VcDrmEventContext {
    int version;
    void (*vblank_handler)(int fd, unsigned int sequence,
                           unsigned int tv_sec, unsigned int tv_usec,
                           void *user_data);
    void (*page_flip_handler)(int fd, unsigned int sequence,
                              unsigned int tv_sec, unsigned int tv_usec,
                              void *user_data);
};

#define VC_DRM_VBLANK_RELATIVE       0x1
#define VC_DRM_VBLANK_EVENT          0x2
#define VC_DRM_VBLANK_HIGH_CRTC_MASK 0x0000003C
#define VC_DRM_VBLANK_HIGH_CRTC_SHIFT 2
#define VC_DRM_EVENT_CONTEXT_VERSION 1

// DRM function pointer types (additional to VblankSource's set)
typedef int  (*vc_drmGetDevices_t)(VcDrmDevice**, int);
typedef void (*vc_drmFreeDevices_t)(VcDrmDevice**, int);
typedef VcDrmModeCrtc* (*vc_drmModeGetCrtc_t)(int fd, uint32_t crtc_id);
typedef void (*vc_drmModeFreeCrtc_t)(VcDrmModeCrtc* ptr);
typedef int  (*vc_drmHandleEvent_t)(int fd, VcDrmEventContext*);

// DRM event-based state
static int  drm_vc_fd = -1;
static uint32_t drm_vc_crtc_id = 0;
static uint64_t drm_vc_last_seq = 0;
static int  drm_vc_last_win_x = -1;
static int  drm_vc_last_win_y = -1;
static bool drm_vc_inited = false;

// DRM function pointers
static void*  drm_vc_lib = nullptr;
static vc_drmGetDevices_t      p_vc_drmGetDevices = nullptr;
static vc_drmFreeDevices_t     p_vc_drmFreeDevices = nullptr;
static vc_drmModeGetCrtc_t     p_vc_drmModeGetCrtc = nullptr;
static vc_drmModeFreeCrtc_t    p_vc_drmModeFreeCrtc = nullptr;
static vc_drmHandleEvent_t     p_vc_drmHandleEvent = nullptr;
static bool   drm_vc_loaded = false;

// The vblank sequence counter - written by the event handler callback.
static uint64_t vc_vblank_seq = 0;

// Reuse VblankSource's DrmModeRes and its get/free functions for CRTC listing.
// They are already loaded by the DrmState singleton and ABI-compatible.

} // anonymous namespace

// --- Wayland callback handler ---
static void vc_wayland_frame_cb(void* /*data*/, struct wl_callback_vc* cb, uint32_t time) {
    (void)data;
    wl_vc_fired = true;
    int64_t t10ns = (int64_t)time * 100000LL;
    wl_vc_lastTime10ns = t10ns;

    if (p_vc_wl_destroy) p_vc_wl_destroy(cb);

    // Re-post one-shot callback
    if (p_vc_wl_frame && p_vc_wl_add_listener && wl_vc_surface) {
        wl_vc_callback = p_vc_wl_frame(wl_vc_surface);
        p_vc_wl_add_listener(wl_vc_callback, &wl_vc_listener, wl_vc_surface);
    }
}

// --- Public: set Wayland surface (call once before first Poll) ---
inline void SetWaylandInfo(void* surface, void* /*display*/) {
    (void)display;
    if (wl_vc_loaded) return;
    wl_vc_loaded = true;

    wl_vc_lib = dlopen("libwayland-client.so.0", RTLD_LAZY);
    if (!wl_vc_lib) return;

    p_vc_wl_frame       = (wl_surface_frame_vc_t)dlsym(wl_vc_lib, "wl_surface_frame");
    p_vc_wl_add_listener = (wl_callback_add_listener_vc_t)dlsym(wl_vc_lib, "wl_callback_add_listener");
    p_vc_wl_destroy     = (wl_callback_destroy_vc_t)dlsym(wl_vc_lib, "wl_callback_destroy");

    if (!p_vc_wl_frame || !p_vc_wl_add_listener || !p_vc_wl_destroy) {
        dlclose(wl_vc_lib);
        wl_vc_lib = nullptr;
        return;
    }

    wl_vc_listener.done = vc_wayland_frame_cb;
    wl_vc_surface = (struct wl_surface_vc*)surface;
    wl_vc_available = (wl_vc_surface != nullptr);

    if (wl_vc_available) {
        wl_vc_callback = p_vc_wl_frame(wl_vc_surface);
        p_vc_wl_add_listener(wl_vc_callback, &wl_vc_listener, wl_vc_surface);
    }
}

// --- Load DRM library for event-based vblank ---
inline bool LoadDrmVC() {
    if (drm_vc_loaded) return (drm_vc_lib != nullptr);
    drm_vc_loaded = true;

    drm_vc_lib = dlopen("libdrm.so.2", RTLD_LAZY);
    if (!drm_vc_lib) {
        drm_vc_lib = dlopen("libdrm.so", RTLD_LAZY);
    }
    if (!drm_vc_lib) return false;

    p_vc_drmGetDevices  = (vc_drmGetDevices_t)dlsym(drm_vc_lib, "drmGetDevices");
    p_vc_drmFreeDevices = (vc_drmFreeDevices_t)dlsym(drm_vc_lib, "drmFreeDevices");
    p_vc_drmModeGetCrtc = (vc_drmModeGetCrtc_t)dlsym(drm_vc_lib, "drmModeGetCrtc");
    p_vc_drmModeFreeCrtc = (vc_drmModeFreeCrtc_t)dlsym(drm_vc_lib, "drmModeFreeCrtc");
    p_vc_drmHandleEvent  = (vc_drmHandleEvent_t)dlsym(drm_vc_lib, "drmHandleEvent");

    if (!p_vc_drmGetDevices || !p_vc_drmFreeDevices ||
        !p_vc_drmModeGetCrtc || !p_vc_drmModeFreeCrtc || !p_vc_drmHandleEvent) {
        dlclose(drm_vc_lib);
        drm_vc_lib = nullptr;
        return false;
    }
    return true;
}

// --- DRM: find the CRTC whose viewport contains (windowX, windowY) ---
inline void DrmVcFindCrtc(int windowX, int windowY) {
    if (!LoadDrmVC()) return;
    // Also need VblankSource's drm resource functions loaded
    DrmState& ds = GetDrmState();
    if (!ds.p_getResources || !ds.p_freeResources || !ds.p_waitVBlank) return;

    if (drm_vc_fd >= 0) { close(drm_vc_fd); drm_vc_fd = -1; }

    VcDrmDevice* devices[16];
    int deviceCount = p_vc_drmGetDevices(devices, 16);
    if (deviceCount <= 0) return;

    for (int i = 0; i < deviceCount; i++) {
        VcDrmDevice* dev = devices[i];
        if (!dev->nodes[VC_DRM_NODE_PRIMARY]) continue;

        int fd = open(dev->nodes[VC_DRM_NODE_PRIMARY], O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;

        DrmModeRes* res = ds.p_getResources(fd);
        if (!res) { close(fd); continue; }

        bool found = false;
        for (int c = 0; c < res->count_crtcs && !found; c++) {
            uint32_t crtcId = res->crtcs[c];
            VcDrmModeCrtc* crtc = p_vc_drmModeGetCrtc(fd, crtcId);
            if (!crtc) continue;

            if (crtc->mode_valid && crtc->width > 0 && crtc->height > 0) {
                if (windowX >= (int)crtc->x && windowX < (int)(crtc->x + crtc->width) &&
                    windowY >= (int)crtc->y && windowY < (int)(crtc->y + crtc->height)) {

                    drm_vc_fd = fd;
                    drm_vc_crtc_id = crtcId;
                    drm_vc_inited = true;
                    found = true;

                    // Prime: request first vblank event (non-blocking, DRM_VBLANK_EVENT)
                    VcDrmVBlank primeVbl;
                    memset(&primeVbl, 0, sizeof(primeVbl));
                    primeVbl.request.type = VC_DRM_VBLANK_RELATIVE | VC_DRM_VBLANK_EVENT;
                    primeVbl.request.type |= (crtcId << VC_DRM_VBLANK_HIGH_CRTC_SHIFT);
                    primeVbl.request.sequence = 1;
                    primeVbl.request.signal = (unsigned long)&vc_vblank_seq;
                    ds.p_waitVBlank(fd, (DrmVBlank*)&primeVbl);
                }
            }
            p_vc_drmModeFreeCrtc(crtc);
        }
        ds.p_freeResources(res);
        if (!found) close(fd);
        if (found) break;
    }
    p_vc_drmFreeDevices(devices, deviceCount);
}

// --- DRM: non-blocking event poll ---
inline bool DrmVcPoll(int64_t now10ns, int64_t renderPeriod10ns, Result& r) {
    if (drm_vc_fd < 0 || drm_vc_crtc_id == 0) return false;

    DrmState& ds = GetDrmState();
    if (!ds.p_waitVBlank) return false;

    struct pollfd pfd;
    pfd.fd = drm_vc_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int pr = poll(&pfd, 1, 0);
    if (pr <= 0 || !(pfd.revents & POLLIN)) return false;

    VcDrmEventContext evctx;
    memset(&evctx, 0, sizeof(evctx));
    evctx.version = VC_DRM_EVENT_CONTEXT_VERSION;
    evctx.vblank_handler = [](int fd, unsigned int sequence,
                               unsigned int tv_sec, unsigned int tv_usec,
                               void *user_data) {
        (void)fd; (void)tv_sec; (void)tv_usec;
        uint64_t *seqPtr = (uint64_t *)user_data;
        *seqPtr = sequence;
    };

    p_vc_drmHandleEvent(drm_vc_fd, &evctx);

    if (vc_vblank_seq != drm_vc_last_seq) {
        int64_t vsyncThreshold = renderPeriod10ns / 4;
        int64_t timeSinceLastRender = now10ns - vc_lastRenderTime_;
        if (timeSinceLastRender >= vsyncThreshold) {
            r.shouldRender = true;
            r.frameTime10ns = now10ns - vc_lastRenderTime_;
            vc_lastRenderTime_ = now10ns;
        }
        drm_vc_last_seq = vc_vblank_seq;

        // Request next vblank event
        VcDrmVBlank nextVbl;
        memset(&nextVbl, 0, sizeof(nextVbl));
        nextVbl.request.type = VC_DRM_VBLANK_RELATIVE | VC_DRM_VBLANK_EVENT;
        nextVbl.request.type |= (drm_vc_crtc_id << VC_DRM_VBLANK_HIGH_CRTC_SHIFT);
        nextVbl.request.sequence = 1;
        nextVbl.request.signal = (unsigned long)&vc_vblank_seq;
        ds.p_waitVBlank(drm_vc_fd, (DrmVBlank*)&nextVbl);
        return true;
    }
    return false;
}

// --- Main Poll ---
inline Result Poll(int64_t now10ns, int64_t renderPeriod10ns,
                   int windowX = 0, int windowY = 0) {
    Result r;

    if (vc_firstFrame_) {
        vc_lastRenderTime_ = now10ns;
        vc_nextRenderTime10ns_ = now10ns + renderPeriod10ns;
        vc_firstFrame_ = false;
    }

    // 1. Wayland (non-blocking callback flag)
    if (wl_vc_available) {
        if (wl_vc_fired) {
            int64_t vsyncThreshold = renderPeriod10ns / 4;
            int64_t timeSinceLastRender = now10ns - vc_lastRenderTime_;
            if (timeSinceLastRender >= vsyncThreshold) {
                r.shouldRender = true;
                r.frameTime10ns = now10ns - vc_lastRenderTime_;
                vc_lastRenderTime_ = now10ns;
                vc_nextRenderTime10ns_ = now10ns + renderPeriod10ns;
            }
            wl_vc_fired = false;
            if (r.shouldRender) return r;
        }
    }
    // 2. DRM event-based fallback (X11, etc.)
    else {
        // Re-init if window moved to a different monitor
        if (!drm_vc_inited || windowX != drm_vc_last_win_x || windowY != drm_vc_last_win_y) {
            drm_vc_last_win_x = windowX;
            drm_vc_last_win_y = windowY;
            DrmVcFindCrtc(windowX, windowY);
        }

        if (DrmVcPoll(now10ns, renderPeriod10ns, r)) {
            return r;
        }
    }

    // 3. Timer fallback
    r = TimerFallback(now10ns, renderPeriod10ns);
    return r;
}

inline int64_t GetPredictedNextVblank10ns() { return 0; }

// =========================================================================
#elif defined(__ANDROID__)
// =========================================================================
// Android: AChoreographer callback flag

namespace {
static AChoreographer* vc_choreo_ = nullptr;
static bool vc_choreo_fired_ = false;
static int64_t vc_choreo_frame_time_ = 0;

static void vc_choreo_cb(long frameTimeNanos, void* /*data*/) {
    (void)data;
    vc_choreo_fired_ = true;
    vc_choreo_frame_time_ = frameTimeNanos / 10;
}
} // anonymous namespace

// Ensure the choreographer is posting callbacks
inline void InitChoreographer() {
    if (!vc_choreo_) {
        vc_choreo_ = AChoreographer_getInstance();
        if (vc_choreo_) {
            AChoreographer_postFrameCallback(vc_choreo_, vc_choreo_cb, nullptr);
        }
    }
}

inline Result Poll(int64_t now10ns, int64_t renderPeriod10ns,
                   int /*windowX*/ = 0, int /*windowY*/ = 0) {
    (void)windowX; (void)windowY;
    Result r;
    InitChoreographer();

    if (vc_choreo_ && vc_choreo_fired_) {
        int64_t vsyncThreshold = renderPeriod10ns / 4;
        int64_t timeSinceLastRender = now10ns - vc_lastRenderTime_;
        if (timeSinceLastRender >= vsyncThreshold) {
            r.shouldRender = true;
            r.frameTime10ns = vc_choreo_frame_time_ - vc_lastRenderTime_;
            vc_lastRenderTime_ = vc_choreo_frame_time_;
            vc_choreo_fired_ = false;
            AChoreographer_postFrameCallback(vc_choreo_, vc_choreo_cb, nullptr);
            return r;
        }
        vc_choreo_fired_ = false;
        AChoreographer_postFrameCallback(vc_choreo_, vc_choreo_cb, nullptr);
    }

    if (vc_firstFrame_) {
        vc_lastRenderTime_ = now10ns;
        vc_nextRenderTime10ns_ = now10ns + renderPeriod10ns;
        vc_firstFrame_ = false;
        return r;
    }
    r = TimerFallback(now10ns, renderPeriod10ns);
    return r;
}

inline int64_t GetPredictedNextVblank10ns() { return 0; }

// =========================================================================
#else
// =========================================================================
// Fallback: timer-only (macOS, emscripten, etc.)

inline Result Poll(int64_t now10ns, int64_t renderPeriod10ns,
                   int /*windowX*/ = 0, int /*windowY*/ = 0) {
    (void)windowX; (void)windowY;
    return TimerFallback(now10ns, renderPeriod10ns);
}

inline int64_t GetPredictedNextVblank10ns() { return 0; }

#endif

// --- Cross-platform helpers ---

inline void NotifyFrameRateChange() {
    vc_lag_ = getTime10ns2();
}

} // namespace VsyncCounter