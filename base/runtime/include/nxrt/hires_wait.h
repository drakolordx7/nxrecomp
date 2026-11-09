// SPDX-License-Identifier: GPL-3.0-or-later
// One wait on a high-resolution waitable timer, for the kernel's SleepThread and the frame limiter. Sleep() counts whole milliseconds on a 1 ms or
// 15.6 ms grid; the timer is good to tens of microseconds. Header only; <windows.h> is included here.
#pragma once
#include <cstdint>

#include <windows.h>

namespace nxrt {

// Waits `ns` nanoseconds (at least one 100 ns unit) on this thread's timer, created on first use and kept for the thread. False, having waited
// nothing, when the system has no high-resolution timer: the caller then falls back to Sleep.
inline bool hires_wait_ns(int64_t ns) {
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

}  // namespace nxrt
