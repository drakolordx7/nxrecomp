// SPDX-License-Identifier: GPL-3.0-or-later
// Pages at fixed addresses for tests that stand in for guest memory (a game's globals at kBase + offset): the portable replacement for the
// VirtualAlloc(addr, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) those tests used.
#pragma once
#include <cstdint>

#include "nxrt/os/mem.h"

namespace nxtest {

// Makes [addr, addr + size) readable and writable at exactly that address (reading zeros), reserving the 64 KiB granules around it first. A
// granule an earlier call already reserved is fine. True when the pages are usable afterwards.
inline bool commit_pages(uint64_t addr, uint64_t size) {
    namespace osmem = nxrt::os::mem;
    const uint64_t g = osmem::kGranularity, pg = osmem::kPage;
    const uint64_t lo = addr & ~(g - 1), hi = (addr + size + g - 1) & ~(g - 1);
    osmem::reserve_fixed(lo, hi - lo);
    const uint64_t p0 = addr & ~(pg - 1), p1 = (addr + size + pg - 1) & ~(pg - 1);
    return osmem::commit(p0, p1 - p0, osmem::Prot::RW);
}

}  // namespace nxtest
