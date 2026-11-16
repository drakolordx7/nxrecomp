// SPDX-License-Identifier: GPL-3.0-or-later
// Memory below 2^39 for tests that put host buffers into HIPC messages. A message describes a buffer with a 39-bit address (the Switch address
// space); Windows hands out heap and stack addresses below that, Linux (ASLR, PIE) does not. low_alloc() gives bytes from a fixed arena at
// kLowArena (outside the guest span of nxrt/memory.h), reset by low_reset(). Nothing here runs unless a test asks for it.
#pragma once

#include <cstdint>
#include <cstring>

#include "nxrt/os/mem.h"

namespace nxtest {

constexpr uint64_t kAddr39 = 1ull << 39;
constexpr uint64_t kLowArena = 0x6000000000ull;
constexpr uint64_t kLowArenaSize = 256ull << 20;

inline uint64_t& low_used() {
    static uint64_t used = 0;
    return used;
}

// Zeroed, 64-byte aligned bytes below 2^39; 0 when the arena is exhausted or cannot be mapped.
inline void* low_alloc(uint64_t size) {
    static bool ready = [] {
        return nxrt::os::mem::reserve_fixed(kLowArena, kLowArenaSize) && nxrt::os::mem::commit(kLowArena, kLowArenaSize, nxrt::os::mem::Prot::RW);
    }();
    if (!ready) return nullptr;
    const uint64_t at = (low_used() + 63) & ~63ull;
    if (size > kLowArenaSize || at > kLowArenaSize - size) return nullptr;
    low_used() = at + size;
    std::memset(reinterpret_cast<void*>(kLowArena + at), 0, size);
    return reinterpret_cast<void*>(kLowArena + at);
}

inline void low_reset() { low_used() = 0; }

inline bool is_low(const void* p) { return reinterpret_cast<uint64_t>(p) < kAddr39; }

}  // namespace nxtest
