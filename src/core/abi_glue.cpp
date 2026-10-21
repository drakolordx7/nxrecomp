// SPDX-License-Identifier: GPL-3.0-or-later
// The runtime-provided half of the generated-code ABI (recomp/include/nxrecomp/ctx.h, section "runtime-provided"):
// call_indirect, host_import, guest_trap, guest_unimpl, guest_svc, guest_mrs, guest_msr, stx_diag, nx_catch_call,
// plus module registration of the lifter's NxModule tables.
// Pattern from smo-recomp runtime/core/hostrt.c (host_import with in_import bookkeeping, stx_diag, guest_trap / guest_unimpl).
#include <windows.h>

#include <atomic>
#include <mutex>
#include <set>
#include <vector>

#include "nxrt/dispatch.h"
#include "nxrt/log.h"
#include "nxrt/module.h"
#include "nxrt/svc.h"

namespace nxrt {

// (not in an anonymous namespace: the extern "C" functions below share them)
std::mutex g_nx_lock;
std::vector<const NxModule*> g_nx;
std::set<uint32_t> g_sysregs_seen;

size_t module_register_nxmodule(Module* m, const NxModule* nx) {
    size_t n = 0;
    if (m) {
        n = module_register_functions(m, nx->fns, static_cast<size_t>(nx->nfns));
    } else {
        for (int i = 0; i < nx->nfns; i++) {
            if (nx->fns[i].addr - nx->base >= nx->text_end - nx->base || !nx->fns[i].fn) continue;
            fn_register(nx->fns[i].addr, nx->fns[i].fn);
            n++;
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_nx_lock);
        g_nx.push_back(nx);
    }
    NXLOG_I("loader", "registered lifted module %s: %zu of %d functions, %d dynamic symbols", nx->name, n, nx->nfns, nx->nsyms);
    return n;
}

}  // namespace nxrt

extern "C" {

void call_indirect(Ctx* c, uint64_t addr) { nxrt::dispatch_call(c, addr); }

// A call to a symbol another module provides: the lifted PLT stub passes its module's DynSym index. The stub does not say which module
// it belongs to, so the module is found through c->fn (the stub's entry address, when the lifted prologue sets it) and otherwise the
// first registered module with that many symbols is used. OPEN QUESTION for S1: sym should be unique across modules.
void host_import(Ctx* c, int sym) {
    const NxModule* mod = nullptr;
    {
        std::lock_guard<std::mutex> lk(nxrt::g_nx_lock);
        for (const NxModule* nx : nxrt::g_nx)
            if (c->fn - nx->base < nx->text_end - nx->base) { mod = nx; break; }
        if (!mod)
            for (const NxModule* nx : nxrt::g_nx)
                if (sym >= 0 && sym < nx->nsyms) { mod = nx; break; }
    }
    if (!mod || sym < 0 || sym >= mod->nsyms) {
        nxrt::trap("host_import with an unknown module or symbol index", static_cast<uint64_t>(sym));
    }
    const char* name = mod->syms[sym].name;
    const char* saved_name = c->in_import;
    const uint64_t saved_from = c->import_from;
    c->in_import = name;
    c->import_from = c->x[30] - 4;
    nxrt::dispatch_call(c, nxrt::hle_address(name));     // a handler, or a logging stub that returns 0
    c->in_import = saved_name;
    c->import_from = saved_from;
}

void guest_trap(const char* what, uint64_t addr) { nxrt::trap(what, addr); }

void guest_unimpl(uint64_t pc, uint32_t insn, const char* what) {
    NXLOG_E("guest", "UNTRANSLATED INSTRUCTION at 0x%llx: %08x (%s)", static_cast<unsigned long long>(pc), insn, what);
    nxrt::log_flush();
    RaiseException(0xE0000002, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
}

void guest_svc(Ctx* c, uint32_t number) { nxrt::svc_dispatch(c, number, 0); }

// System registers the lifter does not model inline: reads give 0, writes are ignored; each register is reported once.
uint64_t guest_mrs(Ctx* c, uint32_t sysreg) {
    std::lock_guard<std::mutex> lk(nxrt::g_nx_lock);
    if (nxrt::g_sysregs_seen.insert(sysreg).second) NXLOG_W("guest", "MRS of system register 0x%04x (not modelled): returns 0 (in f_%llx)", sysreg, static_cast<unsigned long long>(c->fn));
    return 0;
}

void guest_msr(Ctx* c, uint32_t sysreg, uint64_t value) {
    std::lock_guard<std::mutex> lk(nxrt::g_nx_lock);
    if (nxrt::g_sysregs_seen.insert(sysreg | 0x10000u).second) NXLOG_W("guest", "MSR to system register 0x%04x = 0x%llx (not modelled): ignored (in f_%llx)", sysreg, static_cast<unsigned long long>(value), static_cast<unsigned long long>(c->fn));
}

// Called on every failed exclusive store; reports a loop that never succeeds.
void stx_diag(Ctx* c, uint64_t a, uint64_t expected, uint64_t seen, uint64_t v, int bits) {
    uint64_t n = ++c->ex_fail;
    if (n == 1000 || n == 100000)
        NXLOG_W("guest", "STXR failing %llu times in f_%llx: addr=%llx expected=%llx seen=%llx new=%llx (%s%d)", static_cast<unsigned long long>(n),
                static_cast<unsigned long long>(c->fn), static_cast<unsigned long long>(a), static_cast<unsigned long long>(expected),
                static_cast<unsigned long long>(seen), static_cast<unsigned long long>(v), bits < 0 ? "monitor lost, " : "", bits < 0 ? -bits : bits);
}

// STUB (see docs/S3.md "Gaps"): C++ exception catch frames. The lifter (S1) enters a function that has a catch clause as
// nx_catch_call(c, u_<entry>); the runtime has to run body(c, 0) under a host frame it can unwind to, and on a guest exception that
// the function catches call body(c, landing pad) there instead (ssbu-recomp's nro_catch_call). Needs the module's NxEh tables and an
// _Unwind_RaiseException implementation. Until then the body simply runs without a catch frame.
void nx_catch_call(Ctx* c, void (*body)(Ctx*, uint64_t)) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) NXLOG_W("guest", "nx_catch_call: C++ exception support is a stub; catch clauses will not catch");
    body(c, 0);
}

}  // extern "C"
