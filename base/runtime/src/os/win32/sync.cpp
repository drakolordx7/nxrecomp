// SPDX-License-Identifier: GPL-3.0-or-later
// Windows implementation of nxrt/os/sync.h: SRWLOCK and CONDITION_VARIABLE (both pointer sized, zero is the initial state).
#include "nxrt/os/sync.h"

#include <windows.h>

namespace nxrt::os {
namespace {

static_assert(sizeof(SRWLOCK) == 8 && sizeof(CONDITION_VARIABLE) == 8, "the os sync types hold one SRWLOCK / CONDITION_VARIABLE");
static_assert(sizeof(Mutex) == 8 && sizeof(SharedMutex) == 8 && sizeof(CondVar) == 8, "os sync object size");

SRWLOCK* srw(void* p) { return static_cast<SRWLOCK*>(p); }
CONDITION_VARIABLE* cv(void* p) { return static_cast<CONDITION_VARIABLE*>(p); }

}  // namespace

void Mutex::lock() { AcquireSRWLockExclusive(srw(words_)); }
bool Mutex::try_lock() { return TryAcquireSRWLockExclusive(srw(words_)) != 0; }
void Mutex::unlock() { ReleaseSRWLockExclusive(srw(words_)); }

void SharedMutex::lock() { AcquireSRWLockExclusive(srw(words_)); }
bool SharedMutex::try_lock() { return TryAcquireSRWLockExclusive(srw(words_)) != 0; }
void SharedMutex::unlock() { ReleaseSRWLockExclusive(srw(words_)); }
void SharedMutex::lock_shared() { AcquireSRWLockShared(srw(words_)); }
bool SharedMutex::try_lock_shared() { return TryAcquireSRWLockShared(srw(words_)) != 0; }
void SharedMutex::unlock_shared() { ReleaseSRWLockShared(srw(words_)); }

void CondVar::wait(Mutex& m) { SleepConditionVariableSRW(cv(words_), srw(m.words_), INFINITE, 0); }

bool CondVar::wait_for(Mutex& m, int64_t timeout_ns) {
    if (timeout_ns < 0) {
        wait(m);
        return true;
    }
    int64_t ms = (timeout_ns + 999999) / 1000000;
    if (ms > 0x7FFFFFFE) ms = 0x7FFFFFFE;      // one below INFINITE
    return SleepConditionVariableSRW(cv(words_), srw(m.words_), static_cast<DWORD>(ms), 0) != 0;
}

void CondVar::notify_one() { WakeConditionVariable(cv(words_)); }
void CondVar::notify_all() { WakeAllConditionVariable(cv(words_)); }

}  // namespace nxrt::os
