// SPDX-License-Identifier: GPL-3.0-or-later
// Link check for a game's lifted modules (cmake/NxRecomp.cmake nxrecomp_add_game: target <id>_link_check): links every module library with
// the runtime stubs (rt_stubs.cpp) and references each module descriptor, so the linker has to resolve every f_<addr> / u_<addr> / h_<name>
// the generated code calls. Not the runtime: it only proves that the modules are complete and free of duplicate definitions.
// link_check_modules.inc (generated from the game's manifest) lists NXGAME_MODULE(name) once per module.
#include <nxrecomp/ctx.h>
#include <stdio.h>

extern "C" {
#define NXGAME_MODULE(n) extern const NxModule nxmod_##n;
#include "link_check_modules.inc"
#undef NXGAME_MODULE
}

int main() {
    const NxModule *m[] = {
#define NXGAME_MODULE(n) &nxmod_##n,
#include "link_check_modules.inc"
#undef NXGAME_MODULE
    };
    for (const NxModule *x : m)
        printf("%-8s index %u base 0x%llx functions %d dynsyms %d resume %d eh %s\n", x->name, x->index, (unsigned long long)x->base,
               x->nfns, x->nsyms, x->nresume, x->eh ? "yes" : "no");
    return 0;
}
