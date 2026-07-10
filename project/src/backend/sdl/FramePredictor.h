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
    static LARGE_INTEGER freq = []{
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f;
    }();
    if (ti.qpcRefreshPeriod == 0) return 0;
    return (int64_t)((ti.qpcRefreshPeriod * 100000000LL) / freq.QuadPart);
}

inline int64_t GetCurrentVblank10ns() {
    DWM_TIMING_INFO ti = {};
    ti.cbSize = sizeof(ti);
    if (FAILED(DwmGetCompositionTimingInfo(nullptr, &ti)))
        return 0;
    static LARGE_INTEGER freq = []{
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f;
    }();
    return (int64_t)((ti.qpcVBlank * 100000000LL) / freq.QuadPart);
}

// Blocks until the next vblank. Polls DWM's qpcVBlank until it advances.
// At 60Hz this is at most ~17 Sleep(1) calls. Returns the vblank timestamp
// in 10ns units, or 0 on failure.
inline int64_t WaitForNextVblank10ns() {
    DWM_TIMING_INFO ti = {};
    ti.cbSize = sizeof(ti);
    if (FAILED(DwmGetCompositionTimingInfo(nullptr, &ti)))
        return 0;
    static LARGE_INTEGER freq = []{
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f;
    }();
    QPC_TIME v1 = ti.qpcVBlank;
    QPC_TIME v2 = v1;
    // Timeout: 200ms (should never take more than ~17ms at 60Hz)
    int64_t deadline = getTime10ns2() + 20000000;  // 200ms in 10ns units
    while (v2 == v1) {
        if (getTime10ns2() > deadline) break;
        if (FAILED(DwmGetCompositionTimingInfo(nullptr, &ti))) break;
        v2 = ti.qpcVBlank;
        Sleep(1);  // yield to the OS, ~1ms granularity
    }
    if (v2 == v1) return 0;  // timed out
    return (int64_t)((v2 * 100000000LL) / freq.QuadPart);
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
            frameCounter_ += framesBehind;
            framesMissed_ += (uint64_t)framesBehind;
        }

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
    int64_t currentVblank10ns = VblankSource::GetCurrentVblank10ns();
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
