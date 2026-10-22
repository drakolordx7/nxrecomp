// SPDX-License-Identifier: GPL-3.0-or-later
// Builds the runtime's BC7 texture cache from the extracted RomFS: every ASTC texture of every .pak is untiled, transcoded to BC7
// (tools/textures, astc2bc7 library, in-process) and written as one file in exactly the format and under exactly the key that the
// runtime (runtime/src/gfx/nvn/nvn_texdata.cpp, "NXB7" files) looks up when the game creates that texture. BCn and uncompressed
// textures need no cache entry (the runtime passes them through).
//
// THE KEY MUST EQUAL THE RUNTIME'S. nvn_texdata.cpp prepare_texture() computes, for a texture whose game memory is `mem` of
// `storage_size` bytes (= the TXTR's decompressed GPU buffer, i.e. TxtrInfo::gpu_size):
//   meta = { kCacheVersion(1), fmt.nvn, w0, h0, d0, levels, layers, block_height, pitch(0), quality }  (ten u64, in this order)
//   meta_hash = hash_bytes(meta, 80 bytes); key = hash_seeded(mem, storage_size, meta_hash)   (seed = meta_hash)
// where hash_seeded is the xxHash64-style function of that file (copy it exactly), fmt.nvn is the NVN format enum value
// (NVN_FORMAT_RGBA_ASTC_<w>x<h>[_SRGB], see runtime/src/gfx/nvn/third_party/nvn/nvn.h), w0/h0/d0 the level-0 width/height/depth of
// the layout (d0 = 1 except 3D), levels = min(TXTR mips, floor(log2(max dimension)) + 1), layers = 6 for cube, array length for
// 2D arrays, else 1, block_height = gob_blocks_y of level 0 of the layout, quality = the bc7 preset. The layout comes from
// tegra::compute_layout with a TextureDesc built exactly like runtime/src/gfx/nvn/nvn_texture_layout.cpp does for the block-linear
// case (gob_blocks_y = tegra::default_gob_blocks_y(blocks_y); 3D: gob_blocks_y 1 and gob_blocks_z 16).
// File path: <cache_dir>/<first two hex digits of the key>/<16 hex digits, lower case>.bc7 .
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "common.h"
#include "txtr.h"

namespace mprs {

// The runtime's hash of a byte range (hash_bytes in nvn_texdata.cpp, seed 0) and the seeded one it keys the cache with.
u64 tex_hash_bytes(const void* data, size_t len);
u64 tex_hash_seeded(const void* data, size_t len, u64 seed);

// NVN format enum of an ASTC TXTR format id (txtr_format(id).astc must be true); 0 if not ASTC.
int tex_nvn_astc_format(u32 txtr_format_id);

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
TexCachePlan texcache_plan(const TxtrInfo& info, const u8* tiled, size_t tiled_len, int quality);
std::string texcache_path(const std::string& cache_dir_utf8, u64 key);

// Revision of the ASTC -> BC7 conversion. A bc7_cache folder made by another revision is rebuilt by setup (it carries a marker file,
// bc7_cache\encoder.txt); the runtime's own cache format and key are unchanged. 1: first version (opaque blocks without mode 6 and
// without channel rotation); 2: opaque alpha kept exactly 255 by pinning, rotation allowed (RG normal maps 2 to 12 dB better).
constexpr int kTextureEncoderRev = 2;

// Level 0, layer 0, slice 0 of an ASTC texture as raw ASTC blocks (untiled, tightly packed), with its footprint. For quality
// tools (tools/texq.cpp); false when the texture is not one the cache can hold.
struct TexLevel0 {
    u32 width = 0, height = 0, block_w = 0, block_h = 0;
    bool srgb = false;
    std::vector<u8> astc;
};
bool texcache_level0(const TxtrInfo& info, const u8* tiled, size_t tiled_len, TexLevel0& out);

// Transcodes one texture (untile + ASTC -> BC7 per level/layer/slice with a2b_transcode, threads = 1 inside the library) and returns
// the complete file bytes (NXB7 header + level table + data), identical in layout to nvn_texdata.cpp cache_write.
// Throws Error on failure (never crashes on bad data).
std::vector<u8> texcache_build_file(const TxtrInfo& info, const TexCachePlan& plan, const u8* tiled, size_t tiled_len, int quality);

// Cheap check that an existing cache file is complete and matches the plan (size, magic, version, key, format, level table). Does
// not decode. This is what the runtime itself checks, so a file that passes here is accepted by the runtime.
bool texcache_file_valid(const std::string& path_utf8, const TexCachePlan& plan);

struct TexCacheStats {
    u32 textures_total = 0;      // ASTC TXTR entries found (duplicates across paks counted once, by cache key)
    u32 converted = 0, skipped = 0, failed = 0;
    u64 bytes_written = 0;
    double seconds = 0;
};
struct TexCacheOptions {
    std::string romfs_dir;       // extracted RomFS (the .pak files are read from here, recursively)
    std::string cache_dir;
    int quality = 3;             // bc7 preset 0..6
    int threads = 0;             // worker threads (0 = hardware threads); each converts whole textures, largest first
    // Optional: progress weighted by texel count (level 0 of every layer), which tracks the work far better than a texture count
    // because textures are processed largest first. Called together with `progress`.
    std::function<void(u64 done_weight, u64 total_weight)> weighted_progress;
    bool verify_existing = false; // reserved; existing files are always re-checked with texcache_file_valid (header and size)
};
// Scans the paks, builds the list of ASTC textures (reads TXTR headers only), then converts every texture whose cache file is
// missing or invalid, in parallel. Resumable by construction (atomic .tmp -> rename per file; a re-run skips finished files).
// progress(done, total) counts textures (done includes skipped). cancelled() is polled between textures.
// Throws Error(Cancelled) when cancelled (finished files stay), Error(Disk) when a file cannot be written.
TexCacheStats build_texture_cache(const TexCacheOptions& o, const std::function<void(u64 done, u64 total)>& progress,
                                  const std::function<bool()>& cancelled);

}  // namespace mprs
