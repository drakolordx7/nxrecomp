// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt::os time: the host's monotonic clock, wall clock and sleeps. Windows: QueryPerformanceCounter, GetTickCount64, the high-resolution
// waitable timer, timeBeginPeriod. Linux: clock_gettime, clock_nanosleep. See README.md in this folder for the rules of the layer.
#pragma once
#include <cstdint>

namespace nxrt::os {

// Nanoseconds on the host's monotonic clock (Windows: QueryPerformanceCounter; Linux: CLOCK_MONOTONIC). The origin is unspecified; only
// differences mean something. Thread safe, a few tens of nanoseconds.
uint64_t monotonic_ns();

// Milliseconds on a coarse monotonic clock (Windows: GetTickCount64, a 10 to 16 ms grid; Linux: CLOCK_MONOTONIC_COARSE). For timeouts and logs.
uint64_t tick_ms();

// Wall clock as 100 ns units since 1601-01-01 (Windows FILETIME), at the best resolution the host has.
uint64_t wall_filetime();

// Sleeps `ns` nanoseconds with a resolution of tens of microseconds (Windows: a high-resolution waitable timer, one per thread, created
// on first use; Linux: clock_nanosleep on CLOCK_MONOTONIC with the thread's timer slack at 1 ns). Returns false, having waited nothing,
// when the host has no such timer (Windows before 10 1803): the caller then falls back to sleep_ms. ns <= 0 waits the shortest span.
bool sleep_precise_ns(int64_t ns);

// Sleeps at least `ns` nanoseconds: sleep_precise_ns, or whole milliseconds rounded up where that is not available. ns <= 0 returns at once.
void sleep_ns(int64_t ns);

// Sleeps `ms` milliseconds on the host's ordinary sleep (Windows: Sleep, on the system timer grid; Linux: a precise sleep). 0 yields.
void sleep_ms(uint32_t ms);

// Returns when monotonic_ns() >= target_ns (sleeping precisely, not spinning).
void wait_until_ns(uint64_t target_ns);

// Asks for the finest timer grid the host offers while at least one holder exists (Windows: timeBeginPeriod(1) / timeEndPeriod(1),
// reference counted; Linux: nothing to do, timers are always fine). Calls must pair.
void timer_resolution_acquire();
void timer_resolution_release();

}  // namespace nxrt::os
