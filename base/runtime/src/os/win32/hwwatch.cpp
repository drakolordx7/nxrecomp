// SPDX-License-Identifier: GPL-3.0-or-later
// Windows implementation of nxrt/os/hwwatch.h. The code moved here from games/mpr/runtime/mpr_tt_probe.cpp (@watch): debug registers DR0 and
// DR1 on every thread, a vectored handler for EXCEPTION_SINGLE_STEP that reports the hit and clears DR6.
#include "nxrt/os/hwwatch.h"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>

namespace nxrt::os {
namespace {

std::atomic<bool> g_on{false};
uint64_t g_addr[2] = {0, 0};
HwWatchFn g_fn = nullptr;
void* g_veh = nullptr;

LONG CALLBACK watch_veh(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_on.load()) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* cx = ep->ContextRecord;
    const DWORD64 dr6 = cx->Dr6;
    if (!(dr6 & 0x3)) return EXCEPTION_CONTINUE_SEARCH;
    for (int slot = 0; slot < 2; slot++) {
        if (!(dr6 & (1u << slot))) continue;
        HwWatchHit h;
        h.slot = slot;
        h.pc = cx->Rip;
        h.sp = cx->Rsp;
        h.tid = GetCurrentThreadId();
        if (g_fn) g_fn(h);
    }
    cx->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void set_debug_regs(bool on) {
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
        if (!th) continue;
        const bool self = te.th32ThreadID == GetCurrentThreadId();
        if (!self) SuspendThread(th);
        CONTEXT cx{};
        cx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(th, &cx)) {
            cx.Dr0 = on ? g_addr[0] : 0;
            cx.Dr1 = on ? g_addr[1] : 0;
            DWORD64 d7 = 0;
            if (on) {
                if (g_addr[0]) d7 |= (1ull << 0) | (1ull << 16) | (3ull << 18);      // slot 0: local enable, write, 4 bytes
                if (g_addr[1]) d7 |= (1ull << 2) | (1ull << 20) | (3ull << 22);      // slot 1
            }
            cx.Dr7 = d7;
            cx.Dr6 = 0;
            SetThreadContext(th, &cx);
        }
        if (!self) ResumeThread(th);
        CloseHandle(th);
    }
    CloseHandle(snap);
}

}  // namespace

bool hw_watch_supported() { return true; }

bool hw_watch_start(const uint64_t addr[2], HwWatchFn fn) {
    if (!addr[0] && !addr[1]) return false;
    if (g_on.load()) hw_watch_stop();
    g_addr[0] = addr[0];
    g_addr[1] = addr[1];
    g_fn = fn;
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, watch_veh);
    g_on = true;
    set_debug_regs(true);
    return true;
}

void hw_watch_stop() {
    if (!g_on.load()) return;
    set_debug_regs(false);
    g_on = false;
}

}  // namespace nxrt::os
