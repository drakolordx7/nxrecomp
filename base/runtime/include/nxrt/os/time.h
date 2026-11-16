// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt::os time: the host's monotonic clock, wall clock and sleeps. Windows: QueryPerformanceCounter, GetTickCount64, the high-resolution
// waitable timer, timeBeginPeriod. Linux: clock_gettime, clock_nanosleep. See README.md in this folder for the rules of the layer.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

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

// ---- the host's time zone -----------------------------------------------------------------------------------------------------------

// One year of the host's local time rule: the offset from UTC in standard time and in daylight saving time (seconds, east positive) and the
// two instants of the year at which the clocks change (seconds since the posix epoch, UTC). has_dst false: no change that year.
struct HostZoneYear {
    bool has_dst = false;
    int32_t std_off = 0, dst_off = 0;
    int64_t dst_start = 0, dst_end = 0;      // clocks go to dst_off at dst_start and back to std_off at dst_end (either may come first in the year)
};
struct HostZone {
    std::string key;                         // Windows: the time zone key name ("W. Europe Standard Time"); Linux: the IANA name ("Europe/Berlin"); empty when unknown
    std::vector<HostZoneYear> years;         // first_year .. last_year, in order
};
// Reads the host's zone for the years first_year..last_year (Windows: GetDynamicTimeZoneInformation and GetTimeZoneInformationForYear;
// Linux: the C library's local time, probed at the year's changes). False when the host does not say.
bool host_timezone(HostZone* out, int first_year, int last_year);

}  // namespace nxrt::os
