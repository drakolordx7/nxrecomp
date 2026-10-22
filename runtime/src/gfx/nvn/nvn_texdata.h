// SPDX-License-Identifier: GPL-3.0-or-later
// CPU-side texture data preparation (S4d). No Vulkan calls: the output is plain bytes plus the VkFormat that the GPU
// layer copies them into. Implementation: nvn_texdata.cpp.
//
// What prepare_texture does with the bytes of a texture in the game's memory:
//   * block-linear (Tegra X1 layout) is untiled with tegra::untile into a tight linear buffer; pitch-linear rows are copied
//     into tight rows. Either way the result is the image, levels back to back, inside a level all layers, inside a layer
//     the z slices, then rows of row_blocks blocks.
//   * BCn (BC1..BC7, BC6H, BC4/5) blocks are passed through unchanged.
//   * ASTC is kept when caps.astc, otherwise transcoded to BC7 once (caps.bc7_transcode; the result is cached on disk, keyed
//     by a hash of the source bytes), otherwise decoded to RGBA8.
//   * Output sizes: BC7 levels are rounded up to whole 4x4 blocks, i.e. (w+3)/4 * (h+3)/4 * 16 bytes per layer and slice.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <volk.h>

#include "nvn_core.h"

namespace nxrt::nvn::be {

struct TexCaps {
    bool bc = true;              // the GPU samples BCn; BCn data is passed through
    bool astc = false;           // the GPU samples ASTC; ASTC data is kept
    bool bc7_transcode = true;   // when astc is false: transcode ASTC to BC7 (else decode it to RGBA8)
};

struct PreparedLevel {
    uint32_t width, height, depth;   // texels of the level
    uint64_t offset, size;           // bytes in PreparedTexture::data; the size covers all layers of the level
    uint32_t row_blocks;             // blocks per row of the output data (texels per row for uncompressed formats)
};

struct PreparedTexture {
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t levels = 0, layers = 0;
    uint32_t block_w = 1, block_h = 1;   // texels per block of the output format
    uint32_t block_bytes = 0;            // bytes per block (texel bytes for uncompressed formats)
    bool transcoded = false;             // ASTC was transcoded to BC7 or decoded to RGBA8 (the data is not the source encoding)
    bool from_cache = false;             // the data came from the BC7 disk cache
    std::vector<PreparedLevel> level;
    std::vector<uint8_t> data;
    std::string error;                   // set when prepare_texture returns false
};

// Prepares the data of t for the GPU described by caps. Returns false with out.error set on failure (no storage, bad layout,
// no Vulkan format, a codec error, ...). Never crashes on a bad texture.
bool prepare_texture(const Texture& t, const TexCaps& caps, PreparedTexture& out);

// Fast 64-bit hash of a byte range (xxHash64-style mix). Used for the cache key.
uint64_t hash_bytes(const void* data, size_t size);

// Cache file of a key: <[nvn] bc7_cache dir>/<first 2 hex digits>/<16 hex digits>.bc7. Empty when the cache is disabled.
std::string bc7_cache_path(const Texture& t, uint64_t hash);

}  // namespace nxrt::nvn::be
