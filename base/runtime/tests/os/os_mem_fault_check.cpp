// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone check of nxrt::os::mem and nxrt::os::fault (not part of the CMake test targets: no framework, no runtime). Build and run by hand:
//   clang++ -std=c++20 -O1 -Iinclude tests/os/os_mem_fault_check.cpp src/os/fault_chain.cpp src/os/posix/mem.cpp src/os/posix/fault.cpp -o check -pthread
//   ./check
// Windows (clang-cl): the same sources with src/os/win32/*.cpp.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

#include "nxrt/os/fault.h"
#include "nxrt/os/mem.h"

#ifndef _WIN32
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace om = nxrt::os::mem;
using nxrt::os::FaultInfo;
using nxrt::os::FaultResult;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static std::atomic<int> g_handled{0};

// The write-tracker's job in miniature: a read-only page becomes writable on the first write.
static FaultResult repair(const FaultInfo& fi, void*) {
    if (fi.kind != nxrt::os::FaultKind::AccessViolation || !fi.is_write) return FaultResult::Continue;
    if (om::protect_if(fi.addr & ~(om::kPage - 1), om::kPage, om::Prot::R, om::Prot::RW) != om::ProtectResult::Ok) return FaultResult::Continue;
    g_handled.fetch_add(1);
    return FaultResult::Handled;
}

static void touch_null(void*) { *reinterpret_cast<volatile int*>(8) = 1; }
static void trap_it(void*) { nxrt::os::raise_guest_trap(nxrt::os::kTrapUnresolved); }

static int g_kind = -1;
static FaultResult unhandled_probe(const FaultInfo& fi, void*) {
    g_kind = static_cast<int>(fi.kind);
    nxrt::os::terminate(fi.kind == nxrt::os::FaultKind::StackOverflow ? 42 : 43);
}
static volatile int g_sink;
static int recurse(int n) {
    volatile char pad[4096];
    pad[0] = static_cast<char>(n);
    g_sink = pad[0];
    return recurse(n + 1) + pad[1];
}

