// SPDX-License-Identifier: GPL-3.0-or-later
// Internal to src/os/win32: the host stack walk shared by symbols.cpp and thread.cpp.
#pragma once
#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace nxrt::os::detail {

// Walks the host stack described by *ctx with RtlLookupFunctionEntry/RtlVirtualUnwind. Writes up to `max` return addresses (the first is
// ctx->Rip). Does not modify *ctx. Never allocates.
size_t capture_context(const CONTEXT* ctx, uint64_t* frames, size_t max);

}  // namespace nxrt::os::detail
