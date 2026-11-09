// SPDX-License-Identifier: GPL-3.0-or-later
// Builds the runtime's BC7 texture cache from the ASTC textures a game's setup hook lists (game_setup.h, GameSetup::texture_cache): every
// texture is untiled, transcoded to BC7 (tools/textures, astc2bc7 library, in-process) and written as one file in exactly the format and
// under exactly the key that the runtime (runtime/src/gfx/nvn/nvn_texdata.cpp, "NXB7" files) looks up when the game creates that
// texture. BCn and uncompressed textures need no cache entry (the runtime passes them through), so the hook lists only ASTC ones. How a
// game stores its textures (archives, headers, compression) is the hook's business; this file knows only the layout of the NVN texture.
//
// THE KEY MUST EQUAL THE RUNTIME'S. The key, the hash and the file header are formats/nx_formats.h, shared with the runtime
// (nxfmt::bc7_cache_key): for a texture whose game memory is `mem` of `storage_size` bytes (= AstcTexture::gpu_size) the key hashes the
// bytes seeded with { version, fmt.nvn, w0, h0, d0, levels, layers, block_height, pitch(0), quality }. fmt.nvn is the NVN format enum value
// (NVN_FORMAT_RGBA_ASTC_<w>x<h>[_SRGB], see runtime/src/gfx/nvn/third_party/nvn/nvn.h; nvn_astc_format below), w0/h0/d0 the level-0
// width/height/depth of the layout (d0 = 1 except 3D), levels = min(mips, floor(log2(max dimension)) + 1), layers = 6 for cube, array
// length for 2D arrays, else 1, block_height = gob_blocks_y of level 0 of the layout, quality = the bc7 preset. The layout comes from
// tegra::compute_layout with a TextureDesc built exactly like runtime/src/gfx/nvn/nvn_texture_layout.cpp does for the block-linear
// case (3D: gob_blocks_y 1 and gob_blocks_z 16; 2D: console_block_height).
// File path: <cache_dir>/<first two hex digits of the key>/<16 hex digits, lower case>.bc7 .
//
// WHAT AN ENTRY HOLDS. The header names the Vulkan format of the data and the runtime builds its image in that format (docs/S4f.md,
// "Texture cache entries"). The key does not depend on it. Three kinds, picked per texture from its pixels:
//   BC7 (VK_FORMAT_BC7_UNORM/SRGB_BLOCK)   the default, same sRGB-ness as the ASTC format.
//   BC5 (VK_FORMAT_BC5_UNORM_BLOCK)        a linear texture whose level 0 is fully opaque with a blue channel of at most 2 (two-channel
//                                          normal maps): R and G in two 8-value ramps per 4x4 block, sampled as (R, G, 0, 1).
//   RGBA8 (VK_FORMAT_R8G8B8A8_UNORM/SRGB)  a texture BC7 cannot hold: the BC7 result of level 0 layer 0 is under kRgba8PsnrDb dB
//                                          against the ASTC decode (the white-noise lookup texture is at 19 dB). Tight texels, levels
//                                          back to back, all layers of a level together.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "common.h"

