// SPDX-License-Identifier: GPL-3.0-or-later
// System tick and high-resolution sleep. Pattern from smo-recomp runtime/os/os_threads.c sleep_ns and SuperMarioWonderRecomp kernel.cpp system_tick.
#include <windows.h>

#include <atomic>

#include "nxrt/kernel.h"

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
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

uint64_t system_tick() {
    if (const ClockProvider* cp = g_clock.load(std::memory_order_acquire); cp && cp->tick)
        if (uint64_t vt; cp->tick(&vt)) return vt;
    static const uint64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<uint64_t>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const uint64_t v = static_cast<uint64_t>(c.QuadPart);
    return (v / freq) * kTickHz + (v % freq) * kTickHz / freq;
}

// SleepThread takes nanoseconds and the console timer is good to a few microseconds. Sleep() counts whole milliseconds on a
// 1 ms or 15.6 ms grid, so the wait runs on a high-resolution waitable timer (100 ns units); Sleep() is only the fallback.
// A zero or negative span yields, as on the console.
void sleep_ns(int64_t ns) {
    if (const ClockProvider* cp = g_clock.load(std::memory_order_acquire); cp && cp->sleep && cp->sleep(ns)) return;
    static thread_local HANDLE timer = nullptr;
    static thread_local bool no_timer = false;
    if (ns <= 0) {
        yield_host();
        return;
    }
    if (!timer && !no_timer) {
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) no_timer = true;
    }
    LARGE_INTEGER due;
    due.QuadPart = -((ns + 99) / 100);
    if (!timer || !SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        Sleep(static_cast<DWORD>((ns + 999999) / 1000000));
    else
        WaitForSingleObject(timer, INFINITE);
}

}
