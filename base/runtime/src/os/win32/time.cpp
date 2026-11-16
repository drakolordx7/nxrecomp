// SPDX-License-Identifier: GPL-3.0-or-later
// Windows implementation of nxrt/os/time.h.
#include "nxrt/os/time.h"

#include <windows.h>
#include <mmsystem.h>

#include <atomic>

#pragma comment(lib, "winmm.lib")

namespace nxrt::os {

uint64_t monotonic_ns() {
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

uint64_t tick_ms() { return GetTickCount64(); }

uint64_t wall_filetime() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// One wait on a high-resolution waitable timer. Sleep() counts whole milliseconds on a 1 ms or 15.6 ms grid; the timer is good to tens of
// microseconds. The timer is created on first use and kept for the thread.
bool sleep_precise_ns(int64_t ns) {
    static thread_local HANDLE timer = nullptr;
    static thread_local bool no_timer = false;
    if (!timer && !no_timer) {
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) no_timer = true;
    }
    if (!timer) return false;
    LARGE_INTEGER due;
    const int64_t units = (ns + 99) / 100;   // 100 ns units; negative = relative
    due.QuadPart = -(units > 0 ? units : 1);
    if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) return false;
    WaitForSingleObject(timer, INFINITE);
    return true;
}

void sleep_ns(int64_t ns) {
    if (ns <= 0) return;
    if (!sleep_precise_ns(ns)) Sleep(static_cast<DWORD>((ns + 999999) / 1000000));
}

void sleep_ms(uint32_t ms) { Sleep(ms); }

void wait_until_ns(uint64_t target_ns) {
    for (;;) {
        const uint64_t now = monotonic_ns();
        if (now >= target_ns) return;
        sleep_ns(static_cast<int64_t>(target_ns - now));
    }
}

namespace {
std::atomic<int> g_resolution_holders{0};
}

void timer_resolution_acquire() {
    if (g_resolution_holders.fetch_add(1) == 0) timeBeginPeriod(1);
}

void timer_resolution_release() {
    if (g_resolution_holders.fetch_sub(1) == 1) timeEndPeriod(1);
}

}  // namespace nxrt::os
