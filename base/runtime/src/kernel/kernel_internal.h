// SPDX-License-Identifier: GPL-3.0-or-later
// Internal to src/kernel: state shared by thread.cpp, kernel_sync.cpp and svc_thread.cpp.
#pragma once
#include "nxrt/kernel.h"
#include "nxrt/os/sync.h"

namespace nxrt::kernel::detail {

void sync_reset();
void thread_mark_exited(KThread& t);     // marks exited under the kernel lock and wakes WaitSynchronization waiters (kernel_sync.cpp)
extern os::Mutex g_k;                    // the "kernel lock": wait queues and wait state of every thread
// Blocks the calling thread on its own condition variable with g_k held until t.wake or the deadline (ns, < 0 = forever).
bool block_on_wake(KThread& t, int64_t timeout_ns);
void wake_thread(KThread& t, uint32_t result = 0);   // g_k held

}
