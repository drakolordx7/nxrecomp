// SPDX-License-Identifier: GPL-3.0-or-later
// Linux implementation of nxrt/os/time.h.
#include "nxrt/os/time.h"

#include <cerrno>
#include <ctime>

#include <sched.h>
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

}  // namespace nxrt::os
