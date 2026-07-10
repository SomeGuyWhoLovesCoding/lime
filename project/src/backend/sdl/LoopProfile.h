// ============================================================================
// LoopProfile.h — drop-in instrumentation for SDLApplication::Update()
//
// Captures per-call timings + phase error and dumps p50/p90/p99/p99.9
// and a histogram to C:/p99Loop.txt every CAP samples (and on Quit()).
//
// Signals captured per Update() call (all in microseconds):
//   total        end-to-end Update() duration
//   sleep        time spent inside coolSleepUntil10ns()
//   work         total - sleep  (SubLoopTick + PollInputs + UPDATE/RENDER dispatch)
//   wakeErr      actual wake - target sleep time    (+ = late, the stutter source)
//   updatePhase  now10ns - ideal 120Hz grid time    (+ = UPDATE fired late vs grid)
//   interArrival time since previous Update() return (should be ~8333 us @ 120 Hz)
//   renderFired  whether RENDER dispatched this call
//   renderDur    duration of the RENDER dispatch
//
// Interpretation guide for the dump (phase desync diagnosis):
//   * updatePhase p99 < 500us AND interArrival p99 < 9000us
//       -> Loop is landing on grid cleanly. Stutter is NOT from Update().
//          It's because the render grid (driven by user's SwapWindow w/ SDL vsync)
//          is decoupled from the 120Hz update grid. Switch to Update_Vsync()
//          (DWM-driven) path, or run update at the monitor's native rate.
//   * updatePhase p99 > 1500us
//       -> coolSleepUntil10ns is failing to land on grid. Check wakeErr — if it
//          tracks updatePhase, the high-res waitable timer is jittering.
//   * interArrival p99 far from 8333us (e.g. >12000us or <6000us)
//       -> Loop is missing frames. Look for work p99 spikes (user callback stalls).
// ============================================================================

#pragma once
#include <cstdint>
#include <atomic>
#include <array>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <climits>
#include <chrono>
#include <cerrno>
#include <cstring>    // strerror_s
#include <string>

#ifdef _WIN32
#  include <windows.h>   // GetEnvironmentVariableA, MAX_PATH, DWORD
#  pragma comment(lib, "user32.lib")
#endif

namespace LoopProfile {

constexpr size_t CAP = 1 << 12;   // 4096 samples ≈ 30sec @ 120 Hz

// Wall-clock interval between automatic dumps (so the file appears quickly
// even if the user only runs for a few seconds). Set to 0 to disable.
constexpr int64_t DUMP_INTERVAL_SEC = 10;

struct Sample {
    int64_t totalUs;
    int64_t sleepUs;
    int64_t workUs;
    int64_t wakeErrUs;
    int64_t updatePhaseUs;
    int64_t interArrivalUs;
    bool     renderFired;
    int64_t renderDurUs;
    // NEW:
    int64_t subLoopDurUs;   // SubLoopTickEvent::Dispatch duration
    int64_t pollDurUs;      // PollInputs() duration
    int64_t updateDurUs;    // ApplicationEvent::Dispatch (UPDATE) duration
    int64_t execGapUs;      // time in Exec() between Update() calls
};

static std::array<Sample, CAP> g_buf;
static std::atomic<size_t>     g_idx{0};
static std::atomic<bool>       g_overflow{false};
static std::atomic<int64_t>    g_lastReturn10{0};

inline void record(const Sample& s) {
    size_t i = g_idx.fetch_add(1, std::memory_order_relaxed);
    if (i < CAP) {
        g_buf[i] = s;
    } else {
        g_overflow.store(true, std::memory_order_relaxed);
    }
}

inline int64_t lastReturn10() {
    return g_lastReturn10.load(std::memory_order_relaxed);
}
inline void   setLastReturn10(int64_t v) {
    g_lastReturn10.store(v, std::memory_order_relaxed);
}

// Clears the sample buffer (called after a dump so each window is fresh).
inline void reset() {
    g_idx.store(0, std::memory_order_relaxed);
    g_overflow.store(false, std::memory_order_relaxed);
    g_lastReturn10.store(0, std::memory_order_relaxed);
}

// Returns true if DUMP_INTERVAL_SEC has elapsed since the last dump.
// Uses steady_clock so wall-clock adjustments don't trigger false dumps.
inline bool shouldPeriodicDump() {
    if (DUMP_INTERVAL_SEC <= 0) return false;
    static auto lastDump = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastDump).count();
    if (elapsed >= DUMP_INTERVAL_SEC) {
        lastDump = now;
        return true;
    }
    return false;
}

