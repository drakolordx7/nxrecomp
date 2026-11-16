// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt::os fault layer on Linux (include/nxrt/os/fault.h): one SIGSEGV/SIGBUS dispatcher (SA_SIGINFO | SA_ONSTACK | SA_NODEFER) that runs the
// chain, then the thread's guard (sigsetjmp/siglongjmp), then the unhandled handler, then the previous action / the default one (the retried
// instruction faults again and the kernel ends the process). Everything the dispatcher touches is atomics and thread-locals.
#include "../fault_chain.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace nxrt::os {
namespace {

constexpr size_t kAltStackSize = 256 * 1024;   // the crash reporter formats a report on it

struct Frame {
    sigjmp_buf jb;
    Frame* prev;
    bool catch_traps;
};

thread_local Frame* tl_guard = nullptr;
thread_local int tl_depth = 0;                 // > 0 while the dispatcher runs on this thread: a fault inside it is fatal
thread_local uint64_t tl_stack_lo = 0;         // lowest address of this thread's stack (0 unknown)

struct sigaction g_old_segv, g_old_bus;
std::atomic<bool> g_other_installed{false};

uint32_t current_tid() { return static_cast<uint32_t>(syscall(SYS_gettid)); }

// Owns the thread's alternate stack; the destructor runs at thread exit.
struct AltStack {
    void* mem = nullptr;
    bool done = false;
    ~AltStack() {
        if (!mem) return;
        stack_t off{};
        off.ss_flags = SS_DISABLE;
        sigaltstack(&off, nullptr);
        munmap(mem, kAltStackSize + 4096);
    }
};
thread_local AltStack tl_alt;

void restore_default(int sig) {
    struct sigaction dfl;
    std::memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, nullptr);
}

void on_signal(int sig, siginfo_t* si, void* uc_) {
    const int saved_errno = errno;
    if (tl_depth > 0) {                        // a fault inside the dispatcher or a handler: stop, the next fault kills the process
        restore_default(sig);
        errno = saved_errno;
        return;
    }
    ++tl_depth;
    ucontext_t* uc = static_cast<ucontext_t*>(uc_);
    FaultInfo fi;
    fi.kind = FaultKind::AccessViolation;
    fi.code = static_cast<uint32_t>(sig);
    fi.addr = reinterpret_cast<uint64_t>(si->si_addr);
    fi.tid = current_tid();
    fi.native = uc;
#if defined(__x86_64__)
    fi.is_write = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
    fi.is_exec = (uc->uc_mcontext.gregs[REG_ERR] & 0x10) != 0;
    fi.pc = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RIP]);
    fi.sp = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RSP]);
#endif
    if (tl_stack_lo && fi.addr + (256 * 1024) >= tl_stack_lo && fi.addr < tl_stack_lo + 64 * 1024) fi.kind = FaultKind::StackOverflow;

    if (detail::chain_run(fi)) {
        --tl_depth;
        errno = saved_errno;
        return;
    }
    if (Frame* g = tl_guard; g && fi.kind != FaultKind::StackOverflow) {
        --tl_depth;
        siglongjmp(g->jb, 1);
    }
    if (detail::unhandled_run(fi)) {
        --tl_depth;
        errno = saved_errno;
        return;
    }
    --tl_depth;
    const struct sigaction& old = sig == SIGBUS ? g_old_bus : g_old_segv;
    if ((old.sa_flags & SA_SIGINFO) && old.sa_sigaction) {
        old.sa_sigaction(sig, si, uc_);
    } else if (!(old.sa_flags & SA_SIGINFO) && old.sa_handler && old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN) {
        old.sa_handler(sig);
    } else {
        restore_default(sig);                  // returning retries the instruction, which now kills the process the normal way
    }
    errno = saved_errno;
}

