// SPDX-License-Identifier: GPL-3.0-or-later
// The generated-code ABI as the runtime sees it. S1 owns the real header (recomp/include/nxrecomp/); this file
// includes it when it exists and otherwise falls back to a clearly marked TEMPORARY shim (nxrt/shim/runtime_shim.h).
// Everything else in nxrt reaches the guest CPU state through the small accessors below, so a field rename in the real
// header is a one-file fix.
#pragma once

#include <stdint.h>

#if __has_include(<nxrecomp/runtime.h>)
#include <nxrecomp/runtime.h>
#define NXRT_ABI_SOURCE "recomp/include/nxrecomp/runtime.h"
#elif __has_include(<nxrecomp/abi.h>)
#include <nxrecomp/abi.h>
#define NXRT_ABI_SOURCE "recomp/include/nxrecomp/abi.h"
#elif __has_include(<nxrecomp/ctx.h>)
#include <nxrecomp/ctx.h>
#define NXRT_ABI_SOURCE "recomp/include/nxrecomp/ctx.h"
#else
#include "nxrt/shim/runtime_shim.h"
#define NXRT_ABI_SOURCE "TEMPORARY SHIM nxrt/shim/runtime_shim.h (smo-recomp runtime.h layout)"
#endif

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
