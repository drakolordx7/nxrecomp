// SPDX-License-Identifier: GPL-3.0-or-later
// Guest tracer: logs entry into functions and the registers before instructions, at addresses given as lists. It attaches to site
// hooks (hooks.h), so every address needs an entryhook / inline site in the game's site list (a re-lift).
//   [trace] fns = 387260,38a110     env NXRT_TRACE_FNS   log entry: x0..x3, lr, the vtable of [x0], the words of [x1]
//   [trace] at  = 2fe0f0            env NXRT_TRACE_AT    log x0..x3 and x19..x28 before the instruction (put it after a `bl` to see x0)
// Addresses are hex offsets from the main image (mem::kMainBase), comma separated; a value at or above kMainBase is taken as absolute.
// Log lines use the tag "trace". Read by nxrt::init; a game may add more lists with trace_fns / trace_at.
#pragma once
#include <cstdint>
#include <string>

#include "nxrt/abi.h"

namespace nxrt::trace {

void install_from_config();
void trace_fns(const std::string& list);
void trace_at(const std::string& list);

// Called after the generic entry lines of trace_fns with the hooked address; a game uses it to print what only it knows (its own string
// or object layouts). Null removes it. Set it before the first traced call.
using EntryDump = void (*)(::Ctx* c, uint64_t addr);
void set_entry_dump(EntryDump fn);

}
