// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt::os hwwatch: CPU data breakpoints on this process's own memory, for developer probes that ask "who writes this value". Windows: the debug
// registers DR0 and DR1 set on every thread (SuspendThread, SetThreadContext) and a vectored handler for the single-step exception. Linux: not
// provided: a process cannot set debug registers on itself without ptrace, and page protection cannot tell the writer without single-stepping
// (which needs a SIGTRAP handler the fault layer does not own). hw_watch_supported() says which; callers fall back to polling the value.
#pragma once
#include <cstdint>

namespace nxrt::os {

struct HwWatchHit {
    int slot = 0;              // which of the two addresses was written
    uint64_t pc = 0;           // the instruction after the writing one
    uint64_t sp = 0;           // stack pointer of the writing thread: the callback may scan the stack upwards (use os::guard::read)
    uint32_t tid = 0;
};
// Called on the writing thread inside the exception handler: no locks, no allocation, no logging.
using HwWatchFn = void (*)(const HwWatchHit& hit);

bool hw_watch_supported();
// Watches 4-byte writes to addr[0] and addr[1] (0 = slot unused) on every thread that exists now. False when unsupported or nothing to watch.
bool hw_watch_start(const uint64_t addr[2], HwWatchFn fn);
void hw_watch_stop();

}  // namespace nxrt::os
