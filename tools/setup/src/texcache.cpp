// SPDX-License-Identifier: GPL-3.0-or-later
// BC7 texture cache writer, generic: the game's setup hook hands over the ASTC textures (texcache.h, AstcTexture). Port of the runtime's
// CPU-side texture preparation (runtime/src/gfx/nvn/nvn_texdata.cpp: prepare_texture's Mode::Bc7 branch) and of nvn_texture_layout.cpp
// (layout rules for block-linear 2D, cube, 2D array and 3D); the hash, the key and the file header are the shared ones in
// formats/nx_formats.h. See texcache.h for the contract. Libraries: tegra_tiling (untile) and astc2bc7 (ASTC -> BC7 transcode).
#include "texcache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include <cmath>
#include <cwchar>

#include "astc2bc7.h"
#include "nx_formats.h"
#include "bc7decomp.h"
#include "fileio.h"
#include "log.h"
#include "rgbcx.h"
#include "storage.h"
#include "tegra_tiling.h"

namespace nxs {
namespace {

// ---- constants of the runtime (nvn_texdata.cpp); the cache header and the key are nx_formats.h ------------------------------------
constexpr u32 kVkBc7Unorm = 145;                            // VK_FORMAT_BC7_UNORM_BLOCK
constexpr u32 kVkBc7Srgb = 146;                             // VK_FORMAT_BC7_SRGB_BLOCK
constexpr u32 kVkBc5 = 141;                                 // VK_FORMAT_BC5_UNORM_BLOCK
constexpr u32 kVkRgba8Unorm = 37;                           // VK_FORMAT_R8G8B8A8_UNORM
constexpr u32 kVkRgba8Srgb = 43;                            // VK_FORMAT_R8G8B8A8_SRGB
constexpr size_t kCacheHeadBytes = nxfmt::kBc7HeadBytes;
constexpr size_t kCacheLevelBytes = nxfmt::kBc7LevelBytes;
using nxfmt::get32;
using nxfmt::get64;
using nxfmt::put32;
using nxfmt::put64;

u64 div_up(u64 a, u64 b) { return (a + b - 1) / b; }

// ---- layout (nvn_texture_layout.cpp, block-linear case) ----------------------------------------------------------------------
// The block height (in GOBs) of a 2D texture's level 0 on the console: 1.5 times the block rows decides, against 16, 32, 64 and 128 rows
// (the runtime's console_block_height in nvn_texture_layout.cpp). The smaller "smallest height that covers the rows" of
// tegra::default_gob_blocks_y has the same storage size for every game texture but scrambles the texels of the textures where the two
// differ, and the cache key carries the block height.
u32 console_block_height(u32 block_rows) {
    const u32 hh = block_rows + block_rows / 2;
    return hh >= 128 ? 16 : hh >= 64 ? 8 : hh >= 32 ? 4 : hh >= 16 ? 2 : 1;
}

struct CacheLayout {
    tegra::TextureDesc desc;
    tegra::Layout layout;
    u32 levels = 0, layers = 0;
    u64 storage = 0;   // layout.tiled_layer_stride * layers: the bytes the runtime hashes and allocates
};

// Builds the layout the runtime builds for this TXTR. Returns nullptr when accepted, else why not.
const char* build_layout(const AstcTexture& info, CacheLayout& out) {
    if (info.target == TexTarget::Unsupported) return "texture kind is not cached (only 2D, cube, 2D array and 3D)";
    if (info.width == 0 || info.height == 0) return "width or height is zero";
    const bool is3d = info.target == TexTarget::D3;
    const u32 w = info.width, h = info.height;
    const u32 d = is3d ? info.layers : 1;
    if (is3d && d == 0) return "3D texture has depth zero";

    u32 layers = 1;
    if (info.target == TexTarget::Cube) {
        layers = 6;
    } else if (info.target == TexTarget::D2Array) {
        if (info.layers == 0) return "2D array has zero layers";
        layers = info.layers;
    }

    // levels = min(mips, floor(log2(largest dimension)) + 1), at least 1
    const u32 maxdim = is3d ? std::max({w, h, d}) : std::max(w, h);
    u32 top = 1;
    while (top < 32 && (static_cast<u64>(maxdim) >> top) != 0) ++top;
    const u32 levels = std::min<u32>(std::max<u32>(1, info.mips), top);

    tegra::TextureDesc& ds = out.desc;
    ds.width = w;
    ds.height = h;
    ds.depth = d;
    ds.levels = levels;
    ds.layers = layers;   // 3D: the layout requires 1 (the slices are depth)
    if (is3d) ds.layers = 1;
    ds.block_w = info.block_w;
    ds.block_h = info.block_h;
    ds.bytes_per_block = 16;   // every ASTC block
    ds.linear_stride_align = 1;   // tight rows, as the runtime's untile target
    if (is3d) {
        ds.gob_blocks_y = 1;
        ds.gob_blocks_z = 16;
    } else {
        const u32 blocks_y = static_cast<u32>(div_up(h, info.block_h));
        ds.gob_blocks_y = console_block_height(blocks_y);   // no packaged layout for TXTR textures
        ds.gob_blocks_z = 1;
    }
    out.layout = tegra::compute_layout(ds);
    if (!out.layout.valid || out.layout.levels.empty()) return "tegra::compute_layout rejects the size or block height";
    out.levels = levels;
    out.layers = ds.layers;
    out.storage = out.layout.tiled_layer_stride * out.layers;
    return nullptr;
}

// Checks everything texcache_plan needs. Returns nullptr when the texture is cacheable (then L is filled), else the reason.
const char* check_texture(const AstcTexture& info, size_t tiled_len, int quality, CacheLayout& L) {
    if (quality < 0 || quality > 6) return "quality is outside 0..6";
    if (info.nvn_format == 0) return "no NVN format for this ASTC footprint";
    if (const char* why = build_layout(info, L)) return why;
    if (info.target != TexTarget::D3 && L.storage != info.gpu_size)
        return "layout layer stride times layers differs from the buffer's gpu_size";
    if (L.storage > tiled_len) return "tiled buffer is shorter than the layout's storage size";
    return nullptr;
}

// Output level table, as prepare_texture's Mode::Bc7 branch: BC7 blocks per level, all layers, offsets back to back.
void fill_levels(const CacheLayout& L, std::vector<TexOutLevel>& out, u64& data_bytes) {
    out.clear();
    u64 off = 0;
    for (const tegra::LevelLayout& lv : L.layout.levels) {
        const u64 row_blocks = div_up(lv.width, 4);
        const u64 layer_bytes = row_blocks * div_up(lv.height, 4) * 16 * lv.depth;
        TexOutLevel o{};
        o.width = lv.width;
        o.height = lv.height;
        o.depth = lv.depth;
        o.offset = off;
        o.size = layer_bytes * L.layers;
        off += o.size;
        out.push_back(o);
    }
    data_bytes = off;
}

a2b_status ensure_transcoder() {
    static std::once_flag once;
    static a2b_status status = A2B_ERR_NO_ASTC_LIB;
    std::call_once(once, [] { status = a2b_init(nullptr); });
    return status;
}

struct Claim {
    std::mutex& m;
    std::set<u64>& keys;
    u64 key;
    bool keep = false;
    ~Claim() {
        if (keep) return;
        std::lock_guard<std::mutex> lk(m);
        keys.erase(key);
    }
};

double g_rgba8_psnr_db = kRgba8PsnrDb;   // tests move it (detail::texcache_set_rgba8_psnr)

}  // namespace

// ---- public -----------------------------------------------------------------------------------------------------------------
namespace detail {

void texcache_set_rgba8_psnr(double db) { g_rgba8_psnr_db = db; }

// Why texcache_plan refuses this texture (nullptr when it accepts it). Used by the build log and the test.
const char* texcache_reject_reason(const AstcTexture& info, size_t tiled_len, int quality) {
    CacheLayout L;
    return check_texture(info, tiled_len, quality, L);
}

}  // namespace detail

TexCachePlan texcache_plan(const AstcTexture& info, const u8* tiled, size_t tiled_len, int quality) {
    TexCachePlan p;
    if (!tiled) return p;
    CacheLayout L;
    if (check_texture(info, tiled_len, quality, L) != nullptr) return p;

    // key = hash_seeded(tiled bytes, storage, seed = hash_bytes(meta)): nxfmt::bc7_cache_key, the runtime's prepare_texture meta
    const tegra::LevelLayout& l0 = L.layout.levels[0];
    nxfmt::Bc7Meta meta;
    meta.nvn_format = info.nvn_format;
    meta.width = l0.width;
    meta.height = l0.height;
    meta.depth = l0.depth;
    meta.levels = L.levels;
    meta.layers = L.layers;
    meta.block_height = l0.gob_blocks_y;
    meta.pitch = 0;   // block-linear
    meta.quality = static_cast<u64>(quality);
    p.key = nxfmt::bc7_cache_key(tiled, static_cast<size_t>(L.storage), meta);
    p.vk_format = info.srgb ? kVkBc7Srgb : kVkBc7Unorm;
    p.levels = L.levels;
    p.layers = L.layers;
    fill_levels(L, p.level, p.data_bytes);
    p.file_bytes = kCacheHeadBytes + p.level.size() * kCacheLevelBytes + p.data_bytes;
    p.ok = true;
    return p;
}

std::string texcache_path(const std::string& cache_dir_utf8, u64 key) {
    const std::filesystem::path p = std::filesystem::path(widen(cache_dir_utf8)) / nxfmt::bc7_cache_rel_path(key);
    return narrow(p.wstring());
}

bool texcache_level0(const AstcTexture& info, const u8* tiled, size_t tiled_len, TexLevel0& out) {
    CacheLayout L;
    if (check_texture(info, tiled_len, 3, L) != nullptr) return false;
    const tegra::LevelLayout& last = L.layout.levels.back();
    const u64 lin_total = last.linear_offset + last.linear_slice_size * L.layers;
    std::vector<u8> lin(static_cast<size_t>(lin_total));
    if (!tegra::untile(L.desc, L.layout, tiled, lin.data())) return false;
    const tegra::LevelLayout& g = L.layout.levels[0];
    const u64 in_slice = g.linear_slice_size / g.depth;
    out.width = g.width;
    out.height = g.height;
    out.block_w = info.block_w;
    out.block_h = info.block_h;
    out.srgb = info.srgb;
    out.astc.assign(lin.begin() + static_cast<ptrdiff_t>(g.linear_offset), lin.begin() + static_cast<ptrdiff_t>(g.linear_offset + in_slice));
    return true;
}

std::vector<TexOutLevel> texcache_rgba8_levels(const TexCachePlan& plan, u64* file_bytes) {
    std::vector<TexOutLevel> out;
    u64 off = 0;
    for (const TexOutLevel& l : plan.level) {
        TexOutLevel o = l;
        o.offset = off;
        o.size = static_cast<u64>(l.width) * l.height * 4 * l.depth * plan.layers;
        off += o.size;
        out.push_back(o);
    }
    if (file_bytes) *file_bytes = kCacheHeadBytes + out.size() * kCacheLevelBytes + off;
    return out;
}

namespace {

// BC5 of one 2D slice of tight RGBA8 texels: R and G, 4x4 blocks, the edge texels replicated into partial blocks.
void encode_bc5_slice(u32 w, u32 h, const u8* rgba, u8* out) {
    static std::once_flag once;
    std::call_once(once, [] { rgbcx::init(); });
    const u32 bx = static_cast<u32>(div_up(w, 4)), by = static_cast<u32>(div_up(h, 4));
    for (u32 y = 0; y < by; ++y)
        for (u32 x = 0; x < bx; ++x) {
            u8 px[16 * 4];
            for (u32 j = 0; j < 4; ++j)
                for (u32 i = 0; i < 4; ++i) {
                    const u32 sx = std::min(x * 4 + i, w - 1), sy = std::min(y * 4 + j, h - 1);
                    std::memcpy(px + (j * 4 + i) * 4, rgba + (static_cast<u64>(sy) * w + sx) * 4, 4);
                }
            rgbcx::encode_bc5_hq(out + (static_cast<u64>(y) * bx + x) * 16, px, 0, 1, 4);
        }
}

// PSNR over all four channels of a slice of BC7 blocks against the RGBA8 it was made from (99 dB when identical).
double bc7_slice_psnr(u32 w, u32 h, const u8* bc7, const u8* rgba) {
    const u32 bx = static_cast<u32>(div_up(w, 4)), by = static_cast<u32>(div_up(h, 4));
    double se = 0;
    for (u32 y = 0; y < by; ++y)
        for (u32 x = 0; x < bx; ++x) {
            bc7decomp::color_rgba blk[16];
            if (!bc7decomp_ref::unpack_bc7(bc7 + (static_cast<u64>(y) * bx + x) * 16, blk)) std::memset(blk, 0, sizeof blk);
            for (u32 j = 0; j < 4; ++j)
                for (u32 i = 0; i < 4; ++i) {
                    const u32 px = x * 4 + i, py = y * 4 + j;
                    if (px >= w || py >= h) continue;
                    const u8* s = rgba + (static_cast<u64>(py) * w + px) * 4;
                    const bc7decomp::color_rgba& c = blk[j * 4 + i];
                    const double d0 = double(s[0]) - c.r, d1 = double(s[1]) - c.g, d2 = double(s[2]) - c.b, d3 = double(s[3]) - c.a;
                    se += d0 * d0 + d1 * d1 + d2 * d2 + d3 * d3;
                }
        }
    const double mse = se / (4.0 * w * h);
    return mse <= 0 ? 99.0 : std::min(99.0, 10.0 * std::log10(255.0 * 255.0 / mse));
}

// Level 0 is opaque and has (almost) no blue: two-channel data, BC5 holds it.
bool is_two_channel(const std::vector<u8>& rgba) {
    for (size_t i = 0; i + 3 < rgba.size(); i += 4)
        if (rgba[i + 3] != 255 || rgba[i + 2] > 2) return false;
    return true;
}

}  // namespace

std::vector<u8> texcache_build_file(const AstcTexture& info, const TexCachePlan& plan, const u8* tiled, size_t tiled_len, int quality,
                                    TexCacheKind* kind_out) {
    if (!plan.ok) fail(Fail::Generic, "Texture %s: the cache plan was rejected, nothing to build.", info.name.c_str());
    CacheLayout L;
    if (const char* why = check_texture(info, tiled_len, quality, L))
        fail(Fail::Generic, "Texture %s cannot be built: %s.", info.name.c_str(), why);
    std::vector<TexOutLevel> levels;
    u64 data_bytes = 0;
    fill_levels(L, levels, data_bytes);
    if (levels.size() != plan.level.size() || data_bytes != plan.data_bytes || L.levels != plan.levels ||
        L.layers != plan.layers)
        fail(Fail::Generic, "Texture %s: the plan does not match the texture.", info.name.c_str());
    for (size_t i = 0; i < levels.size(); ++i) {
        const TexOutLevel& a = levels[i];
        const TexOutLevel& b = plan.level[i];
        if (a.width != b.width || a.height != b.height || a.depth != b.depth || a.offset != b.offset || a.size != b.size)
            fail(Fail::Generic, "Texture %s: the plan does not match the texture.", info.name.c_str());
    }
    const u32 vk = info.srgb ? kVkBc7Srgb : kVkBc7Unorm;
    if (vk != plan.vk_format) fail(Fail::Generic, "Texture %s: the plan has the wrong format.", info.name.c_str());

    const a2b_status init = ensure_transcoder();
    if (init != A2B_OK)
        fail(Fail::Generic, "The ASTC transcoder is not available (%s).", a2b_status_string(init));

    const tegra::LevelLayout& last = L.layout.levels.back();
    const u64 lin_total = last.linear_offset + last.linear_slice_size * L.layers;
    std::vector<u8> lin(static_cast<size_t>(lin_total));
    if (!tegra::untile(L.desc, L.layout, tiled, lin.data()))
        fail(Fail::Generic, "Texture %s: untile failed.", info.name.c_str());

    a2b_options opt{};
    opt.quality = quality;
    opt.threads = 1;
    opt.perceptual = 0;
    opt.allow_opaque_mode6 = 0;

    const auto astc_in = [&](size_t i, a2b_input& in) {
        const tegra::LevelLayout& g = L.layout.levels[i];
        in = a2b_input{};
        in.format = info.srgb ? A2B_ASTC_SRGB : A2B_ASTC_UNORM;
        in.width = g.width;
        in.height = g.height;
        in.block_w = info.block_w;
        in.block_h = info.block_h;
    };
    const auto astc_src = [&](size_t i, u32 layer, u32 z) {
        const tegra::LevelLayout& g = L.layout.levels[i];
        return lin.data() + g.linear_offset + static_cast<u64>(layer) * g.linear_slice_size + z * (g.linear_slice_size / g.depth);
    };
    const auto decode_slice = [&](size_t i, u32 layer, u32 z, u8* rgba) {
        a2b_input in;
        astc_in(i, in);
        const tegra::LevelLayout& g = L.layout.levels[i];
        const a2b_status st = a2b_decode_astc_rgba8(&in, astc_src(i, layer, z), static_cast<size_t>(g.linear_slice_size / g.depth), 1, rgba,
                                                    nullptr);
        if (st != A2B_OK)
            fail(Fail::Generic, "Texture %s: ASTC decode failed (%s).", info.name.c_str(), a2b_status_string(st));
    };

    const tegra::LevelLayout& g0 = L.layout.levels[0];
    const u64 px0 = static_cast<u64>(g0.width) * g0.height;
    const u32 slices0 = L.layers * g0.depth;
    std::vector<u8> rgba0(static_cast<size_t>(px0 * 4 * slices0));
    for (u32 layer = 0; layer < L.layers; ++layer)
        for (u32 z = 0; z < g0.depth; ++z) decode_slice(0, layer, z, rgba0.data() + (static_cast<u64>(layer) * g0.depth + z) * px0 * 4);

    std::vector<u8> data(static_cast<size_t>(data_bytes));
    TexCacheKind kind = TexCacheKind::Bc7;
    const u64 bc_slice0 = plan.level[0].size / L.layers / g0.depth;
    if (!info.srgb && is_two_channel(rgba0)) {
        kind = TexCacheKind::Bc5;
    } else {
        for (u32 s = 0; s < slices0; ++s) {
            const a2b_status st = a2b_encode_bc7(g0.width, g0.height, rgba0.data() + static_cast<u64>(s) * px0 * 4, &opt,
                                                 data.data() + plan.level[0].offset + s * bc_slice0, static_cast<size_t>(bc_slice0), nullptr);
            if (st != A2B_OK)
                fail(Fail::Generic, "Texture %s: BC7 encode failed (%s).", info.name.c_str(), a2b_status_string(st));
        }
        if (bc7_slice_psnr(g0.width, g0.height, data.data() + plan.level[0].offset, rgba0.data()) < g_rgba8_psnr_db) kind = TexCacheKind::Rgba8;
    }

    std::vector<TexOutLevel> table = plan.level;
    u64 file_bytes = plan.file_bytes;
    u32 out_vk = plan.vk_format;
    std::vector<u8> tmp;
    switch (kind) {
        case TexCacheKind::Bc7:
            for (size_t i = 1; i < L.layout.levels.size(); ++i) {
                const tegra::LevelLayout& g = L.layout.levels[i];
                const TexOutLevel& out = plan.level[i];
                a2b_input in;
                astc_in(i, in);
                const u64 out_slice = out.size / L.layers / g.depth;
                if (a2b_output_size(&in) != out_slice)
                    fail(Fail::Generic, "Texture %s: internal error, BC7 slice size mismatch.", info.name.c_str());
                for (u32 layer = 0; layer < L.layers; ++layer)
                    for (u32 z = 0; z < g.depth; ++z) {
                        u8* dst = data.data() + out.offset + (static_cast<u64>(layer) * g.depth + z) * out_slice;
                        a2b_format of = A2B_BC7_UNORM;
                        const a2b_status st = a2b_transcode(&in, astc_src(i, layer, z), static_cast<size_t>(g.linear_slice_size / g.depth),
                                                            &opt, &of, dst, static_cast<size_t>(out_slice), nullptr);
                        if (st != A2B_OK)
                            fail(Fail::Generic, "Texture %s: ASTC to BC7 transcode failed (%s).", info.name.c_str(),
                                 a2b_status_string(st));
                    }
            }
            break;
        case TexCacheKind::Bc5:
            out_vk = kVkBc5;
            for (size_t i = 0; i < L.layout.levels.size(); ++i) {
                const tegra::LevelLayout& g = L.layout.levels[i];
                const TexOutLevel& out = plan.level[i];
                const u64 out_slice = out.size / L.layers / g.depth;
                const u64 px = static_cast<u64>(g.width) * g.height;
                for (u32 layer = 0; layer < L.layers; ++layer)
                    for (u32 z = 0; z < g.depth; ++z) {
                        const u8* rgba = nullptr;
                        if (i == 0) {
                            rgba = rgba0.data() + (static_cast<u64>(layer) * g.depth + z) * px * 4;
                        } else {
                            tmp.resize(static_cast<size_t>(px * 4));
                            decode_slice(i, layer, z, tmp.data());
                            rgba = tmp.data();
                        }
                        encode_bc5_slice(g.width, g.height, rgba, data.data() + out.offset + (static_cast<u64>(layer) * g.depth + z) * out_slice);
                    }
            }
            break;
        case TexCacheKind::Rgba8: {
            out_vk = info.srgb ? kVkRgba8Srgb : kVkRgba8Unorm;
            table = texcache_rgba8_levels(plan, &file_bytes);
            data.assign(static_cast<size_t>(file_bytes - kCacheHeadBytes - table.size() * kCacheLevelBytes), 0);
            for (size_t i = 0; i < L.layout.levels.size(); ++i) {
                const tegra::LevelLayout& g = L.layout.levels[i];
                const u64 slice = static_cast<u64>(g.width) * g.height * 4;
                for (u32 layer = 0; layer < L.layers; ++layer)
                    for (u32 z = 0; z < g.depth; ++z) {
                        u8* dst = data.data() + table[i].offset + (static_cast<u64>(layer) * g.depth + z) * slice;
                        if (i == 0) std::memcpy(dst, rgba0.data() + (static_cast<u64>(layer) * g.depth + z) * slice, static_cast<size_t>(slice));
                        else decode_slice(i, layer, z, dst);
                    }
            }
            break;
        }
    }

    std::vector<u8> out;
    out.reserve(static_cast<size_t>(file_bytes));
    std::vector<nxfmt::Bc7Level> head_levels;
    for (const TexOutLevel& l : table) head_levels.push_back({l.width, l.height, l.depth, l.offset, l.size});
    nxfmt::bc7_put_head(out, plan.key, out_vk, L.layers, head_levels.data(), head_levels.size());
    out.insert(out.end(), data.begin(), data.end());
    if (out.size() != file_bytes)
        fail(Fail::Generic, "Texture %s: internal error, file size mismatch.", info.name.c_str());
    if (kind_out) *kind_out = kind;
    return out;
}

bool texcache_file_valid(const std::string& path_utf8, const TexCachePlan& plan, TexCacheKind* kind_out) {
    if (!plan.ok) return false;
    std::ifstream f(std::filesystem::path(widen(path_utf8)), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    f.seekg(0, std::ios::beg);
    if (size < 0) return false;

    std::vector<u8> head(kCacheHeadBytes);
    if (static_cast<size_t>(size) < head.size() || !f.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size())))
        return false;
    const u8* p = head.data();
    if (!nxfmt::bc7_head_matches(p, head.size(), plan.key)) return false;

