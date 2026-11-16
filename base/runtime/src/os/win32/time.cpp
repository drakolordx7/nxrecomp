// SPDX-License-Identifier: GPL-3.0-or-later
// Windows implementation of nxrt/os/time.h.
#include "nxrt/os/time.h"

#include <windows.h>
#include <mmsystem.h>

#include <atomic>

#include "nxrt/strutil.h"

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

// ---- the host's time zone -----------------------------------------------------------------------------------------------------------

namespace {

int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {      // Howard Hinnant's algorithm (public domain)
    y -= m <= 2 ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
int64_t floor_mod7(int64_t a) { return ((a % 7) + 7) % 7; }
int days_in_month(int64_t y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : d[m - 1];
}

// Local wall time of a SYSTEMTIME as seconds since the epoch in that wall clock. A wYear of 0 is a recurring rule:
// wDay is the n-th wDayOfWeek of the month, 5 meaning the last.
int64_t resolve_systemtime(const SYSTEMTIME& st, int year) {
    const int64_t y = st.wYear != 0 ? st.wYear : year;
    const int64_t first = days_from_civil(y, st.wMonth, 1);
    int64_t day_of_month;
    if (st.wYear != 0) {
        day_of_month = st.wDay;
    } else {
        const int64_t first_wd = floor_mod7(first + 4);
        day_of_month = 1 + floor_mod7(st.wDayOfWeek - first_wd) + 7 * (st.wDay - 1);
        while (day_of_month > days_in_month(y, st.wMonth)) day_of_month -= 7;
    }
    return (first + day_of_month - 1) * 86400 + st.wHour * 3600 + st.wMinute * 60 + st.wSecond;
}

}  // namespace

bool host_timezone(HostZone* out, int first_year, int last_year) {
    DYNAMIC_TIME_ZONE_INFORMATION dtzi{};
    if (GetDynamicTimeZoneInformation(&dtzi) == TIME_ZONE_ID_INVALID) return false;
    out->key = str::narrow(dtzi.TimeZoneKeyName);
    out->years.clear();
    for (int year = first_year; year <= last_year; ++year) {
        TIME_ZONE_INFORMATION tzi{};
        if (!GetTimeZoneInformationForYear(static_cast<USHORT>(year), &dtzi, &tzi)) return false;
        HostZoneYear y;
        y.std_off = -(tzi.Bias + tzi.StandardBias) * 60;
        y.dst_off = -(tzi.Bias + tzi.DaylightBias) * 60;
        if (tzi.DaylightDate.wMonth != 0 && tzi.StandardDate.wMonth != 0) {
            y.has_dst = true;
            // DaylightDate is given in standard wall time, StandardDate in daylight wall time.
            y.dst_start = resolve_systemtime(tzi.DaylightDate, year) - y.std_off;
            y.dst_end = resolve_systemtime(tzi.StandardDate, year) - y.dst_off;
        }
        out->years.push_back(y);
    }
    return true;
}

}  // namespace nxrt::os
