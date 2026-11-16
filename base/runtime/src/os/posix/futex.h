// SPDX-License-Identifier: GPL-3.0-or-later
// Private to src/os/posix: the futex system call (process-private, CLOCK_MONOTONIC timeouts).
#pragma once
#include <cerrno>
#include <climits>
#include <cstdint>

#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

namespace nxrt::os::posix {

// Sleeps while *addr == expected, until woken or timeout_ns (negative = no limit) has passed. False only when the time ran out;
// true when woken, when *addr differed, or on a signal (callers always re-check).
inline bool futex_wait(const uint32_t* addr, uint32_t expected, int64_t timeout_ns = -1) {
    timespec ts;
    timespec* pts = nullptr;
    if (timeout_ns >= 0) {
        if (timeout_ns > static_cast<int64_t>(INT64_MAX / 2)) timeout_ns = INT64_MAX / 2;
        ts.tv_sec = static_cast<time_t>(timeout_ns / 1000000000);
        ts.tv_nsec = static_cast<long>(timeout_ns % 1000000000);
        pts = &ts;
    }
    const long r = syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, pts, nullptr, 0);
    return !(r == -1 && errno == ETIMEDOUT);
}

// Wakes up to `count` sleepers; returns how many were woken.
inline long futex_wake(const uint32_t* addr, int count) { return syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, count, nullptr, nullptr, 0); }
inline bool futex_wake_one(const uint32_t* addr) { return futex_wake(addr, 1) > 0; }
inline void futex_wake_all(const uint32_t* addr) { futex_wake(addr, INT_MAX); }

inline void relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __builtin_arm_yield();
#endif
}

}  // namespace nxrt::os::posix
