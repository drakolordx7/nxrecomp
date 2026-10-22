// SPDX-License-Identifier: GPL-3.0-or-later
// Link check for the lifted game modules: implements the runtime-provided functions of nxrecomp/ctx.h as stubs and
// references each module descriptor, so the linker has to resolve every f_<addr> / u_<addr> / h_<name> the generated code
// calls. Not the runtime: it only proves that the modules are complete and free of duplicate definitions.
#include <nxrecomp/ctx.h>
#include <stdio.h>

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
extern const NxModule nxmod_main, nxmod_sdk, nxmod_subsdk0, nxmod_rtld;
}

int main() {
    const NxModule *m[] = {&nxmod_main, &nxmod_sdk, &nxmod_subsdk0, &nxmod_rtld};
    for (const NxModule *x : m)
        printf("%-8s index %u base 0x%llx functions %d dynsyms %d resume %d eh %s\n", x->name, x->index, (unsigned long long)x->base,
               x->nfns, x->nsyms, x->nresume, x->eh ? "yes" : "no");
    return 0;
}
