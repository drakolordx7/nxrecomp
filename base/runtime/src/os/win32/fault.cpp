// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt::os fault layer on Windows (include/nxrt/os/fault.h): one vectored exception handler (registered "first", where the write tracker's
// handler was before) runs the chain; guards are structured exception handling; guest traps are RaiseException.
#include "../fault_chain.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstring>

namespace nxrt::os {
namespace {

bool is_trap_code(DWORD code) { return (code & 0xFFFFFFF0u) == 0xE0000000u; }

// Fills fi from an exception; false for exceptions that do not concern the chain.
bool describe(EXCEPTION_POINTERS* ep, FaultInfo* fi) {
    if (!ep || !ep->ExceptionRecord) return false;
    const EXCEPTION_RECORD* rec = ep->ExceptionRecord;
    const DWORD code = rec->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        fi->kind = FaultKind::AccessViolation;
        fi->is_write = rec->ExceptionInformation[0] == 1;
        fi->addr = static_cast<uint64_t>(rec->ExceptionInformation[1]);
    } else if (code == EXCEPTION_STACK_OVERFLOW) {
        fi->kind = FaultKind::StackOverflow;
        fi->addr = rec->NumberParameters >= 2 ? static_cast<uint64_t>(rec->ExceptionInformation[1]) : 0;
    } else if (is_trap_code(code)) {
        fi->kind = FaultKind::GuestTrap;
    } else {
        return false;
    }
    fi->code = code;
    if (ep->ContextRecord) {
        fi->pc = ep->ContextRecord->Rip;
        fi->sp = ep->ContextRecord->Rsp;
    }
    fi->tid = GetCurrentThreadId();
    fi->native = ep;
    return true;
}

LONG CALLBACK veh(EXCEPTION_POINTERS* ep) {
    FaultInfo fi;
    if (!describe(ep, &fi)) return EXCEPTION_CONTINUE_SEARCH;
    return detail::chain_run(fi) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* ep) {
    FaultInfo fi;
    if (!describe(ep, &fi)) {          // an exception the chain never sees: the reporter still wants it
        fi = FaultInfo{};
        fi.kind = FaultKind::Other;
        if (ep && ep->ExceptionRecord) fi.code = ep->ExceptionRecord->ExceptionCode;
        if (ep && ep->ContextRecord) { fi.pc = ep->ContextRecord->Rip; fi.sp = ep->ContextRecord->Rsp; }
        fi.tid = GetCurrentThreadId();
        fi.native = ep;
    }
    return detail::unhandled_run(fi) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

std::atomic<bool> g_filter_set{false};

// ---- guards: SEH, kept in functions without C++ objects. The access itself is in a non-inlined callee because clang-cl's SEH only models
// faults at call sites (no /EHa), so a plain store inside the __try block would not reliably reach the handler.
__declspec(noinline) void copy_bytes(void* dst, const void* src, size_t n) { std::memcpy(dst, src, n); }

int guard_filter(DWORD code, bool catch_trap) {
    if (code == EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_EXECUTE_HANDLER;
    if (catch_trap && is_trap_code(code)) return EXCEPTION_EXECUTE_HANDLER;
    return EXCEPTION_CONTINUE_SEARCH;
}

__declspec(noinline) bool guarded_copy(void* dst, const void* src, size_t n) {
    __try {
        copy_bytes(dst, src, n);
        return true;
    } __except (guard_filter(GetExceptionCode(), false)) {
        return false;
    }
}

__declspec(noinline) bool guarded_call(void (*fn)(void*), void* arg, bool catch_trap) {
    __try {
        fn(arg);
        return true;
    } __except (guard_filter(GetExceptionCode(), catch_trap)) {
        return false;
    }
}

}  // namespace

namespace detail {

void platform_install() { AddVectoredExceptionHandler(1, veh); }

void platform_unhandled_set() {
    if (!g_filter_set.exchange(true)) SetUnhandledExceptionFilter(unhandled_filter);
}

}  // namespace detail

void fault_thread_init() {}

void raise_guest_trap(uint32_t code) { RaiseException(code, EXCEPTION_NONCONTINUABLE, 0, nullptr); }

[[noreturn]] void terminate(uint32_t code) {
    TerminateProcess(GetCurrentProcess(), code);
    for (;;) Sleep(1000);
}

namespace guard {
bool read(const void* src, void* dst, size_t n) { return n == 0 || guarded_copy(dst, src, n); }
bool write(void* dst, const void* src, size_t n) { return n == 0 || guarded_copy(dst, src, n); }
bool call(void (*fn)(void*), void* arg, bool catch_guest_trap) { return guarded_call(fn, arg, catch_guest_trap); }
}  // namespace guard

}  // namespace nxrt::os