namespace nxs {

enum class TexTarget { D2, D3, Cube, D2Array, Unsupported };   // Unsupported: a kind the cache cannot hold (rejected with a reason)

// One ASTC texture, as the game's hook describes it.
struct AstcTexture {
    std::string name;            // for the log
    TexTarget target = TexTarget::D2;
    u32 nvn_format = 0;          // NVN format enum value (nvn_astc_format); 0 = no NVN format: the texture is rejected
    u32 block_w = 4, block_h = 4;   // ASTC footprint (16-byte blocks)
    bool srgb = false;
    u32 width = 0, height = 0;   // level 0
    u32 layers = 1;              // 2D array: array length; 3D: depth; cube: ignored (6)
    u32 mips = 1;
    u64 gpu_size = 0;            // bytes of the tiled (block-linear) buffer
    u64 weight = 0;              // work estimate for progress and the order (largest first); 0 = width * height * layers
    std::function<std::vector<u8>()> read_tiled;   // the tiled buffer, gpu_size bytes (called from worker threads)
};

// NVN format enum value of an ASTC footprint (nvn.h: NVN_FORMAT_RGBA_ASTC_<w>x<h>[_SRGB], the 28 LDR formats); 0 for a footprint that has none.
inline int nvn_astc_format(u32 block_w, u32 block_h, bool srgb) {
    struct E { u32 w, h; bool srgb; u32 nvn; };
    static constexpr E kTable[] = {
        {4, 4, false, 0x79},   {5, 4, false, 0x7A},   {5, 5, false, 0x7B},   {6, 5, false, 0x7C},
        {6, 6, false, 0x7D},   {8, 5, false, 0x7E},   {8, 6, false, 0x7F},   {8, 8, false, 0x80},
        {10, 5, false, 0x81},  {10, 6, false, 0x82},  {10, 8, false, 0x83},  {10, 10, false, 0x84},
        {12, 10, false, 0x85}, {12, 12, false, 0x86},
        {4, 4, true, 0x87},    {5, 4, true, 0x88},    {5, 5, true, 0x89},    {6, 5, true, 0x8A},
        {6, 6, true, 0x8B},    {8, 5, true, 0x8C},    {8, 6, true, 0x8D},    {8, 8, true, 0x8E},
        {10, 5, true, 0x8F},   {10, 6, true, 0x90},   {10, 8, true, 0x91},   {10, 10, true, 0x92},
        {12, 10, true, 0x93},  {12, 12, true, 0x94},
    };
    for (const E& e : kTable)
        if (e.w == block_w && e.h == block_h && e.srgb == srgb) return static_cast<int>(e.nvn);
    return 0;
}

// One output level of the BC7 file, as the runtime's PreparedLevel.
struct TexOutLevel {
    u32 width, height, depth;
    u64 offset, size;    // into the data block; size covers all layers of the level
};
struct TexCachePlan {
    bool ok = false;               // false: not an ASTC texture the cache can hold (kind, format or size unsupported)
    u64 key = 0;
    u32 vk_format = 0;             // VK_FORMAT_BC7_UNORM_BLOCK (145) or VK_FORMAT_BC7_SRGB_BLOCK (146)
    u32 levels = 0, layers = 0;
    std::vector<TexOutLevel> level;
    u64 data_bytes = 0;            // sum of level sizes
    u64 file_bytes = 0;            // header + data
};
// Everything about the cache file of this texture that does not need the pixels except for the key hash (which reads `tiled`).
TexCachePlan texcache_plan(const AstcTexture& info, const u8* tiled, size_t tiled_len, int quality);
std::string texcache_path(const std::string& cache_dir_utf8, u64 key);

// Revision of the ASTC -> BC7 conversion. A bc7_cache folder made by another revision is rebuilt by setup (it carries a marker file,
// bc7_cache\encoder.txt); the runtime's own cache format and key are unchanged. 1: first version (opaque blocks without mode 6 and
// without channel rotation); 2: opaque alpha kept exactly 255 by pinning, rotation allowed (RG normal maps 2 to 12 dB better).
// 3: the block height follows the console's rule (1.5 times the block rows against 16/32/64/128, the runtime's console_block_height), which
// changes the key and the untiling of the textures where it differs from the smallest covering height; two-channel normal maps become
// BC5 entries and textures BC7 cannot hold become RGBA8 entries.
constexpr int kTextureEncoderRev = 3;

// BC7 results under this PSNR (dB, all four channels, level 0 layer 0 against the ASTC decode) make the entry RGBA8. Measured over the
// ASTC textures of the first game (4,413 of them) at quality 3: a white-noise lookup texture 19.2 dB, every other texture 33.6 dB or more.
constexpr double kRgba8PsnrDb = 28.0;

enum class TexCacheKind { Bc7, Bc5, Rgba8 };

// Level 0, layer 0, slice 0 of an ASTC texture as raw ASTC blocks (untiled, tightly packed), with its footprint. For quality
// tools (tools/texq.cpp); false when the texture is not one the cache can hold.
struct TexLevel0 {
    u32 width = 0, height = 0, block_w = 0, block_h = 0;
    bool srgb = false;
    std::vector<u8> astc;
};
bool texcache_level0(const AstcTexture& info, const u8* tiled, size_t tiled_len, TexLevel0& out);

// Transcodes one texture (untile, ASTC decode, then BC7, BC5 or RGBA8 as described at the top; threads = 1 inside the library) and
// returns the complete file bytes (NXB7 header + level table + data), identical in layout to nvn_texdata.cpp cache_write. The plan's
// geometry is the BC7/BC5 one; an RGBA8 entry has its own level table and size (texcache_rgba8_levels). kind (optional) receives the
// kind that was written. Throws Error on failure (never crashes on bad data).
std::vector<u8> texcache_build_file(const AstcTexture& info, const TexCachePlan& plan, const u8* tiled, size_t tiled_len, int quality,
                                    TexCacheKind* kind = nullptr);

// The level table and total file size of an RGBA8 entry for this plan (tight 4-byte texels, all layers of a level together).
std::vector<TexOutLevel> texcache_rgba8_levels(const TexCachePlan& plan, u64* file_bytes);

// Cheap check that an existing cache file is complete and matches the plan: magic, version, key, level table and size, for whichever of
// the three formats the header declares (BC7 as planned, BC5 for a linear texture, RGBA8 of the same sRGB-ness). Does not decode. This is
// what the runtime itself checks, so a file that passes here is accepted by the runtime. kind (optional) receives the format found.
bool texcache_file_valid(const std::string& path_utf8, const TexCachePlan& plan, TexCacheKind* kind = nullptr);

struct TexCacheStats {
    u32 textures_total = 0;      // ASTC textures handed over (duplicates by cache key counted once)
    u32 converted = 0, skipped = 0, failed = 0;
    u32 bc5 = 0, rgba8 = 0;      // of the converted: entries written as BC5 and as RGBA8 (the rest are BC7)
    u64 bytes_written = 0;
    double seconds = 0;
};
struct TexCacheOptions {
    std::string cache_dir;
    int quality = 3;             // bc7 preset 0..6
    int threads = 0;             // worker threads (0 = hardware threads); each converts whole textures, largest first
    // Optional: progress weighted by texel count (level 0 of every layer), which tracks the work far better than a texture count
    // because textures are processed largest first. Called together with `progress`.
    std::function<void(u64 done_weight, u64 total_weight)> weighted_progress;
    bool verify_existing = false; // reserved; existing files are always re-checked with texcache_file_valid (header and size)
};
// Converts every texture whose cache file is missing or invalid, in parallel, largest first. Resumable by construction (atomic
// .tmp -> rename per file; a re-run skips finished files). progress(done, total) counts textures (done includes skipped). cancelled() is polled
// between textures. Throws Error(Cancelled) when cancelled (finished files stay), Error(Disk) when a file cannot be written.
TexCacheStats build_texture_cache(const TexCacheOptions& o, std::vector<AstcTexture> textures,
                                  const std::function<void(u64 done, u64 total)>& progress,
                                  const std::function<bool()>& cancelled);

}  // namespace nxs
