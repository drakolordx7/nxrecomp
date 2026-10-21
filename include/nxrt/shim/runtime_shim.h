// SPDX-License-Identifier: GPL-3.0-or-later
// TEMPORARY SHIM. Used only while recomp/include/nxrecomp/ has no ABI header yet (S1 owns it).
// Layout copied from smo-recomp runtime/core/runtime.h (GPL-3.0), reduced to what the runtime skeleton touches, plus the
// separate TPIDRRO_EL0 the maintainer asked for. Delete this file when the real header exists; abi.h switches by itself.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union {
    uint8_t b[16];
    uint16_t h[8];
    uint32_t w[4];
    uint64_t d[2];
    float f[4];
    double df[2];
} V128;

typedef struct Ctx {
    uint64_t x[32];          /* x[31] is SP */
    V128 v[32];
    uint32_t N, Z, C, V;
    uint64_t tpidr;          /* TPIDR_EL0 */
    uint64_t tpidrro;        /* TPIDRRO_EL0 (TLS region pointer) */
    uint64_t ex_addr, ex_val;
    uint64_t ex_fail;
    uint64_t fn;             /* entry address of the lifted function entered most recently */
    const char* in_import;
    uint64_t import_from;
} Ctx;

typedef struct { uint64_t addr; void (*fn)(Ctx*); } FnEntry;

void call_indirect(Ctx* c, uint64_t addr);
void guest_trap(const char* what, uint64_t addr);
void guest_svc(Ctx* c, uint32_t number);

#ifdef __cplusplus
}
#endif
