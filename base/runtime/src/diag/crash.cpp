// SPDX-License-Identifier: GPL-3.0-or-later
// Crash handler (declared in nxrt/diag.h): the reporter of last resort of the fault layer (os::fault_set_unhandled) that writes the "HOST
// EXCEPTION" report. It sees access violations that nothing repaired or guarded, stack overflows, guest traps and the other exceptions the
// host delivers (Windows: any; Linux: SIGILL, SIGFPE, SIGABRT). A stack overflow is reported from a helper thread: the faulting thread has
// no stack to spare. Where the host has no machine context (a guest trap on Linux) the stack of the calling thread stands in. The layout of
// the report follows crash_build_report() in diag.h and is the same on both systems; the Linux header maps the signal to the Windows
// exception code and adds a "signal" line. Windows minidumps are not written (the OS reporting handles that); on Linux the OS writes a core
// file when the report is followed by the default action (CrashOptions::hand_to_system).
// Idea from smo-recomp runtime/core/hostrt.c crash_filter; written here from scratch.
#include "nxrt/diag.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/log.h"
#include "nxrt/os/fault.h"
#include "nxrt/os/fs.h"
#include "nxrt/os/mem.h"
#include "nxrt/os/process.h"
#include "nxrt/os/symbols.h"
#include "nxrt/os/thread.h"
#include "nxrt/symbols.h"

namespace nxrt {
namespace {

constexpr uint32_t kTrapCode = os::kTrapUnresolved;
constexpr uint32_t kUntranslatedCode = os::kTrapUntranslated;
constexpr uint32_t kGuestBreakCode = os::kTrapBreak;   // svcBreak / svcReturnFromException (svc_thread.cpp)
constexpr uint32_t kAccessViolationCode = 0xC0000005u;
constexpr uint32_t kStackOverflowCode = 0xC00000FDu;
constexpr size_t kMaxHostFrames = 32;
constexpr int kMaxGuestFrames = 24;
constexpr uint32_t kOverflowWaitMs = 30000;

std::mutex g_install_mu;
bool g_installed = false;

bool g_path_given = false;  // false: default "<exe>_crash.txt" next to the exe; true with "" : no file
std::string g_path;
bool g_terminate = true;
bool g_hand_to_system = false;
uint32_t g_exit_code = 4;
void (*g_on_report)(const char*, void*) = nullptr;
void* g_user = nullptr;

std::atomic<bool> g_in_handler{false};
std::atomic<uint32_t> g_reports{0};

// Stack overflow: the report is built by a helper thread from what the faulting thread captured before waiting for it.
os::FaultInfo g_ov_fi{};
uint64_t g_ov_frames[kMaxHostFrames] = {};
size_t g_ov_n = 0;

#if defined(__clang__) || defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void appendf(std::string& out, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    if (n >= 0 && static_cast<size_t>(n) < sizeof buf) {
        out.append(buf, static_cast<size_t>(n));
    } else if (n > 0) {
        std::string big(static_cast<size_t>(n) + 1, '\0');
        std::vsnprintf(big.data(), big.size(), fmt, ap2);
        out.append(big.data(), static_cast<size_t>(n));
    }
    va_end(ap);
    va_end(ap2);
}

// True when every byte of [addr, addr+n) is committed and readable. Never faults.
bool readable(uint64_t addr, uint64_t n) { return n == 0 || os::mem::is_accessible(addr, n); }

std::string default_report_path() {
    std::string p = os::process::exe_path();
    size_t slash = p.find_last_of("\\/");
    size_t dot = p.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) p.erase(dot);
    return p + "_crash.txt";
}

bool write_file(const std::string& path, const std::string& text) {
    std::FILE* f = os::fs::fopen(path, "wb");
    if (!f) return false;
    const size_t done = std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    return done == text.size();
}

// The Windows exception code the header shows. Linux signals are mapped to the nearest one; the native number is on the "signal" line.
uint32_t header_code(const os::FaultInfo& fi, const char** signal_name) {
    *signal_name = nullptr;
    const bool is_signal = fi.code != 0 && fi.code < 0x100;   // a Linux signal number
    if (is_signal) {
        switch (fi.code) {
            case 4: *signal_name = "SIGILL"; break;
            case 6: *signal_name = "SIGABRT"; break;
            case 7: *signal_name = "SIGBUS"; break;
            case 8: *signal_name = "SIGFPE"; break;
            case 11: *signal_name = "SIGSEGV"; break;
            default: *signal_name = "signal"; break;
        }
    }
    switch (fi.kind) {
        case os::FaultKind::AccessViolation: return kAccessViolationCode;
        case os::FaultKind::StackOverflow: return kStackOverflowCode;
        case os::FaultKind::GuestTrap: return fi.code;
        case os::FaultKind::Other:
            if (!is_signal) return fi.code;
            return fi.code == 4 ? 0xC000001Du : fi.code == 8 ? 0xC0000094u : fi.code == 6 ? 0x40000015u : 0xC0000005u;
    }
    return fi.code;
}

