// SPDX-License-Identifier: GPL-3.0-or-later
// Display tuning that the NVN back end reads on the render thread (stream S10): texture filtering overrides. Play mode writes them
// from the settings page; the sampler cache includes them in its key, so a change takes effect on the next draw without a restart.
#pragma once
#include <atomic>

namespace nxrt::gfx {

struct Tuning {
    std::atomic<int> anisotropy{0};        // 0 = what the game's samplers ask for; 2, 4, 8, 16 = at least this much on every filtering sampler
    std::atomic<float> lod_bias{0.0f};     // added to the LOD bias of every sampler (negative = sharper)
};

Tuning& tuning();                          // process-wide

}  // namespace nxrt::gfx
