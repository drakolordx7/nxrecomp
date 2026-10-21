// SPDX-License-Identifier: GPL-3.0-or-later
// The generated-code ABI as the runtime sees it. S1 owns the header (recomp/include/nxrecomp/ctx.h: Ctx, V128, FnEntry,
// DynSym, NxModule and the declarations of the functions this runtime provides: call_indirect, host_import, guest_trap,
// guest_unimpl, guest_svc, guest_mrs, guest_msr, stx_diag, nx_catch_call; their definitions are src/core/abi_glue.cpp).
// Everything else in nxrt reaches the guest CPU state through the small accessors below.
#pragma once

#include <nxrecomp/ctx.h>

static_assert(NXRECOMP_ABI_VERSION == 1, "nxrt was written against nxrecomp ABI version 1: review src/core/abi_glue.cpp and diag/ before bumping");

namespace nxrt {

using GuestFn = void (*)(::Ctx*);   // a lifted function (or an HLE handler): guest registers in, guest registers out
using HleFn = GuestFn;

// Register access. x[31] is SP; w-register views are the caller's business.
inline uint64_t& reg(::Ctx* c, int n) { return c->x[n]; }
inline uint64_t& sp(::Ctx* c) { return c->x[31]; }
inline uint64_t& lr(::Ctx* c) { return c->x[30]; }
inline uint64_t& fp(::Ctx* c) { return c->x[29]; }
inline uint64_t& tpidr_el0(::Ctx* c) { return c->tpidr; }
inline uint64_t& tpidrro_el0(::Ctx* c) { return c->tpidrro; }

}  // namespace nxrt