std::string build_report(const os::FaultInfo& fi, const uint64_t* frames, size_t nframes) {
    g_reports.fetch_add(1, std::memory_order_relaxed);
    std::string out;
    const uint64_t rip = fi.pc;
    const uint32_t tid = fi.tid;
    const std::vector<ThreadRecord> threads = diag_threads();

    std::string tname = "unregistered";
    bool named = false;
    ::Ctx* gctx = nullptr;
    for (const auto& t : threads) {
        if (t.host_tid != tid) continue;
        if (!named) {
            tname = t.name.empty() ? std::string("(unnamed)") : t.name;
            named = true;
        }
        if (!gctx && t.ctx) gctx = t.ctx;
    }
    if (tid == os::thread_current_id()) gctx = diag_thread_ctx();
    if (gctx && !readable(reinterpret_cast<uint64_t>(gctx), sizeof(::Ctx))) gctx = nullptr;

    const char* signal_name = nullptr;
    const uint32_t code = header_code(fi, &signal_name);
    const char* code_name = nullptr;
    if (fi.kind == os::FaultKind::GuestTrap) {
        if (code == kTrapCode) code_name = "GUEST TRAP";
        else if (code == kUntranslatedCode) code_name = "UNTRANSLATED INSTRUCTION";
        else if (code == kGuestBreakCode) code_name = "GUEST BREAK (svcBreak)";
    }
    if (code_name) {
        appendf(out, "HOST EXCEPTION 0x%08lX (%s) at host address 0x%llx on thread %s (tid %lu)\n", static_cast<unsigned long>(code), code_name,
                static_cast<unsigned long long>(rip), tname.c_str(), static_cast<unsigned long>(tid));
    } else {
        appendf(out, "HOST EXCEPTION 0x%08lX at host address 0x%llx on thread %s (tid %lu)\n", static_cast<unsigned long>(code),
                static_cast<unsigned long long>(rip), tname.c_str(), static_cast<unsigned long>(tid));
    }
    appendf(out, "  in %s\n", symbols_describe(rip).c_str());
    if (signal_name) appendf(out, "  %s (signal %u)\n", signal_name, static_cast<unsigned>(fi.code));
    if (fi.kind == os::FaultKind::AccessViolation) {
        const char* what = fi.is_exec ? "executing" : fi.is_write ? "writing" : "reading";
        appendf(out, "  %s address 0x%llx\n", what, static_cast<unsigned long long>(fi.addr));
    }

    if (const uint64_t g = guest_function_of(rip))
        appendf(out, "last guest function entered: f_%llx\n", static_cast<unsigned long long>(g));
    else
        out += "last guest function entered: (unknown: fault address is not inside lifted code)\n";

    if (!gctx) {
        out += "guest registers: (no guest context on this thread)\n";
    } else {
        out += "guest registers:\n";
        for (int n = 0; n <= 30; ++n) {
            out += (n % 3 == 0) ? (n == 0 ? "  " : "\n  ") : "   ";
            appendf(out, "x%-2d %016llx", n, static_cast<unsigned long long>(reg(gctx, n)));
        }
        out += "\n";
        appendf(out, "  sp %016llx   tpidr %016llx   tpidrro %016llx\n", static_cast<unsigned long long>(sp(gctx)),
                static_cast<unsigned long long>(tpidr_el0(gctx)), static_cast<unsigned long long>(tpidrro_el0(gctx)));
    }

    if (!gctx) {
        out += "guest frame chain (x29): (none)\n";
    } else {
        appendf(out, "guest frame chain (x29): lr=0x%llx", static_cast<unsigned long long>(lr(gctx)));
        uint64_t f = fp(gctx);
        for (int i = 0; i < kMaxGuestFrames && f && readable(f, 16); ++i) {
            const uint64_t next = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(f));
            const uint64_t ret = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(f + 8));
            if (ret) appendf(out, " <- 0x%llx", static_cast<unsigned long long>(ret));
            if (next <= f) break;
            f = next;
        }
        out += "\n";
    }

    out += "host call stack (innermost first):\n";
    if (!nframes) out += "  (none)\n";
    for (size_t i = 0; i < nframes; ++i) appendf(out, "  %s\n", symbols_describe(frames[i]).c_str());

    out += "guest call stack (innermost first):";
    uint64_t last = 0;
    bool any = false;
    for (size_t i = 0; i < nframes; ++i) {
        const uint64_t g = guest_function_of(frames[i]);
        if (!g || g == last) continue;
        appendf(out, " f_%llx", static_cast<unsigned long long>(g));
        last = g;
        any = true;
    }
    if (!any) out += " (none)";
    out += "\n";

    out += "threads:\n";
    if (threads.empty()) out += "  (none)\n";
    for (const auto& t : threads) {
        char lrbuf[32] = "-";
        if (t.ctx && readable(reinterpret_cast<uint64_t>(t.ctx), sizeof(::Ctx)))
            std::snprintf(lrbuf, sizeof lrbuf, "0x%llx", static_cast<unsigned long long>(lr(t.ctx)));
        appendf(out, "  %s tid=%lu guest_tid=%llu lr=%s\n", t.name.empty() ? "(unnamed)" : t.name.c_str(),
                static_cast<unsigned long>(t.host_tid), static_cast<unsigned long long>(t.guest_tid), lrbuf);
    }

    const std::vector<std::string> files = diag_recent_files();
    out += "last files opened (oldest first):\n";
    if (files.empty()) out += "  (none)\n";
    for (const auto& f : files) appendf(out, "  %s\n", f.c_str());
    return out;
}