// --- percentile helper (sorts a copy) ---
inline int64_t percentile(std::vector<int64_t> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t i = (size_t)((v.size() - 1) * p);
    return v[i];
}

inline void dumpToFile(const char* path) {
    size_t total = g_idx.load();
    size_t n = std::min<size_t>(total, CAP);
    if (n == 0) return;

    std::vector<int64_t> tot, slp, wrk, werr, uph, ia, rndur;
    std::vector<int64_t> sub, poll, upd, egap;
    tot.reserve(n); slp.reserve(n); wrk.reserve(n);
    werr.reserve(n); uph.reserve(n); ia.reserve(n);

    size_t renderCount = 0;
    for (size_t i = 0; i < n; ++i) {
        const auto& s = g_buf[i];
        tot.push_back(s.totalUs);
        slp.push_back(s.sleepUs);
        wrk.push_back(s.workUs);
        werr.push_back(s.wakeErrUs);
        uph.push_back(s.updatePhaseUs);
        ia.push_back(s.interArrivalUs);
        sub.push_back(s.subLoopDurUs);
        poll.push_back(s.pollDurUs);
        upd.push_back(s.updateDurUs);
        egap.push_back(s.execGapUs);
        if (s.renderFired) {
            renderCount++;
            rndur.push_back(s.renderDurUs);
        }
    }

    // Try multiple write locations in order — C:\ root is often UAC-locked
    // on modern Windows, so we fall back to %TEMP%, then the working dir.
    // On every failure we print to stderr so you can see WHY nothing appears.
    std::string finalPath;
    FILE* f = nullptr;
    std::string err1, err2, err3;

#ifdef _WIN32
    // 1) The path the caller asked for (typically C:/p99Loop.txt)
    err1 = std::string(path);
    f = fopen(path, "w");
    if (!f) {
        int e = errno; char buf[256]; strerror_s(buf, sizeof(buf), e);
        err1 = std::string(path) + " -> " + buf;

        // 2) %TEMP% (always writable for the current user)
        char tmpEnv[MAX_PATH];
        DWORD tmpLen = GetEnvironmentVariableA("TEMP", tmpEnv, MAX_PATH);
        if (tmpLen == 0 || tmpLen >= MAX_PATH) {
            tmpLen = GetEnvironmentVariableA("USERPROFILE", tmpEnv, MAX_PATH);
        }
        if (tmpLen > 0 && tmpLen < MAX_PATH) {
            std::string tmpPath = std::string(tmpEnv) + "\\p99Loop.txt";
            err2 = tmpPath;
            f = fopen(tmpPath.c_str(), "w");
            if (!f) {
                int e2 = errno; char buf2[256]; strerror_s(buf2, sizeof(buf2), e2);
                err2 = tmpPath + " -> " + buf2;
            } else {
                finalPath = tmpPath;
            }
        }
    } else {
        finalPath = std::string(path);
    }

    // 3) Last resort: current working directory
    if (!f) {
        std::string cwdPath = "p99Loop.txt";
        err3 = cwdPath;
        f = fopen(cwdPath.c_str(), "w");
        if (!f) {
            int e3 = errno; char buf3[256]; strerror_s(buf3, sizeof(buf3), e3);
            err3 = cwdPath + " -> " + buf3;
        } else {
            finalPath = cwdPath;
        }
    }

    if (!f) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            fprintf(stderr, "[LoopProfile] ALL dump paths failed:\n");
            fprintf(stderr, "  1) %s\n", err1.c_str());
            fprintf(stderr, "  2) %s\n", err2.c_str());
            fprintf(stderr, "  3) %s\n", err3.c_str());
            fprintf(stderr, "  (Most common cause: writing to C:\\ root requires admin/UAC elevation.)\n");
            fflush(stderr);
        }
        return;
    }

    static bool announcedPath = false;
    if (!announcedPath) {
        announcedPath = true;
        fprintf(stderr, "[LoopProfile] writing dump to: %s\n", finalPath.c_str());
        fflush(stderr);
    }
