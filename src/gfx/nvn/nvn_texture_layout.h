// SPDX-License-Identifier: GPL-3.0-or-later
// Storage layout of an NVN texture, computed from its builder (family texture, S4d). Sizes come from the Maxwell
// block-linear rules in tools/tiling (tegra::compute_layout); the rules were checked against the GPU buffer sizes of the
// game's 6,927 textures (docs/mpr/survey/SURVEY.md section 3).
#pragma once
#include <cstdint>

#include "nvn_decoded.h"
#include "nvn_format.h"
#include "tegra_tiling.h"

namespace nxrt::nvn::be {

struct TexLayout {
    tegra::TextureDesc desc;     // what tegra::untile_level / tile_level take (block-linear textures)
    tegra::Layout layout;        // per-level offsets and sizes of one layer (block-linear textures)
    uint32_t levels = 0, layers = 0;
    bool block_linear = true;    // false: pitch-linear (NVN_TEXTURE_FLAGS_LINEAR)
    uint64_t pitch = 0;          // pitch-linear only: bytes per row
    uint64_t storage_size = 0;   // bytes of the texture's storage: all levels, layers and samples
    uint64_t alignment = 0;      // storage alignment the layout implies
    const FormatInfo* fmt = nullptr;
    bool valid = false;          // false: unknown format, bad size, a target without a layout (buffer), or a bad layout
};

// The layout of the texture the builder describes. Never fails on a zero-filled builder: it returns valid = false.
TexLayout compute_texture_layout(const TextureBuilderInfo& b);

}  // namespace nxrt::nvn::be
