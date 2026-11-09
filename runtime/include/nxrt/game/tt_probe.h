// SPDX-License-Identifier: GPL-3.0-or-later
// What a game's test-tooling probes (nxrt/tt.h GameProbes) may use of the test tooling's own memory helpers. The definitions are the test
// tooling's (src/testtools/tt_game.cpp); a game module sees only these declarations, not the tooling's private header.
#pragma once
#include <cstddef>
#include <cstdint>

namespace nxrt::tt::detail {

bool read_mem(uint64_t addr, void* out, size_t n);       // ReadProcessMemory on ourselves: false instead of a fault
bool write_mem(uint64_t addr, const void* in, size_t n);

}  // namespace nxrt::tt::detail
