// SPDX-License-Identifier: GPL-3.0-or-later
// Entry hook on NTonemap::build_tonemap_eval_params (main + 0x211c60, S14): the game writes the 80-byte tone-curve block the tone-map shader
// reads through x8. With the HDR scene active the hook lets the game's function run and then moves the shoulder's asymptote from 1 to the
// display's peak ratio (hdr_math extend_shoulder), so the curve keeps the game's look up to the start of the shoulder and keeps climbing above it.
#pragma once
#include <cstdint>

namespace nxrt::extras {

void tonemap_hook_install(uint64_t module_base);
void tonemap_hook_remove();
uint32_t tonemap_hook_calls();      // times the hook rewrote a block

}  // namespace nxrt::extras
