// SPDX-License-Identifier: GPL-3.0-or-later
// System tick and high-resolution sleep. Pattern from smo-recomp runtime/os/os_threads.c sleep_ns and SuperMarioWonderRecomp kernel.cpp system_tick.
#include <atomic>

#include "nxrt/kernel.h"
#include "nxrt/os/time.h"

namespace nxrt::kernel {

namespace {
std::atomic<const ClockProvider*> g_clock{nullptr};
std::atomic<RngFillFn> g_rng{nullptr};
}

void set_clock_provider(const ClockProvider* p) { g_clock.store(p, std::memory_order_release); }
void set_rng_provider(RngFillFn fn) { g_rng.store(fn, std::memory_order_release); }
bool rng_provided_fill(void* p, size_t n) {
    const RngFillFn fn = g_rng.load(std::memory_order_acquire);
    return fn && fn(p, n);
}

uint64_t wall_filetime() {
    if (const ClockProvider* cp = g_clock.load(std::memory_order_acquire); cp && cp->wall_filetime)
        if (uint64_t v; cp->wall_filetime(&v)) return v;
    return os::wall_filetime();
}

uint64_t system_tick() {
    if (const ClockProvider* cp = g_clock.load(std::memory_order_acquire); cp && cp->tick)
        if (uint64_t vt; cp->tick(&vt)) return vt;
    const uint64_t ns = os::monotonic_ns();
    return (ns / 1000000000ull) * kTickHz + (ns % 1000000000ull) * kTickHz / 1000000000ull;
}

// SleepThread takes nanoseconds and the console timer is good to a few microseconds. Windows' Sleep() counts whole milliseconds on a
// 1 ms or 15.6 ms grid, so os::sleep_ns waits on a high-resolution timer; the whole-millisecond sleep is only its fallback.
// A zero or negative span yields, as on the console.
void sleep_ns(int64_t ns) {
    if (const ClockProvider* cp = g_clock.load(std::memory_order_acquire); cp && cp->sleep && cp->sleep(ns)) return;
    if (ns <= 0) {
        yield_host();
        return;
    }
    os::sleep_ns(ns);
}

}
