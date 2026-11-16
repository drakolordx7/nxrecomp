// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt::os sync: the host's lock and wait primitives. Windows: SRWLOCK and CONDITION_VARIABLE. Linux: futex based (the same algorithms as
// the Rust standard library's futex locks). Not the guest-address wait objects of the kernel (nxrt/sync.h).
//
// All three types are constant-initialised (a namespace-scope object is ready before any code runs, as SRWLOCK_INIT was), cannot be copied,
// are not recursive and never need a destroy call. Mutex and SharedMutex work with std::lock_guard, std::unique_lock and std::shared_lock.
#pragma once
#include <cstdint>

namespace nxrt::os {

// Exclusive lock (Windows: SRWLOCK used exclusively). Not recursive; unlock from the thread that locked it.
class Mutex {
  public:
    constexpr Mutex() = default;
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
    void lock();
    bool try_lock();
    void unlock();

  private:
    friend class CondVar;
    alignas(8) uint32_t words_[2] = {0, 0};
};

// Reader/writer lock (Windows: SRWLOCK). Not recursive. A blocked writer holds back new readers (Linux) or not (Windows: SRW lets readers
// in while others read); callers must not depend on either.
class SharedMutex {
  public:
    constexpr SharedMutex() = default;
    SharedMutex(const SharedMutex&) = delete;
    SharedMutex& operator=(const SharedMutex&) = delete;
    void lock();
    bool try_lock();
    void unlock();
    void lock_shared();
    bool try_lock_shared();
    void unlock_shared();

  private:
    alignas(8) uint32_t words_[2] = {0, 0};
};

// Condition variable for a Mutex (Windows: CONDITION_VARIABLE with SleepConditionVariableSRW). Mesa semantics: a wait can return without a
// notification (spurious), so callers re-check their condition in a loop. A notification issued after a waiter has released the mutex
// inside wait() is never lost.
class CondVar {
  public:
    constexpr CondVar() = default;
    CondVar(const CondVar&) = delete;
    CondVar& operator=(const CondVar&) = delete;
    // Releases `m` (which the caller holds), waits, takes `m` again before returning.
    void wait(Mutex& m);
    // The same with a timeout in nanoseconds (negative = none). Returns false when the wait ended because the time ran out, true otherwise.
    // The wait never ends early on the clock; Windows rounds it up to whole milliseconds, Linux is exact to the timer slack.
    bool wait_for(Mutex& m, int64_t timeout_ns);
    void notify_one();
    void notify_all();

  private:
    alignas(8) uint32_t words_[2] = {0, 0};
};

}  // namespace nxrt::os
