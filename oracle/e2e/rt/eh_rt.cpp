// SPDX-License-Identifier: GPL-3.0-or-later
#include "eh_rt.h"
void eh_rt_init(const NxModule *) {}
bool eh_host_call(Ctx *, uint64_t) { return false; }
extern "C" void nx_catch_call(Ctx *c, void (*body)(Ctx *, uint64_t)) { body(c, 0); }
