// SPDX-License-Identifier: GPL-3.0-or-later
// See include/nxrt/pacing.h.
#include "nxrt/pacing.h"

#include <windows.h>
#include <emmintrin.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/hires_wait.h"

namespace nxrt::pacing {

namespace {
constexpr double kMinMarginNs = 300000.0;   // 0.3 ms: the spin always covers at least this much
constexpr double kMaxMarginNs = 3000000.0;  // 3 ms: the spin never covers more than this

struct Listener {
    CapListener fn;
    void* user;
};
std::mutex g_mu;
std::vector<Listener> g_listeners;
std::atomic<double> g_cap{0.0};
std::atomic<int> g_boundary_paced{0};
}  // namespace

uint64_t now_ns() {
    static const uint64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<uint64_t>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const uint64_t q = static_cast<uint64_t>(c.QuadPart);
    return (q / freq) * 1000000000ull + ((q % freq) * 1000000000ull) / freq;
}

const Config& config() {
    static const Config c = [] {
        Config v;
        v.in_main = config_get_str("latency", "pace", "main") != "present";
        v.vsync_cap = config_get_bool("latency", "vsync_cap", true);
        v.reflex_limiter = config_get_bool("latency", "reflex_limiter", false);
        return v;
    }();
    return c;
}

// With vsync on and no cap below the refresh rate the limiter still keeps just under it: the game otherwise renders as fast as the swapchain takes
// frames, the queue behind the display fills (three images) and every picture is shown one to two frames after it was simulated; just under the
// refresh rate the queue stays empty.
double frame_cap(double fps_cap, bool vsync, float refresh_hz) {
    if (!vsync) return fps_cap;
    if (fps_cap <= 0 || (refresh_hz > 0 && fps_cap >= refresh_hz)) return (config().vsync_cap && refresh_hz > 0) ? static_cast<double>(refresh_hz) * 0.9995 : 0;
    return fps_cap;
}

void set_cap(double fps) {
    fps = fps > 0 ? fps : 0;
    std::lock_guard<std::mutex> lk(g_mu);
    g_cap.store(fps);
    for (const Listener& l : g_listeners) l.fn(l.user, fps);
}

double cap() { return g_cap.load(); }

void add_cap_listener(CapListener fn, void* user) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_listeners.push_back({fn, user});
}

void remove_cap_listener(CapListener fn, void* user) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_listeners.erase(std::remove_if(g_listeners.begin(), g_listeners.end(), [&](const Listener& l) { return l.fn == fn && l.user == user; }), g_listeners.end());
}

void boundary_done() { g_boundary_paced.fetch_add(1); }

bool boundary_consume() {
    int n = g_boundary_paced.load();
    while (n > 0) {
        if (g_boundary_paced.compare_exchange_weak(n, n - 1)) return true;
    }
    return false;
}

void Waiter::until_ns(uint64_t target) {
    for (;;) {
        const uint64_t t = now_ns();
        if (t >= target) return;
        const uint64_t remaining = target - t;
        const uint64_t margin = static_cast<uint64_t>(std::min(std::max(kMinMarginNs, 2.0 * overshoot_ema_ns_), kMaxMarginNs));
        if (remaining > margin) {
            const uint64_t coarse = remaining - margin;
            const uint64_t set_at = now_ns();
            if (hires_wait_ns(static_cast<int64_t>(coarse))) {
                const uint64_t woke = now_ns();
                const uint64_t requested = set_at + coarse;
                const double over = woke > requested ? static_cast<double>(woke - requested) : 0.0;
                overshoot_ema_ns_ = overshoot_ema_ns_ * 0.875 + over * 0.125;
            } else {
                Sleep(1);   // no high-resolution timer: no timeBeginPeriod
            }
            continue;
        }
        while (now_ns() < target) _mm_pause();
        return;
    }
}

}  // namespace nxrt::pacing