// SIGILL, SIGFPE, SIGABRT: not faults of the chain; only the unhandled handler (the crash reporter) hears of them. After it returns the default
// action runs: returning re-executes the instruction (ILL, FPE) or lets abort() finish (ABRT), now with the default disposition.
void on_other_signal(int sig, siginfo_t* si, void* uc_) {
    const int saved_errno = errno;
    if (tl_depth == 0) {
        ++tl_depth;
        ucontext_t* uc = static_cast<ucontext_t*>(uc_);
        FaultInfo fi;
        fi.kind = FaultKind::Other;
        fi.code = static_cast<uint32_t>(sig);
        fi.addr = reinterpret_cast<uint64_t>(si->si_addr);
        fi.tid = current_tid();
        fi.native = uc;
#if defined(__x86_64__)
        fi.pc = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RIP]);
        fi.sp = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RSP]);
#endif
        detail::unhandled_run(fi);
        --tl_depth;
    }
    restore_default(sig);
    errno = saved_errno;
}

__attribute__((noinline)) bool run_guarded(void (*fn)(void*), void* arg, bool traps) {
    Frame f;
    f.prev = tl_guard;
    f.catch_traps = traps;
    if (sigsetjmp(f.jb, 0)) {                  // SA_NODEFER: the signal was never blocked, so there is no mask to restore
        tl_guard = f.prev;
        return false;
    }
    tl_guard = &f;
    fn(arg);
    tl_guard = f.prev;
    return true;
}

}  // namespace

namespace detail {

void platform_install() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_old_segv);
    sigaction(SIGBUS, &sa, &g_old_bus);
}

void platform_unhandled_set() {
    if (g_other_installed.exchange(true)) return;
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_other_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
}

}  // namespace detail

void fault_thread_init() {
    if (tl_alt.done) return;
    tl_alt.done = true;
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        void* addr = nullptr;
        size_t size = 0;
        if (pthread_attr_getstack(&attr, &addr, &size) == 0) tl_stack_lo = reinterpret_cast<uint64_t>(addr);
        pthread_attr_destroy(&attr);
    }
    stack_t cur{};
    if (sigaltstack(nullptr, &cur) == 0 && !(cur.ss_flags & SS_DISABLE) && cur.ss_sp) return;   // somebody gave this thread one already
    void* m = mmap(nullptr, kAltStackSize + 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return;
    mprotect(m, 4096, PROT_NONE);              // guard page below the stack
    stack_t ss{};
    ss.ss_sp = static_cast<char*>(m) + 4096;
    ss.ss_size = kAltStackSize;
    if (sigaltstack(&ss, nullptr) != 0) {
        munmap(m, kAltStackSize + 4096);
        return;
    }
    tl_alt.mem = m;
}

__attribute__((noinline)) void raise_guest_trap(uint32_t code) {
    FaultInfo fi;
    fi.kind = FaultKind::GuestTrap;
    fi.code = code;
    fi.tid = current_tid();
    fi.pc = reinterpret_cast<uint64_t>(__builtin_return_address(0));   // the code that raised it: the crash reporter finds it in the unwind
    if (detail::chain_run(fi)) return;                                  // the chain first, then a guard that catches traps (fault.h points 3 and 6; Windows: VEH before SEH)
    if (Frame* g = tl_guard; g && g->catch_traps) siglongjmp(g->jb, 1);
    detail::unhandled_run(fi);
}

[[noreturn]] void terminate(uint32_t code) { _exit(static_cast<int>(code)); }

namespace guard {
namespace {
struct CopyArgs { void* dst; const void* src; size_t n; };
void copy_cb(void* a) {
    CopyArgs* x = static_cast<CopyArgs*>(a);
    std::memcpy(x->dst, x->src, x->n);
}
}  // namespace
bool read(const void* src, void* dst, size_t n) {
    if (n == 0) return true;
    CopyArgs a{dst, src, n};
    return run_guarded(copy_cb, &a, false);
}
bool write(void* dst, const void* src, size_t n) {
    if (n == 0) return true;
    CopyArgs a{dst, src, n};
    return run_guarded(copy_cb, &a, false);
}
bool call(void (*fn)(void*), void* arg, bool catch_guest_trap) { return run_guarded(fn, arg, catch_guest_trap); }
}  // namespace guard

}  // namespace nxrt::os
