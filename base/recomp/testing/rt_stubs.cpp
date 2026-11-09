// SPDX-License-Identifier: GPL-3.0-or-later
// Do-nothing definitions of every runtime-provided function of nxrecomp/ctx.h, for programs that link lifted modules without the runtime
// (the link check of a game's libraries, testing/link_check.cpp). When ctx.h gains a runtime-provided function, add it here once.
// The hash-locked oracles (oracle/difftest/host.cpp, oracle/e2e/rt/x64_runner.cpp, oracle/ka/ka_host.cpp) keep their own working
// versions: they run the code, this only satisfies the linker.
#include <nxrecomp/ctx.h>

extern "C" {
void call_indirect(Ctx *, uint64_t) {}
void host_import(Ctx *, uint32_t) {}
void guest_trap(const char *, uint64_t) {}
void guest_unimpl(uint64_t, uint32_t, const char *) {}
void guest_svc(Ctx *, uint32_t) {}
uint64_t guest_mrs(Ctx *, uint32_t) { return 0; }
void guest_msr(Ctx *, uint32_t, uint64_t) {}
void nx_site_inline(Ctx *, uint64_t) {}
int nx_site_midhook(Ctx *, uint64_t) { return 0; }
int nx_hook_entry(Ctx *, uint64_t) { return 0; }
void stx_diag(Ctx *, uint64_t, uint64_t, uint64_t, uint64_t, int) {}
void nx_catch_call(Ctx *c, void (*body)(Ctx *, uint64_t)) { body(c, 0); }
}
