// SPDX-License-Identifier: GPL-3.0-or-later
// Linux implementation of nxrt/os/time.h.
#include "nxrt/os/time.h"

#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <fstream>

#include <sched.h>
#include <unistd.h>
#include <sys/prctl.h>

namespace nxrt::os {
namespace {

uint64_t clock_ns(clockid_t id) {
    timespec ts;
    clock_gettime(id, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

// The default timer slack of a thread is 50 us: a sleep may end that much late. A thread that sleeps precisely asks for 1 ns, once.
void ensure_fine_slack() {
    static thread_local bool done = false;
    if (done) return;
    done = true;
    prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);
}

}  // namespace

uint64_t monotonic_ns() { return clock_ns(CLOCK_MONOTONIC); }

uint64_t tick_ms() { return clock_ns(CLOCK_MONOTONIC_COARSE) / 1000000ull; }

uint64_t wall_filetime() {
    constexpr uint64_t kUnixToFiletime = 11644473600ull;      // seconds between 1601-01-01 and 1970-01-01
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (static_cast<uint64_t>(ts.tv_sec) + kUnixToFiletime) * 10000000ull + static_cast<uint64_t>(ts.tv_nsec) / 100;
}

void wait_until_ns(uint64_t target_ns) {
    ensure_fine_slack();
    timespec ts;
    ts.tv_sec = static_cast<time_t>(target_ns / 1000000000ull);
    ts.tv_nsec = static_cast<long>(target_ns % 1000000000ull);
    // clock_nanosleep returns the error number itself; an absolute sleep resumes at the same deadline after a signal
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
}

bool sleep_precise_ns(int64_t ns) {
    wait_until_ns(monotonic_ns() + static_cast<uint64_t>(ns > 0 ? ns : 1));
    return true;
}

void sleep_ns(int64_t ns) {
    if (ns > 0) sleep_precise_ns(ns);
}

void sleep_ms(uint32_t ms) {
    if (ms == 0) {
        sched_yield();
        return;
    }
    sleep_precise_ns(static_cast<int64_t>(ms) * 1000000);
}

void timer_resolution_acquire() {}
void timer_resolution_release() {}

// ---- the host's time zone -----------------------------------------------------------------------------------------------------------

namespace {

struct LocalState {
    bool dst;
    int32_t off;
};
LocalState local_state(int64_t t) {
    const time_t tt = static_cast<time_t>(t);
    tm r{};
    localtime_r(&tt, &r);
    return {r.tm_isdst > 0, static_cast<int32_t>(r.tm_gmtoff)};
}
// The first second in (lo, hi] whose dst flag differs from the one at lo; the flags at lo and hi must differ.
int64_t find_change(int64_t lo, int64_t hi) {
    const bool d = local_state(lo).dst;
    while (hi - lo > 1) {
        const int64_t mid = lo + (hi - lo) / 2;
        (local_state(mid).dst == d ? lo : hi) = mid;
    }
    return hi;
}
int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {      // Howard Hinnant's algorithm (public domain)
    y -= m <= 2 ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

std::string zone_key() {
    if (const char* tz = std::getenv("TZ"); tz && *tz) return tz[0] == ':' ? std::string(tz + 1) : std::string(tz);
    char buf[512];
    const ssize_t n = readlink("/etc/localtime", buf, sizeof buf - 1);
    if (n > 0) {
        const std::string link(buf, static_cast<size_t>(n));
        const size_t at = link.find("zoneinfo/");
        if (at != std::string::npos) return link.substr(at + 9);
    }
    std::ifstream f("/etc/timezone");
    std::string line;
    if (f && std::getline(f, line)) return line;
    return std::string();
}

}  // namespace

bool host_timezone(HostZone* out, int first_year, int last_year) {
    tzset();
    out->key = zone_key();
    out->years.clear();
    for (int year = first_year; year <= last_year; ++year) {
        const int64_t t0 = days_from_civil(year, 1, 1) * 86400, t1 = days_from_civil(year, 7, 1) * 86400, t2 = days_from_civil(year + 1, 1, 1) * 86400;
        const LocalState a = local_state(t0), b = local_state(t1);
        HostZoneYear y;
        if (a.dst == b.dst) {
            y.std_off = y.dst_off = a.off;
        } else {
            y.has_dst = true;
            y.std_off = a.dst ? b.off : a.off;
            y.dst_off = a.dst ? a.off : b.off;
            const int64_t first = find_change(t0, t1), second = find_change(t1, t2);
            y.dst_start = a.dst ? second : first;      // a year that begins in dst (southern hemisphere) leaves it first
            y.dst_end = a.dst ? first : second;
        }
        out->years.push_back(y);
    }
    return true;
}

}  // namespace nxrt::os