int main() {
    const uint64_t base = 0x7100000000ull, heap = 0x7800000000ull, alias = 0x7A00000000ull;
    nxrt::os::fault_thread_init();

    // ---- reserve / commit / query ----
    CHECK(om::reserve_fixed(base, 0x100000));
    CHECK(!om::reserve_fixed(base + 0x10000, 0x10000));              // taken: refused, nothing replaced
    om::Region r;
    CHECK(om::query(base, &r) && r.state == om::State::Reserved && r.size == 0x100000);
    CHECK(!om::is_accessible(base, 1));
    CHECK(om::commit(base, 0x4000, om::Prot::RW));
    CHECK(om::is_accessible(base, 0x4000) && !om::is_accessible(base, 0x5000));
    *reinterpret_cast<volatile uint32_t*>(base + 8) = 0x1234;
    CHECK(om::protect(base + 0x1000, 0x1000, om::Prot::R));
    CHECK(om::query(base + 0x1000, &r) && r.state == om::State::Committed && r.prot == om::Prot::R && r.size == 0x1000);
    CHECK(!om::protect(base + 0x8000, 0x1000, om::Prot::R));          // not committed
    CHECK(om::decommit(base + 0x3000, 0x1000));
    CHECK(!om::is_accessible(base + 0x3000, 1));

    // ---- fault chain + guard ----
    int id = nxrt::os::fault_register(repair, nullptr, nxrt::os::kPriorityFirst);
    CHECK(id > 0);
    *reinterpret_cast<volatile uint32_t*>(base + 0x1000 + 4) = 7;     // read-only page: the handler repairs it and the store goes on
    CHECK(g_handled.load() == 1);
    CHECK(om::query(base + 0x1000, &r) && r.prot == om::Prot::RW);
    CHECK(*reinterpret_cast<volatile uint32_t*>(base + 0x1000 + 4) == 7);
    uint32_t v = 0;
    CHECK(nxrt::os::guard::read(reinterpret_cast<void*>(base + 8), &v, 4) && v == 0x1234);
    CHECK(!nxrt::os::guard::read(reinterpret_cast<void*>(base + 0x3000), &v, 4));        // decommitted
    CHECK(!nxrt::os::guard::read(reinterpret_cast<void*>(base + 0x50000), &v, 4));       // reserved
    CHECK(!nxrt::os::guard::write(reinterpret_cast<void*>(base + 0x3000), &v, 4));
    CHECK(om::protect(base + 0x2000, 0x1000, om::Prot::R));
    CHECK(nxrt::os::guard::write(reinterpret_cast<void*>(base + 0x2000), &v, 4));        // tracker-style repair happens before the guard
    CHECK(g_handled.load() == 2);
    CHECK(!nxrt::os::guard::call(touch_null, nullptr));
    CHECK(!nxrt::os::guard::call(trap_it, nullptr, true));                                // a guest trap inside a guard that catches them
    CHECK(nxrt::os::guard::read(reinterpret_cast<void*>(base + 8), &v, 4));              // still usable after catching faults
    std::thread([&] {
        nxrt::os::fault_thread_init();
        uint32_t w = 0;
        CHECK(!nxrt::os::guard::read(reinterpret_cast<void*>(8), &w, 4));
        CHECK(nxrt::os::guard::read(reinterpret_cast<void*>(base + 8), &w, 4));
    }).join();
    nxrt::os::fault_unregister(id);

    // ---- sections: primary view, alias, decommit-by-protect ----
    om::Section s;
    CHECK(om::section_create(0x400000, &s));
    CHECK(om::section_map(s, 0, 0x400000, heap, om::Prot::None) == heap);
    CHECK(om::query(heap, &r) && r.state == om::State::Reserved);
    CHECK(om::commit(heap, 0x200000, om::Prot::RW));
    CHECK(om::section_map(s, 0x10000, 0x10000, alias, om::Prot::RW) == alias);
    CHECK(om::section_map(s, 0, 0x10000, alias, om::Prot::RW) == 0);                       // range taken
    *reinterpret_cast<volatile uint64_t*>(heap + 0x10010) = 0xDEADBEEFCAFEF00Dull;
    CHECK(*reinterpret_cast<volatile uint64_t*>(alias + 0x10) == 0xDEADBEEFCAFEF00Dull);   // one page, two addresses
    CHECK(om::protect(heap + 0x10000, 0x1000, om::Prot::None, false));
    CHECK(!om::is_accessible(heap + 0x10000, 1));
    CHECK(*reinterpret_cast<volatile uint64_t*>(alias + 0x10) == 0xDEADBEEFCAFEF00Dull);  // contents survive: the alias still sees them
    CHECK(om::section_unmap(alias, 0x10000));
    CHECK(om::query(alias, &r) && r.state == om::State::Free);
    CHECK(om::section_unmap(heap, 0x400000));
    om::section_close(&s);
    CHECK(om::release(base, 0x100000));
    CHECK(om::query(base, &r) && r.state == om::State::Free);

    // ---- the rest ----
    void* p = om::alloc_aligned(100, 4096);
    CHECK(p && (reinterpret_cast<uintptr_t>(p) & 4095) == 0);
    om::free_aligned(p);
    void* q = om::alloc(1 << 20);
    CHECK(q != nullptr);
    std::memset(q, 1, 1 << 20);
    CHECK(om::footprint() >= (1u << 20));
    CHECK(om::query(reinterpret_cast<uint64_t>(q), &r) && r.state == om::State::Committed);        // outside the tracked window
    CHECK(om::is_accessible(reinterpret_cast<uint64_t>(q), 1 << 20));
    CHECK(om::free(q, 1 << 20));
    std::printf("max_mappings=%llu footprint=%llu\n", static_cast<unsigned long long>(om::max_mappings()), static_cast<unsigned long long>(om::footprint()));

#ifndef _WIN32
    // ---- stack overflow reaches the unhandled handler on the alternate stack ----
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        nxrt::os::fault_set_unhandled(unhandled_probe, nullptr);
        nxrt::os::fault_thread_init();
        std::thread([] {
            nxrt::os::fault_thread_init();
            recurse(0);
        }).join();
        _exit(1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    std::printf("stack overflow child: exit=%d (42 = StackOverflow kind, 43 = plain access violation)\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 42);
    pid = fork();                                                                          // unhandled plain fault
    if (pid == 0) {
        nxrt::os::fault_set_unhandled(unhandled_probe, nullptr);
        touch_null(nullptr);
        _exit(1);
    }
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 43);
    pid = fork();                                                                          // nobody handles it: the default action kills the process
    if (pid == 0) {
        nxrt::os::fault_register(repair, nullptr, 0);
        touch_null(nullptr);
        _exit(1);
    }
    waitpid(pid, &st, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV);
#endif
    std::printf(g_fail ? "FAILED: %d\n" : "ALL PASS\n", g_fail);
    return g_fail != 0;
}