// Log block, report file, stderr, then the on_report callback. Terminating is the caller's decision.
void emit(const std::string& text) {
    for (size_t pos = 0; pos < text.size();) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        NXLOG_E("crash", "%.*s", static_cast<int>(eol - pos), text.data() + pos);
        pos = eol + 1;
    }
    log_flush();
    const std::string path = g_path_given ? g_path : default_report_path();
    if (!path.empty()) write_file(path, text);
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fflush(stderr);
    if (g_on_report) g_on_report(text.c_str(), g_user);
}

[[noreturn]] void second_exception() {
    std::fputs("nxrt: second exception inside the crash handler, terminating\n", stderr);
    std::fflush(stderr);
    os::terminate(g_exit_code);
}

void overflow_thread(void*) {
    emit(build_report(g_ov_fi, g_ov_frames, g_ov_n));
    if (g_terminate) os::terminate(g_exit_code);
}

os::FaultResult on_fault(const os::FaultInfo& fi, void*) {
    if (g_in_handler.exchange(true)) second_exception();
    uint64_t frames[kMaxHostFrames] = {};
    const size_t n = os::symbols::capture_fault(fi, frames, kMaxHostFrames);   // on the faulting thread: it is the one whose stack this is
    if (fi.kind == os::FaultKind::StackOverflow) {
        g_ov_fi = fi;
        g_ov_fi.native = nullptr;
        g_ov_n = n;
        std::copy(frames, frames + n, g_ov_frames);
        os::ThreadOptions to;
        to.name = "crash-report";
        to.stack_bytes = 1 << 20;
        os::ThreadHandle h = nullptr;
        os::ThreadId id = 0;
        if (os::thread_start(to, overflow_thread, nullptr, &h, &id)) {
            os::thread_join(h, kOverflowWaitMs);
            os::thread_close(h);
        }
    } else {
        emit(build_report(fi, frames, n));
        if (g_terminate && !g_hand_to_system) os::terminate(g_exit_code);
    }
    g_in_handler = false;
    return os::FaultResult::Continue;
}

}

void crash_install(const CrashOptions& opt) {
    std::lock_guard<std::mutex> lk(g_install_mu);
    g_path_given = opt.report_path != nullptr;
    g_path = opt.report_path ? opt.report_path : "";
    g_terminate = opt.terminate;
    g_hand_to_system = opt.hand_to_system || config_get_bool("diag", "crash_core", false);
    g_exit_code = opt.exit_code;
    g_on_report = opt.on_report;
    g_user = opt.user;
    if (g_installed) return;
    os::fault_thread_init();             // Windows: reserves stack for the handler of an overflowing thread; Linux: the alternate stack
    os::symbols::prepare();              // loads the symbol table now, not inside a fault handler
    os::fault_set_unhandled(on_fault, nullptr);
    g_installed = true;
}

void crash_uninstall() {
    std::lock_guard<std::mutex> lk(g_install_mu);
    if (!g_installed) return;
    os::fault_set_unhandled(nullptr, nullptr);
    g_installed = false;
}

bool crash_installed() {
    std::lock_guard<std::mutex> lk(g_install_mu);
    return g_installed;
}

std::string crash_build_report(const os::FaultInfo& fi) {
    uint64_t frames[kMaxHostFrames] = {};
    const size_t n = os::symbols::capture_fault(fi, frames, kMaxHostFrames);
    return build_report(fi, frames, n);
}

uint32_t crash_exceptions_reported() { return g_reports.load(std::memory_order_relaxed); }

}
