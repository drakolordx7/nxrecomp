// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/abi.h"
namespace nxrt {
const char* abi_source() { return "recomp/include/nxrecomp/ctx.h, ABI version " "1"; }
int abi_version() { return NXRECOMP_ABI_VERSION; }
}  // namespace nxrt
