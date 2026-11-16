// SPDX-License-Identifier: GPL-3.0-or-later
// Linux implementation of nxrt/os/sync.h on futexes. The algorithms are the ones of the Rust standard library's futex locks (Mutex, RwLock
// and Condvar, MIT / Apache-2.0): a three-state mutex, a reader/writer lock that keeps its waiter bits in the state word, and a condition
// variable that is a sequence counter.
#include "nxrt/os/sync.h"

#include <cstdlib>

#include "futex.h"

namespace nxrt::os {
namespace {

using u32 = uint32_t;
using posix::futex_wait;
using posix::futex_wake_all;
using posix::futex_wake_one;

inline u32 load(const u32* p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }
// Weak-free compare and swap; on failure `expected` receives the current value.
inline bool cas(u32* p, u32& expected, u32 desired, int success = __ATOMIC_ACQUIRE) {
    return __atomic_compare_exchange_n(p, &expected, desired, false, success, __ATOMIC_RELAXED);
}

// ---- Mutex: 0 unlocked, 1 locked, 2 locked with sleepers --------------------------------------------------------------------

u32 mutex_spin(const u32* f) {
    for (int i = 0; i < 100; i++) {
        const u32 s = load(f);
        if (s != 1) return s;
        posix::relax();
    }
    return load(f);
}

void mutex_contended(u32* f) {
    u32 state = mutex_spin(f);
    if (state == 0) {
        u32 e = 0;
        if (cas(f, e, 1)) return;
        state = e;
    }
    for (;;) {
        if (state != 2 && __atomic_exchange_n(f, 2, __ATOMIC_ACQUIRE) == 0) return;
        futex_wait(f, 2);
        state = mutex_spin(f);
    }
}

// ---- SharedMutex: state = reader count (30 bits) | READERS_WAITING | WRITERS_WAITING, writers sleep on a second word --------------

constexpr u32 kReadLocked = 1;
constexpr u32 kMask = (1u << 30) - 1;
constexpr u32 kWriteLocked = kMask;
constexpr u32 kMaxReaders = kMask - 1;
constexpr u32 kReadersWaiting = 1u << 30;
constexpr u32 kWritersWaiting = 1u << 31;

inline bool is_unlocked(u32 s) { return (s & kMask) == 0; }
inline bool is_write_locked(u32 s) { return (s & kMask) == kWriteLocked; }
inline bool has_readers_waiting(u32 s) { return (s & kReadersWaiting) != 0; }
inline bool has_writers_waiting(u32 s) { return (s & kWritersWaiting) != 0; }
inline bool is_read_lockable(u32 s) { return (s & kMask) < kMaxReaders && !has_readers_waiting(s) && !has_writers_waiting(s); }

u32 spin_read(const u32* st) {
    for (int i = 0; i < 100; i++) {
        const u32 s = load(st);
        if (!is_write_locked(s) || has_readers_waiting(s) || has_writers_waiting(s)) return s;
        posix::relax();
    }
    return load(st);
}

u32 spin_write(const u32* st) {
    for (int i = 0; i < 100; i++) {
        const u32 s = load(st);
        if (is_unlocked(s) || has_writers_waiting(s) || has_readers_waiting(s)) return s;
        posix::relax();
    }
    return load(st);
}

bool wake_writer(u32* notify) {
    __atomic_fetch_add(notify, 1, __ATOMIC_RELEASE);
    return futex_wake_one(notify);
}

// Called with the lock free (state has no holder) when a waiter bit is set: hands it to a writer or lets the readers in.
void wake_writer_or_readers(u32* st, u32* notify, u32 state) {
    if (state == kWritersWaiting) {
        if (cas(st, state, 0, __ATOMIC_RELAXED)) {
            wake_writer(notify);
            return;
        }
    }
    if (state == (kReadersWaiting | kWritersWaiting)) {
        if (!cas(st, state, kReadersWaiting, __ATOMIC_RELAXED)) return;     // another thread locked it in the meantime
        if (wake_writer(notify)) return;
        // no writer was actually asleep: wake the readers instead
        state = kReadersWaiting;
    }
    if (state == kReadersWaiting) {
        if (cas(st, state, 0, __ATOMIC_RELAXED)) futex_wake_all(st);
    }
}

void read_contended(u32* st) {
    u32 state = spin_read(st);
    for (;;) {
        if (is_read_lockable(state)) {
            if (cas(st, state, state + kReadLocked)) return;
            continue;
        }
        if ((state & kMask) == kMaxReaders) std::abort();     // 2^30 readers: a bug in the caller
        if (!has_readers_waiting(state)) {
            if (!cas(st, state, state | kReadersWaiting, __ATOMIC_RELAXED)) continue;
            state |= kReadersWaiting;
        }
        futex_wait(st, state);
        state = spin_read(st);
    }
}

void write_contended(u32* st, u32* notify) {
    u32 state = spin_write(st);
    u32 other_writers_waiting = 0;
    for (;;) {
        if (is_unlocked(state)) {
            if (cas(st, state, state | kWriteLocked | other_writers_waiting)) return;
            continue;
        }
        if (!has_writers_waiting(state)) {
            if (!cas(st, state, state | kWritersWaiting, __ATOMIC_RELAXED)) continue;
        }
        // other writers may be waiting now too: keep the bit on once this one gets the lock
        other_writers_waiting = kWritersWaiting;
        // read the notification counter before looking at the state again, so no notification is missed
        const u32 seq = __atomic_load_n(notify, __ATOMIC_ACQUIRE);
        state = load(st);
        if (is_unlocked(state) || !has_writers_waiting(state)) continue;
        futex_wait(notify, seq);
        state = spin_write(st);
    }
}

}  // namespace

// ---- Mutex ----------------------------------------------------------------------------------------------------------------------

void Mutex::lock() {
    u32 e = 0;
    if (!cas(&words_[0], e, 1)) mutex_contended(&words_[0]);
}

bool Mutex::try_lock() {
    u32 e = 0;
    return cas(&words_[0], e, 1);
}

void Mutex::unlock() {
    if (__atomic_exchange_n(&words_[0], 0, __ATOMIC_RELEASE) == 2) futex_wake_one(&words_[0]);
}

// ---- SharedMutex ----------------------------------------------------------------------------------------------------------------

void SharedMutex::lock_shared() {
    u32 s = load(&words_[0]);
    if (!is_read_lockable(s) || !cas(&words_[0], s, s + kReadLocked)) read_contended(&words_[0]);
}

bool SharedMutex::try_lock_shared() {
    u32 s = load(&words_[0]);
    for (;;) {
        if (!is_read_lockable(s)) return false;
        if (cas(&words_[0], s, s + kReadLocked)) return true;
    }
}

void SharedMutex::unlock_shared() {
    const u32 state = __atomic_fetch_sub(&words_[0], kReadLocked, __ATOMIC_RELEASE) - kReadLocked;
    // readers only wait while a writer waits too
    if (is_unlocked(state) && has_writers_waiting(state)) wake_writer_or_readers(&words_[0], &words_[1], state);
}

void SharedMutex::lock() {
    u32 e = 0;
    if (!cas(&words_[0], e, kWriteLocked)) write_contended(&words_[0], &words_[1]);
}

bool SharedMutex::try_lock() {
    u32 s = load(&words_[0]);
    for (;;) {
        if (!is_unlocked(s)) return false;
        if (cas(&words_[0], s, s + kWriteLocked)) return true;
    }
}

void SharedMutex::unlock() {
    const u32 state = __atomic_fetch_sub(&words_[0], kWriteLocked, __ATOMIC_RELEASE) - kWriteLocked;
    if (has_writers_waiting(state) || has_readers_waiting(state)) wake_writer_or_readers(&words_[0], &words_[1], state);
}

// ---- CondVar: words_[0] counts notifications -------------------------------------------------------------------------------------

void CondVar::wait(Mutex& m) { wait_for(m, -1); }

bool CondVar::wait_for(Mutex& m, int64_t timeout_ns) {
    const u32 seen = load(&words_[0]);
    m.unlock();
    const bool woken = futex_wait(&words_[0], seen, timeout_ns);
    m.lock();
    return woken;
}

void CondVar::notify_one() {
    __atomic_fetch_add(&words_[0], 1, __ATOMIC_RELEASE);
    futex_wake_one(&words_[0]);
}

void CondVar::notify_all() {
    __atomic_fetch_add(&words_[0], 1, __ATOMIC_RELEASE);
    futex_wake_all(&words_[0]);
}

}  // namespace nxrt::os