    const bool srgb = plan.vk_format == kVkBc7Srgb;
    const u32 vk = get32(p + 16);
    TexCacheKind kind;
    std::vector<TexOutLevel> table;
    u64 want_size = plan.file_bytes;
    if (vk == plan.vk_format) {
        kind = TexCacheKind::Bc7;
        table = plan.level;
    } else if (vk == kVkBc5 && !srgb) {
        kind = TexCacheKind::Bc5;
        table = plan.level;
    } else if (vk == (srgb ? kVkRgba8Srgb : kVkRgba8Unorm)) {
        kind = TexCacheKind::Rgba8;
        table = texcache_rgba8_levels(plan, &want_size);
    } else {
        return false;
    }
    if (static_cast<u64>(size) != want_size) return false;

    const size_t nlevels = table.size();
    if (get32(p + 20) != nlevels) return false;
    if (get32(p + 24) != plan.layers) return false;
    std::vector<u8> tab(nlevels * kCacheLevelBytes);
    if (!f.read(reinterpret_cast<char*>(tab.data()), static_cast<std::streamsize>(tab.size()))) return false;
    for (size_t i = 0; i < nlevels; ++i) {
        const u8* e = tab.data() + i * kCacheLevelBytes;
        const TexOutLevel& want = table[i];
        if (get32(e) != want.width || get32(e + 4) != want.height || get32(e + 8) != want.depth ||
            get64(e + 12) != want.offset || get64(e + 20) != want.size)
            return false;
    }
    if (kind_out) *kind_out = kind;
    return true;
}