#else
    f = fopen("/tmp/p99Loop.txt", "w");
    if (!f) return;
    finalPath = "/tmp/p99Loop.txt";
#endif

    fprintf(f, "=== Loop Profile (Update() non-vsync path) ===\n");
    fprintf(f, "Samples: %zu / %zu  (overflow: %s)\n\n",
            n, total, g_overflow.load() ? "yes" : "no");

    auto row = [&](const char* name, std::vector<int64_t> v) {
        if (v.empty()) { fprintf(f, "  %-26s (no samples)\n", name); return; }
        int64_t p50  = percentile(v, 0.50);
        int64_t p90  = percentile(v, 0.90);
        int64_t p99  = percentile(v, 0.99);
        int64_t p999 = percentile(v, 0.999);
        int64_t mx   = *std::max_element(v.begin(), v.end());
        fprintf(f, "  %-26s p50=%8lld  p90=%8lld  p99=%8lld  p99.9=%8lld  max=%8lld  us\n",
                name, (long long)p50, (long long)p90, (long long)p99,
                (long long)p999, (long long)mx);
    };

    fprintf(f, "Per-call timings (microseconds):\n");
    row("total (end-to-end)",        tot);
    row("  sleep (coolSleepUntil)",   slp);
    row("  work (dispatch+poll)",     wrk);
    row("wakeErr (late=+)",           werr);
    row("updatePhaseErr (late=+)",    uph);
    row("interArrival",               ia);
    fprintf(f, "  (interArrival should be ~8333 us at 120 Hz; deviation = stutter)\n\n");

    fprintf(f, "Render fired on %zu / %zu calls (%.1f%%)\n",
            renderCount, n, 100.0 * renderCount / n);
    if (!rndur.empty()) {
        fprintf(f, "  render dispatch:\n");
        row("    renderDur", rndur);
    }
    fprintf(f, "\n");

    // ---- Histogram of total ----
    fprintf(f, "Histogram - total Update() time (us):\n");
    const int64_t buckets[] = {0, 100, 250, 500, 1000, 2000, 5000, 10000, 20000, 50000, INT64_MAX};
    const size_t nb = sizeof(buckets) / sizeof(buckets[0]) - 1;
    size_t counts[16] = {0};
    for (auto v : tot) {
        for (size_t i = 0; i < nb; ++i) {
            if (v >= buckets[i] && v < buckets[i + 1]) { counts[i]++; break; }
        }
    }
    size_t maxc = *std::max_element(counts, counts + nb);
    for (size_t i = 0; i < nb; ++i) {
        if (counts[i] == 0) continue;
        int barLen = (int)((double)counts[i] / std::max<size_t>(maxc, 1) * 40.0);
        if (buckets[i + 1] == INT64_MAX)
            fprintf(f, "  [%7lld+        ) ", (long long)buckets[i]);
        else
            fprintf(f, "  [%7lld-%7lld) ", (long long)buckets[i], (long long)buckets[i + 1]);
        for (int b = 0; b < barLen; ++b) fputc('#', f);
        fprintf(f, " %6zu (%5.1f%%)\n", counts[i], 100.0 * counts[i] / n);
    }
    fprintf(f, "\n");

    // ---- Histogram of interArrival (key stutter signal) ----
    fprintf(f, "Histogram - interArrival (us) [target = 8333]:\n");
    const int64_t iaBuckets[] = {
        0, 7000, 8000, 8200, 8300, 8333, 8400, 8500, 8700, 9000,
        10000, 12000, 15000, 20000, INT64_MAX
    };
    const size_t inb = sizeof(iaBuckets) / sizeof(iaBuckets[0]) - 1;
    size_t iaCounts[16] = {0};
    for (auto v : ia) {
        for (size_t i = 0; i < inb; ++i) {
            if (v >= iaBuckets[i] && v < iaBuckets[i + 1]) { iaCounts[i]++; break; }
        }
    }
    size_t iamax = *std::max_element(iaCounts, iaCounts + inb);
    for (size_t i = 0; i < inb; ++i) {
        if (iaCounts[i] == 0) continue;
        int barLen = (int)((double)iaCounts[i] / std::max<size_t>(iamax, 1) * 40.0);
        if (iaBuckets[i + 1] == INT64_MAX)
            fprintf(f, "  [%7lld+        ) ", (long long)iaBuckets[i]);
        else
            fprintf(f, "  [%7lld-%7lld) ", (long long)iaBuckets[i], (long long)iaBuckets[i + 1]);
        for (int b = 0; b < barLen; ++b) fputc('#', f);
        fprintf(f, " %6zu (%5.1f%%)\n", iaCounts[i], 100.0 * iaCounts[i] / n);
    }
    fprintf(f, "\n");

    // ---- Worst 20 wake errors (the phase-drift tail) ----
    fprintf(f, "Worst 20 wakeErr samples (us) — sleep overshoot tail:\n");
    std::sort(werr.begin(), werr.end(), std::greater<int64_t>());
    for (size_t i = 0; i < std::min<size_t>(20, werr.size()); ++i)
        fprintf(f, "  %8lld\n", (long long)werr[i]);

    // ---- Diagnostic verdict ----
    fprintf(f, "\n=== Diagnostic verdict ===\n");
    int64_t upP99  = percentile(uph, 0.99);
    int64_t iaP99  = percentile(ia, 0.99);
    int64_t wP99   = percentile(werr, 0.99);
    int64_t workP99 = percentile(wrk, 0.99);

    fprintf(f, "  updatePhaseErr p99 : %lld us\n", (long long)upP99);
    fprintf(f, "  interArrival p99   : %lld us (target 8333)\n", (long long)iaP99);
    fprintf(f, "  wakeErr p99        : %lld us\n", (long long)wP99);
    fprintf(f, "  work p99           : %lld us\n", (long long)workP99);
    fprintf(f, "\n");

    if (upP99 < 500 && std::abs((long long)iaP99 - 8333LL) < 700) {
        fprintf(f, "  VERDICT: Loop is landing on its 120Hz grid cleanly.\n");
        fprintf(f, "           Stutter is NOT from Update() — it is decoupled\n");
        fprintf(f, "           render/swap timing vs the monitor's physical vsync.\n");
        fprintf(f, "           => Switch to Update_Vsync() (DWM-driven path), OR\n");
        fprintf(f, "              set UPDATE_PERIOD to match monitor refresh, OR\n");
        fprintf(f, "              run with vsync OFF and frame-limit at the monitor rate.\n");
    } else if (upP99 > 1500) {
        fprintf(f, "  VERDICT: Update is missing its 120Hz grid (phaseErr p99 > 1.5ms).\n");
        if (wP99 > 1000) {
            fprintf(f, "           Cause: coolSleepUntil10ns is overshooting (wakeErr p99 = %lld us).\n", (long long)wP99);
            fprintf(f, "           Likely high-res waitable timer jitter or DPC latency.\n");
        } else if (workP99 > 2000) {
            fprintf(f, "           Cause: work p99 = %lld us — a user dispatch is stalling.\n", (long long)workP99);
            fprintf(f, "           Profile ApplicationEvent::Dispatch / RenderEvent::Dispatch.\n");
        } else {
            fprintf(f, "           Cause unclear — neither sleep nor work is the obvious culprit.\n");
        }
    } else {
        fprintf(f, "  VERDICT: Borderline. updatePhaseErr p99 = %lld us.\n", (long long)upP99);
        fprintf(f, "           Mild grid drift. Check interArrival histogram for bimodal shape.\n");
    }

    fclose(f);
}

} // namespace LoopProfile
