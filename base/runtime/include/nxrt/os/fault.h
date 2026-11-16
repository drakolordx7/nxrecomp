// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt::os fault layer (src/os/win32/fault.cpp, src/os/posix/fault.cpp): the one place that sees host faults (Windows vectored exceptions,
// Linux SIGSEGV/SIGBUS), guest traps and process exit. Nobody else installs a vectored handler, a signal handler or an unhandled-exception
// filter for memory faults, and nobody else calls RaiseException or TerminateProcess.
//
// The chain contract (read this before registering anything, the crash reporter included):
//
//  1. One dispatcher per process. fault_register() adds a handler to an ordered chain; the first call also installs the dispatcher (VEH
//     with "first" set on Windows, sigaction SA_SIGINFO|SA_ONSTACK|SA_NODEFER on Linux) and the alternate stack of the calling thread.
//  2. A fault walks the chain from the highest priority down (equal priority: the handler registered first runs first). A handler that
//     repairs the cause (for example makes the page writable again) returns Handled: the faulting instruction is retried and the walk stops.
//     Continue means "not mine". No handler may block, wait for a lock another thread can hold while that thread faults, allocate, log or
//     print: it runs inside a signal handler on Linux, on the faulting thread, with the thread's other signals possibly pending.
//     Allowed: atomics, mprotect/os::mem::protect_if/query, writing to a pre-opened fd. (write_watch.cpp is the model.)
//  3. After the chain: if the faulting thread is inside a guard::read/write/call, the guard catches the fault (the call returns false).
//  4. Then the unhandled handler (fault_set_unhandled), if any: the crash reporter. It is the SetUnhandledExceptionFilter slot on Windows
//     and the last stop before the default action on Linux. Handled retries the instruction (so only return it after repairing), Continue
//     hands the fault to the system: the process dies as it would without us (Linux: default SIGSEGV action, core dump; Windows: normal
//     unhandled-exception behaviour). A reporter that wants to end the run itself calls terminate().
//  5. Only access violations, stack overflows and guest traps reach the chain; other exceptions (breakpoints, C++ exceptions, ...) do not.
//  6. Guest traps (raise_guest_trap) run the same chain with kind GuestTrap on the calling thread's normal stack (Linux) or as a vectored
//     exception (Windows). If nothing handles the trap raise_guest_trap returns and the caller ends the process with terminate().
//  7. Every thread that runs host code the runtime starts calls fault_thread_init() once at its start (an alternate stack on Linux; so
//     a stack overflow can be reported; a no-op on Windows). Calling it again is harmless.
//  Priorities: kPriorityFirst for repair handlers that must see every fault (write tracking), 0 for ordinary ones, kPriorityLast for reporters.
#pragma once
#include <cstddef>
#include <cstdint>

namespace nxrt::os {

enum class FaultKind : uint8_t { AccessViolation, StackOverflow, GuestTrap, Other };   // Other: only passed to the unhandled handler (Windows: any other exception, `code` says which)

struct FaultInfo {
    FaultKind kind = FaultKind::AccessViolation;
    uint32_t code = 0;          // GuestTrap: the code given to raise_guest_trap; else the native code (Windows exception code, Linux signal number)
    uint64_t addr = 0;          // AccessViolation / StackOverflow: the address the instruction touched
    bool is_write = false;      // AccessViolation: the instruction wrote (false for reads and instruction fetches)
    uint64_t pc = 0, sp = 0;    // instruction and stack pointer of the faulting thread (0 for a GuestTrap on Linux: no machine context)
    uint32_t tid = 0;           // host thread id (GetCurrentThreadId / gettid)
    void* native = nullptr;     // EXCEPTION_POINTERS* (Windows) or ucontext_t* (Linux, null for a GuestTrap): for the crash reporter's own os code
};

enum class FaultResult : uint8_t { Continue, Handled };
using FaultHandler = FaultResult (*)(const FaultInfo& info, void* user);

constexpr int kPriorityFirst = 100;
constexpr int kPriorityLast = -100;

// Guest trap codes (the values the Windows crash reporter has always shown).
constexpr uint32_t kTrapUnresolved = 0xE0000001u;   // dispatch: call of an address nothing is registered for
constexpr uint32_t kTrapUntranslated = 0xE0000002u; // guest code that was not translated
constexpr uint32_t kTrapBreak = 0xE0000003u;        // svcBreak / svcReturnFromException

// Adds a handler; returns an id for fault_unregister, or -1 (chain full: 16 entries). Any thread, not from a handler.
int fault_register(FaultHandler h, void* user, int priority);
void fault_unregister(int id);
// The reporter of last resort (one slot; null clears it). See point 4.
void fault_set_unhandled(FaultHandler h, void* user);
void fault_thread_init();

// Raises a guest trap on this thread (point 6). Returns only when the chain and the unhandled handler declined it.
void raise_guest_trap(uint32_t code);
// Ends the process at once with this exit code: no destructors, no atexit, other threads stop where they are.
[[noreturn]] void terminate(uint32_t code);

// Safe access to memory that may be unmapped or protected. A fault inside is caught after the chain had its turn (point 3): a write to a
// page the write tracker armed still goes through. Never throws; usable from any thread that ran fault_thread_init() (or any thread on Windows).
namespace guard {
bool read(const void* src, void* dst, size_t n);        // copies n bytes; false when a byte could not be read (dst partly written)
bool write(void* dst, const void* src, size_t n);       // copies n bytes; false when a byte could not be written (dst partly written)
// Runs fn(arg); false when it faulted (the stack of fn is abandoned as by longjmp: no destructors run, keep fn free of owning objects).
// catch_guest_trap: a raise_guest_trap inside fn also ends it (false) instead of going to the unhandled handler.
bool call(void (*fn)(void*), void* arg, bool catch_guest_trap = false);
}

}  // namespace nxrt::os