TexCacheStats build_texture_cache(const TexCacheOptions& o, std::vector<AstcTexture> textures,
                                  const std::function<void(u64 done, u64 total)>& progress, const std::function<bool()>& cancelled) {
    const auto t0 = std::chrono::steady_clock::now();
    TexCacheStats stats;
    if (o.cache_dir.empty()) fail(Fail::Input, "No cache folder was given.");
    if (o.quality < 0 || o.quality > 6) fail(Fail::Generic, "The BC7 quality must be 0 to 6.");
    if (const a2b_status st = ensure_transcoder(); st != A2B_OK)
        fail(Fail::Generic, "The ASTC transcoder is not available (%s).", a2b_status_string(st));

    struct Job {
        AstcTexture info;
        u64 weight;
    };
    std::vector<Job> jobs;
    jobs.reserve(textures.size());
    for (AstcTexture& t : textures) {
        const u64 weight = t.weight ? t.weight : static_cast<u64>(t.width) * t.height * std::max<u32>(1, t.layers);
        jobs.push_back(Job{std::move(t), weight});
    }
    textures.clear();
    std::stable_sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.weight > b.weight; });
    const size_t total = jobs.size();
    u64 total_weight = 0, done_weight = 0;
    for (const Job& j : jobs) total_weight += j.weight;

    std::mutex m;                    // guards claimed, the counters, disk_error and the progress callback
    std::set<u64> claimed;           // keys claimed by a job in this run
    std::atomic<size_t> next{0};
    std::atomic<bool> stop{false};
    bool cancel_seen = false;
    std::string disk_error;
    u64 done = 0;

    enum class Outcome { Converted, Skipped, Rejected };
    struct Result {
        Outcome outcome = Outcome::Rejected;
        u64 bytes = 0;
        TexCacheKind kind = TexCacheKind::Bc7;
    };

    const auto run_job = [&](const Job& j) -> Result {
        const std::vector<u8> tiled = j.info.read_tiled();
        const TexCachePlan plan = texcache_plan(j.info, tiled.data(), tiled.size(), o.quality);
        if (!plan.ok) {
            const char* why = detail::texcache_reject_reason(j.info, tiled.size(), o.quality);
            NXS_LOGW("Texture %s: not cached, the cache plan rejected it: %s", j.info.name.c_str(),
                      why ? why : "unknown reason");
            return {Outcome::Rejected, 0};
        }
        {
            std::lock_guard<std::mutex> lk(m);
            if (!claimed.insert(plan.key).second) return {Outcome::Skipped, 0};
        }
        Claim claim{m, claimed, plan.key};
        const std::string path = texcache_path(o.cache_dir, plan.key);
        if (texcache_file_valid(path, plan)) {
            claim.keep = true;
            return {Outcome::Skipped, 0};
        }
        TexCacheKind kind = TexCacheKind::Bc7;
        const std::vector<u8> bytes = texcache_build_file(j.info, plan, tiled.data(), tiled.size(), o.quality, &kind);
        {
            AtomicFileWriter w(path);
            w.write(bytes.data(), bytes.size());
            w.finish();
        }
        claim.keep = true;
        return {Outcome::Converted, bytes.size(), kind};
    };

    const auto work = [&]() {
        for (;;) {
            if (stop.load()) return;
            if (cancelled && cancelled()) {
                std::lock_guard<std::mutex> lk(m);
                cancel_seen = true;
                stop = true;
                return;
            }
            const size_t idx = next.fetch_add(1);
            if (idx >= total) return;
            const Job& j = jobs[idx];

            Result r;
            bool failed_job = false;
            bool disk = false;
            std::string err;
            try {
                r = run_job(j);
            } catch (const Error& e) {
                failed_job = true;
                disk = e.kind == Fail::Disk;
                err = e.what();
            } catch (const std::exception& e) {
                failed_job = true;
                err = e.what();
            }

            if (disk) {
                NXS_LOGE("Texture %s: %s", j.info.name.c_str(), err.c_str());
                std::lock_guard<std::mutex> lk(m);
                if (disk_error.empty()) disk_error = err;
                stop = true;
                return;
            }
            if (failed_job) NXS_LOGW("Texture %s: not cached: %s", j.info.name.c_str(), err.c_str());

            std::lock_guard<std::mutex> lk(m);
            if (failed_job) {
                ++stats.failed;
            } else if (r.outcome == Outcome::Converted) {
                ++stats.converted;
                stats.bytes_written += r.bytes;
                if (r.kind == TexCacheKind::Bc5) ++stats.bc5;
                if (r.kind == TexCacheKind::Rgba8) ++stats.rgba8;
            } else if (r.outcome == Outcome::Skipped) {
                ++stats.skipped;
            } else {
                ++stats.failed;
            }
            ++done;
            done_weight += j.weight;
            if (progress) progress(done, total);
            if (o.weighted_progress) o.weighted_progress(done_weight, total_weight);
        }
    };

    if (total > 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        size_t n = o.threads > 0 ? static_cast<size_t>(o.threads) : static_cast<size_t>(hw ? hw : 1);
        n = std::min(n, total);
        std::vector<std::thread> pool;
        pool.reserve(n);
        for (size_t i = 0; i < n; ++i) pool.emplace_back(work);
        for (std::thread& t : pool) t.join();
    }

    if (!disk_error.empty()) fail(Fail::Disk, "%s", disk_error.c_str());
    if (cancel_seen)
        fail(Fail::Cancelled, "The texture cache build was stopped. Finished files are kept; run it again to continue.");

    stats.textures_total = static_cast<u32>(claimed.size()) + stats.failed;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return stats;
}

}  // namespace nxs
