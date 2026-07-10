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
//    The predictor thread re-queries the platform's vblank source (DWM on
//    Windows, DRM on Linux, AChoreographer on Android) and corrects the
//    anchor if crystal drift has accumulated beyond ~500µs. Only fires when
//    the predictor's rate matches the monitor's rate.
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

    if (!s.p_frame || !s.p_add_listener || !s.p_destroy || !s.p_dispatch || !s.p_flush) {
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

// Blocks until the next Wayland frame callback fires.
// MUST be called from the main thread (Wayland display is not thread-safe).
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

    // Dispatch events until the callback fires.
    // wl_display_dispatch blocks until at least one event is processed.
    while (!s.fired) {
        if (s.p_dispatch(s.display) < 0) break;
    }

    std::lock_guard<std::mutex> lk(s.mtx);
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
    // is phase-locked to the monitor's vblank — critical when running at
    // a multiple of the monitor rate (e.g., 120Hz update on 60Hz monitor:
    // every 2nd frame lands exactly on vblank).
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

        // --- Anchor to a real vblank ---
        // Blocks until the next vblank on all platforms:
        //   Windows : polls DWM qpcVBlank until it advances (~17ms max @ 60Hz)
        //   Linux   : drmWaitVBlank (blocks in kernel)
        //   Android : AChoreographer + condition variable
        // If the vblank source is unavailable, falls back to getTime10ns2().
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
    // running, RE-ANCHORS to the next vblank, recomputes the period,
    // restarts.
    //
    // Safe to call from the main loop at any time. The next isTime() will
    // fire one frame period after the next vblank.
    // ---------------------------------------------------------------------
    void SetFrameRate(double frameRate) {
        if (frameRate < 1.0 || frameRate > 1000.0) return;

        bool wasRunning = running_.load(std::memory_order_acquire);
        if (wasRunning) Stop();

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
    // --- Read the monitor's actual refresh period (cached after first call). ---
    // If our predictor's framePeriod doesn't match the monitor's refresh rate
    // (within RATE_MATCH_TOLERANCE), re-anchoring to vblank would yank us
    // onto the WRONG grid. So we skip resync entirely when rates don't match.
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
            printf("[FramePredictor] monitor ~%.2f Hz, predictor %.2f Hz, ratio %.3f — resync %s\n",
                   monitorHz, ourHz, ratio,
                   (ratio > 1.0 - RATE_MATCH_TOLERANCE && ratio < 1.0 + RATE_MATCH_TOLERANCE)
                       ? "ENABLED" : "DISABLED (rates don't match)");
        } else {
            printf("[FramePredictor] could not read monitor refresh period — resync ENABLED by default\n");
        }
    }

    // --- If rates don't match, do NOT re-anchor. ---
    if (monitorPeriod10ns > 0) {
        double ratio = (double)framePeriod10ns_ / (double)monitorPeriod10ns;
        if (ratio < 1.0 - RATE_MATCH_TOLERANCE || ratio > 1.0 + RATE_MATCH_TOLERANCE) {
            return false;  // different grids — leave the predictor alone
        }
    }

    // --- Rates match (or we couldn't measure) — correct drift. ---
    int64_t currentVblank10ns = VblankSource::GetCurrentVblank10ns();
    if (currentVblank10ns == 0)
        return false;  // vblank source unavailable

    int64_t ourLastVblank = anchor10ns_ + frameCounter_ * framePeriod10ns_;
    int64_t drift = currentVblank10ns - ourLastVblank;
    if (std::abs(drift) > RESYNC_THRESHOLD_10NS) {
        anchor10ns_ = currentVblank10ns - frameCounter_ * framePeriod10ns_;
        resyncsDone_.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }
    return false;
}
