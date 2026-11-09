// SPDX-License-Identifier: GPL-3.0-or-later
// CPU-side texture data preparation (S4d). See nvn_texdata.h for what comes out. No Vulkan calls in this file.
// Libraries: tegra_tiling (untile) and astc2bc7 (ASTC -> BC7 transcode, ASTC -> RGBA8 decode; tools/textures).
// Config: [nvn] bc7_quality (env NXRT_NVN_BC7_QUALITY, default 3), [nvn] bc7_cache (env NXRT_NVN_BC7_CACHE, default
// "bc7_cache" in the current directory, "" disables the cache).
#include "nvn_texdata.h"
#include "nvn_settings.h"
#include "nvn_debug.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#include <nxrt/config.h>
#include <nxrt/diag.h>
#include <nxrt/log.h>

#include "astc2bc7.h"
#include "nx_formats.h"
#include "nvn_format.h"
#include "nvn_texture_layout.h"
#include "tegra_tiling.h"

namespace nxrt::nvn::be {
namespace {

constexpr const char* kTag = "nvn.texdata";
// The cache file format, the hash and the key are formats/nx_formats.h, shared with the setup tool that writes the same files.
constexpr size_t kCacheHeadBytes = nxfmt::kBc7HeadBytes;
constexpr size_t kCacheLevelBytes = nxfmt::kBc7LevelBytes;
using nxfmt::get32;
using nxfmt::get64;
constexpr int kDefaultQuality = 3;

uint64_t div_up(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

std::string cache_dir() { return nvn_settings().bc7_cache; }

int bc7_quality() {
    const int64_t q = nvn_settings().bc7_quality_set ? nvn_settings().bc7_quality : kDefaultQuality;
    if (q < 0 || q > 6) {
        NXLOG_W(kTag, "[nvn] bc7_quality %lld is outside 0..6: using %d", static_cast<long long>(q), kDefaultQuality);
        return kDefaultQuality;
    }
    return static_cast<int>(q);
}

// Writes the header and data of `out` under `key`. Atomic: the file is written under a temporary name and renamed, so a reader
// never sees a partial file and concurrent writers of the same key both leave a complete file. A failure leaves no file.
bool cache_write(const std::string& path, uint64_t key, const PreparedTexture& out) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path final_path(path);
    fs::create_directories(final_path.parent_path(), ec);
    if (ec) return false;

    std::vector<uint8_t> head;
    head.reserve(kCacheHeadBytes + out.level.size() * kCacheLevelBytes);
    std::vector<nxfmt::Bc7Level> levels;
    levels.reserve(out.level.size());
    for (const PreparedLevel& l : out.level) levels.push_back({l.width, l.height, l.depth, l.offset, l.size});
    nxfmt::bc7_put_head(head, key, static_cast<uint32_t>(out.format), out.layers, levels.data(), levels.size());

    static std::atomic<uint64_t> seq{0};
    const std::string tmp_name = path + ".tmp." +
                                 std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + "." +
                                 std::to_string(seq.fetch_add(1));
    const fs::path tmp(tmp_name);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
        f.write(reinterpret_cast<const char*>(out.data.data()), static_cast<std::streamsize>(out.data.size()));
        f.close();
        if (f.fail()) {
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, final_path, ec);
    if (ec) {
        std::error_code ignore;
        fs::remove(tmp, ignore);
        return false;
    }
    return true;
}

// Reads the cache file of `key` into out.data. out.format, out.level and out.layers already hold what the file must describe;
// a missing file is a quiet miss, a file of the wrong size, magic, key or layout (for example a truncated write) is ignored
// with a warning, and the caller transcodes again.
bool cache_read(const std::string& path, uint64_t key, PreparedTexture& out) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    const size_t head_bytes = kCacheHeadBytes + out.level.size() * kCacheLevelBytes;
    const uint64_t data_bytes = out.level.empty() ? 0 : out.level.back().offset + out.level.back().size;
    f.seekg(0, std::ios::end);
    const std::streamoff file_bytes = f.tellg();
    f.seekg(0, std::ios::beg);
    if (file_bytes < 0 || static_cast<uint64_t>(file_bytes) != head_bytes + data_bytes) {
        NXLOG_W(kTag, "cache file %s has the wrong size (truncated?): ignored", path.c_str());
        return false;
    }

    std::vector<uint8_t> head(head_bytes);
    if (!f.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head_bytes))) return false;
    const uint8_t* p = head.data();
    bool ok = nxfmt::bc7_head_matches(p, head.size(), key) &&
              get32(p + 16) == static_cast<uint32_t>(out.format) && get32(p + 20) == out.level.size() &&
              get32(p + 24) == out.layers;
    for (size_t i = 0; ok && i < out.level.size(); ++i) {
        const uint8_t* e = p + kCacheHeadBytes + i * kCacheLevelBytes;
        const PreparedLevel& want = out.level[i];
        ok = get32(e) == want.width && get32(e + 4) == want.height && get32(e + 8) == want.depth &&
             get64(e + 12) == want.offset && get64(e + 20) == want.size;
    }
    if (!ok) {
        NXLOG_W(kTag, "cache file %s does not match its key or the texture: ignored", path.c_str());
        return false;
    }

    out.data.resize(static_cast<size_t>(data_bytes));
    if (!f.read(reinterpret_cast<char*>(out.data.data()), static_cast<std::streamsize>(data_bytes))) {
        out.data.clear();
        return false;
    }
    return true;
}

// One level of the tight linear buffer (the untiled image, or the pitch-linear copy).
struct LevelGeom {
    uint32_t w = 1, h = 1, d = 1;   // texels
    uint32_t bx = 1, by = 1;        // blocks per row and rows, in the source format's blocks
    uint64_t lin_off = 0;           // start of the level in the linear buffer (layer 0)
    uint64_t slice = 0;             // bytes of one layer of the level (all z slices)
};

std::vector<LevelGeom> level_geometry(const TextureBuilderInfo& b, const TexLayout& L) {
    std::vector<LevelGeom> g;
    if (L.block_linear) {
        for (const tegra::LevelLayout& lv : L.layout.levels) {
            LevelGeom e;
            e.w = lv.width;
            e.h = lv.height;
            e.d = lv.depth;
            e.bx = lv.blocks_x;
            e.by = lv.blocks_y;
            e.lin_off = lv.linear_offset;
            e.slice = lv.linear_slice_size;
            g.push_back(e);
        }
    } else {
        // pitch-linear: one level; the 1D targets have height 1 (as compute_texture_layout uses them)
        LevelGeom e;
        e.w = static_cast<uint32_t>(b.width);
        e.h = (b.target == NVN_TEXTURE_TARGET_1D || b.target == NVN_TEXTURE_TARGET_1D_ARRAY) ? 1u : static_cast<uint32_t>(b.height);
        e.d = b.target == NVN_TEXTURE_TARGET_3D ? static_cast<uint32_t>(b.depth) : 1u;
        e.bx = static_cast<uint32_t>(div_up(e.w, L.fmt->block_w));
        e.by = static_cast<uint32_t>(div_up(e.h, L.fmt->block_h));
        e.lin_off = 0;
        e.slice = static_cast<uint64_t>(e.bx) * L.fmt->bytes * e.by * e.d;
        g.push_back(e);
    }
    return g;
}

enum class Mode { Passthrough, Bc7, Rgba8, Bc5 };

}  // namespace

uint64_t hash_bytes(const void* data, size_t size) { return nxfmt::hash_bytes(data, size); }

namespace {

// The cache key of a texture: the hash of the source bytes, seeded with what else decides the output (format, size, layout, quality).
uint64_t cache_key_of(const Texture& t, const uint8_t* mem, const TexLayout& L, const std::vector<LevelGeom>& geo, const FormatInfo& fmt, int quality) {
    const LevelGeom& g0 = geo[0];
    const uint64_t block_height = L.block_linear ? L.layout.levels[0].gob_blocks_y : 0;
    nxfmt::Bc7Meta meta;
    meta.nvn_format = static_cast<uint64_t>(fmt.nvn);
    meta.width = g0.w;
    meta.height = g0.h;
    meta.depth = g0.d;
    meta.levels = L.levels;
    meta.layers = L.layers;
    meta.block_height = block_height;
    meta.pitch = L.pitch;
    meta.quality = static_cast<uint64_t>(quality);
    return nxfmt::bc7_cache_key(mem, static_cast<size_t>(t.storage_size), meta);
}

// The VkFormat a cache file declares (magic, version and key checked), VK_FORMAT_UNDEFINED when there is no valid file.
VkFormat cache_peek_format(const std::string& path, uint64_t key) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return VK_FORMAT_UNDEFINED;
    std::ifstream f(path, std::ios::binary);
    uint8_t head[kCacheHeadBytes];
    if (!f || !f.read(reinterpret_cast<char*>(head), sizeof head)) return VK_FORMAT_UNDEFINED;
    if (!nxfmt::bc7_head_matches(head, sizeof head, key)) return VK_FORMAT_UNDEFINED;
    return static_cast<VkFormat>(get32(head + 16));
}

}  // namespace

std::string bc7_cache_path(const Texture& t, uint64_t hash) {
    (void)t;
    const std::string dir = cache_dir();
    if (dir.empty()) return std::string();
    return (std::filesystem::path(dir) / nxfmt::bc7_cache_rel_path(hash)).string();
}

VkFormat cache_entry_format(const Texture& t) {
    if (!t.mem || cache_dir().empty()) return VK_FORMAT_UNDEFINED;
    const TexLayout L = compute_texture_layout(t.b);
    if (!L.valid || !L.fmt || !L.fmt->astc || t.b.samples > 1 || t.storage_size < L.storage_size) return VK_FORMAT_UNDEFINED;
    const std::vector<LevelGeom> geo = level_geometry(t.b, L);
    if (geo.size() != L.levels) return VK_FORMAT_UNDEFINED;
    const uint64_t key = cache_key_of(t, t.mem, L, geo, *L.fmt, bc7_quality());
    const VkFormat f = cache_peek_format(bc7_cache_path(t, key), key);
    const VkFormat rgba = L.fmt->srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    return (f == rgba || (f == VK_FORMAT_BC5_UNORM_BLOCK && !L.fmt->srgb)) ? f : VK_FORMAT_UNDEFINED;
}

bool prepare_texture(const Texture& t, const TexCaps& caps, PreparedTexture& out) {
    out = PreparedTexture();
    const auto fail = [&out, &t](std::string msg) {
        NXLOG_W(kTag, "texture %u: %s", t.id, msg.c_str());
        out.error = std::move(msg);
        return false;
    };

    // The source bytes: the texture's storage in the game's memory, or for a sparse texture the tiles mapped into its virtual pool.
    const uint8_t* src_mem = t.mem;
    std::vector<uint8_t> gathered;
    Ref<MemoryPool> direct_keep;
    if (!src_mem && t.vpool) {
        if (const uint8_t* d = texture_virtual_direct(t, direct_keep)) src_mem = d;      // one piece of one pool: untile straight from it
        else if (texture_gather_virtual(t, gathered)) src_mem = gathered.data();
    }
    if (!src_mem) return fail("no storage (mem is null)");
    const TexLayout L = compute_texture_layout(t.b);
    if (!L.valid || !L.fmt) return fail("layout is invalid (format, size, target or layout)");
    if (t.b.samples > 1) return fail("multisample textures are not supported");
    if (t.storage_size < L.storage_size) return fail("storage_size is smaller than the layout needs");
    const FormatInfo& fmt = *L.fmt;

    const std::vector<LevelGeom> geo = level_geometry(t.b, L);
    if (geo.size() != L.levels) return fail("internal: level count mismatch");

    Mode mode = Mode::Passthrough;
    std::string cache_file;   // empty: cache off (or not a BC7 output)
    uint64_t key = 0;
    const VkFormat rgba8_format = fmt.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    bool entry_applies = false;   // the cache file of the key holds the output format the mode needs
    VkFormat entry_format = VK_FORMAT_UNDEFINED;   // the format a valid cache file of the key declares
    if (fmt.astc) {
        if (caps.astc) mode = Mode::Passthrough;
        else if (caps.bc7_transcode) mode = Mode::Bc7;
        else mode = Mode::Rgba8;
        if (!caps.astc && !cache_dir().empty()) {
            // A cache entry may hold the texture as BC7 (the default), as RGBA8 (a texture BC7 cannot reproduce well) or as BC5 (a two-channel
            // normal map: blue 0 and alpha 1 on sampling). The image the data goes into is made in the entry's format (caps.image_format).
            key = cache_key_of(t, src_mem, L, geo, fmt, bc7_quality());
            cache_file = bc7_cache_path(t, key);
            const VkFormat ef = cache_peek_format(cache_file, key);
            entry_format = ef;
            const VkFormat bc7_format = fmt.srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
            const VkFormat bc5_format = VK_FORMAT_BC5_UNORM_BLOCK;
            const VkFormat want = caps.image_format;   // UNDEFINED: whatever the entry has
            if (ef == rgba8_format && (want == VK_FORMAT_UNDEFINED || want == rgba8_format)) { mode = Mode::Rgba8; entry_applies = true; }
            else if (ef == bc5_format && !fmt.srgb && (want == VK_FORMAT_UNDEFINED || want == bc5_format)) { mode = Mode::Bc5; entry_applies = true; }
            else if (ef == bc7_format && (want == VK_FORMAT_UNDEFINED || want == bc7_format)) { mode = Mode::Bc7; entry_applies = true; }
            else if (want == rgba8_format) mode = Mode::Rgba8;     // an RGBA8 image and no entry for it: decode the ASTC
            else if (want == bc5_format) return fail("the image is BC5 but its cache entry is gone");
            else mode = Mode::Bc7;
        }
    } else if (fmt.bc) {
        if (!caps.bc) return fail(std::string("BCn format ") + fmt.name + ", and the GPU does not sample BCn");
    } else if (fmt.compressed) {
        return fail(std::string("compressed format ") + fmt.name + " is neither BCn nor ASTC");
    }
    if (mode == Mode::Passthrough && fmt.vk == VK_FORMAT_UNDEFINED)
        return fail(std::string("format ") + fmt.name + " has no Vulkan format");

    switch (mode) {
        case Mode::Passthrough:
            out.format = fmt.vk;
            out.block_w = fmt.block_w;
            out.block_h = fmt.block_h;
            out.block_bytes = fmt.bytes;
            break;
        case Mode::Bc7:
            out.format = fmt.srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
            out.block_w = 4;
            out.block_h = 4;
            out.block_bytes = 16;
            break;
        case Mode::Bc5:
            out.format = VK_FORMAT_BC5_UNORM_BLOCK;
            out.block_w = 4;
            out.block_h = 4;
            out.block_bytes = 16;
            break;
        case Mode::Rgba8:
            out.format = fmt.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
            out.block_w = 1;
            out.block_h = 1;
            out.block_bytes = 4;
            break;
    }
    out.transcoded = mode != Mode::Passthrough;
    out.levels = L.levels;
    out.layers = L.layers;

    out.level.reserve(geo.size());
    for (const LevelGeom& g : geo) {
        PreparedLevel pl{};
        pl.width = g.w;
        pl.height = g.h;
        pl.depth = g.d;
        uint64_t layer_bytes = 0;
        switch (mode) {
            case Mode::Passthrough:
                pl.row_blocks = g.bx;
                pl.offset = g.lin_off;
                layer_bytes = g.slice;
                break;
            case Mode::Bc7:
            case Mode::Bc5:
                pl.row_blocks = static_cast<uint32_t>(div_up(g.w, 4));
                layer_bytes = static_cast<uint64_t>(pl.row_blocks) * div_up(g.h, 4) * 16 * g.d;
                pl.offset = out.level.empty() ? 0 : out.level.back().offset + out.level.back().size;
                break;
            case Mode::Rgba8:
                pl.row_blocks = g.w;
                layer_bytes = static_cast<uint64_t>(g.w) * g.h * 4 * g.d;
                pl.offset = out.level.empty() ? 0 : out.level.back().offset + out.level.back().size;
                break;
        }
        pl.size = layer_bytes * L.layers;
        out.level.push_back(pl);
    }
    const uint64_t out_total = out.level.back().offset + out.level.back().size;

    // Debug: [nvn] dump_astc = DIR writes level 0 of every ASTC texture of width [nvn] dump_astc_w (default any) as tight ASTC blocks to
    // DIR/astc_<key>_<w>x<h>_<bw>x<bh>_<srgb>.bin, to decode them with another decoder.
    {
        const std::string& dump_dir = nvn_debug().dump_astc;
        if (!dump_dir.empty() && fmt.astc && L.block_linear) {
            const int64_t want_w = nvn_debug().dump_astc_w;
            if (want_w == 0 || want_w == static_cast<int64_t>(geo[0].w)) {
                char nm[160];
                std::snprintf(nm, sizeof nm, "/astc_t%u_%016llx_%ux%u_%ux%u_%d.bin", t.id, static_cast<unsigned long long>(key), geo[0].w, geo[0].h,
                              fmt.block_w, fmt.block_h, fmt.srgb ? 1 : 0);
                const std::string path = dump_dir + nm;
                if (!std::filesystem::exists(path)) {
                    const LevelGeom& lg = geo.back();
                    std::vector<uint8_t> dl(static_cast<size_t>(lg.lin_off + lg.slice * L.layers));
                    if (tegra::untile(L.desc, L.layout, src_mem, dl.data())) {
                        std::ofstream f(path, std::ios::binary);
                        f.write(reinterpret_cast<const char*>(dl.data() + geo[0].lin_off), static_cast<std::streamsize>(geo[0].slice));
                        NXLOG_I(kTag, "texture %u: ASTC level 0 written to %s", t.id, path.c_str());
                    }
                }
            }
        }
    }

    TexEditKey edit;
    bool edit_make = false;
    if (fmt.astc && (mode == Mode::Bc7 || mode == Mode::Rgba8) && L.layers == 1 && geo[0].d == 1 && t.b.target == NVN_TEXTURE_TARGET_2D &&
        tex_edit_active()) {
        edit = tex_edit_match(t, src_mem);
        if (edit.id) {
            std::vector<uint8_t> kept;
            if (tex_edit_lookup(edit, out.format, kept) && kept.size() == out_total) {
                out.data = std::move(kept);
                tex_edit_note(t);
                NXLOG_I(kTag, "texture %u: edited data (host id %016llx) reused", t.id, static_cast<unsigned long long>(edit.id));
                return true;
            }
            edit_make = true;
        }
    }
    struct EditGuard {   // a failure below gives up the data this call was to make, so a waiting thread makes it itself
        const TexEditKey* k = nullptr;
        VkFormat f = VK_FORMAT_UNDEFINED;
        ~EditGuard() {
            if (k) tex_edit_abandon(*k, f);
        }
    } edit_guard;
    if (edit_make) {
        edit_guard.k = &edit;
        edit_guard.f = out.format;
    }

    const bool bc7 = mode == Mode::Bc7;
    const int quality = bc7 ? bc7_quality() : 0;
    (void)quality;
    if (entry_applies && !edit_make) {
        if (cache_read(cache_file, key, out)) {
            out.from_cache = true;
            NXLOG_I(kTag, "texture %u: %s data from cache %s", t.id, mode == Mode::Bc5 ? "BC5" : mode == Mode::Rgba8 ? "RGBA8" : "BC7", cache_file.c_str());
            return true;
        }
        if (mode == Mode::Bc5) return fail("the BC5 cache entry could not be read");
    }
    if (caps.cache_only && mode != Mode::Passthrough && !edit_make) {   // background job: a transcode is a long CPU job on data that may not be final; the render thread does it if it is needed
        out.error = "no cache entry";
        return false;
    }
    // only BC7 results are written to the cache, and never over a valid entry of another format (RGBA8 / BC5 entries come from the setup tool)
    if (!bc7 || (entry_format != VK_FORMAT_UNDEFINED && entry_format != out.format)) cache_file.clear();
    if (edit_make) cache_file.clear();   // edited data stays out of the shared cache

    const LevelGeom& last = geo.back();
    const uint64_t lin_total = last.lin_off + last.slice * L.layers;
    std::vector<uint8_t> lin(static_cast<size_t>(lin_total));
    if (L.block_linear) {
        if (!tegra::untile(L.desc, L.layout, src_mem, lin.data())) return fail("untile failed");
    } else {
        const LevelGeom& g = geo[0];
        const uint64_t row = static_cast<uint64_t>(g.bx) * fmt.bytes;
        for (uint32_t layer = 0; layer < L.layers; ++layer)
            for (uint32_t z = 0; z < g.d; ++z)
                for (uint32_t y = 0; y < g.by; ++y) {
                    const uint64_t r = static_cast<uint64_t>(z) * g.by + y;
                    const uint8_t* src = src_mem + static_cast<uint64_t>(layer) * (L.pitch * g.by * g.d) + r * L.pitch;
                    std::memcpy(lin.data() + static_cast<uint64_t>(layer) * g.slice + r * row, src, static_cast<size_t>(row));
                }
    }

    if (mode == Mode::Passthrough) {
        // button prompt icons and the like: an upload subscriber may replace the pixels of a small single-level BC7 sRGB texture
        if (fmt.vk == VK_FORMAT_BC7_SRGB_BLOCK && L.levels == 1 && L.layers == 1 && geo[0].d == 1 && geo[0].w <= 128 &&
            geo[0].h <= 128 && tex_edit_active() && tex_upload_bc7(t, geo[0].w, geo[0].h, lin))
            NXLOG_I(kTag, "texture %u: content replaced by the host (%ux%u BC7)", t.id, geo[0].w, geo[0].h);
        out.data = std::move(lin);
        return true;
    }

    out.data.assign(static_cast<size_t>(out_total), 0);
    a2b_options opt{};
    opt.quality = quality;
    opt.threads = 0;
    opt.perceptual = 0;
    opt.allow_opaque_mode6 = 0;
    a2b_input in{};
    in.format = fmt.srgb ? A2B_ASTC_SRGB : A2B_ASTC_UNORM;
    in.block_w = fmt.block_w;
    in.block_h = fmt.block_h;

    const auto t0 = std::chrono::steady_clock::now();
    if (edit_make) {
        // decode each level to RGBA8, let the host edit it, encode it again (one layer, one slice: checked above)
        a2b_options eopt = opt;
        eopt.quality = tex_edit_quality();
        std::vector<uint8_t> rgba;
        for (size_t i = 0; i < geo.size(); ++i) {
            const LevelGeom& g = geo[i];
            const PreparedLevel& pl = out.level[i];
            in.width = g.w;
            in.height = g.h;
            uint8_t* dst = out.data.data() + pl.offset;
            uint8_t* px = dst;
            if (bc7) {
                rgba.resize(static_cast<size_t>(g.w) * g.h * 4);
                px = rgba.data();
            }
            a2b_status st = a2b_decode_astc_rgba8(&in, lin.data() + g.lin_off, static_cast<size_t>(g.slice), 0, px, nullptr);
            if (st != A2B_OK) return fail(std::string("ASTC decode for the edit failed: ") + a2b_status_string(st));
            (void)tex_edit_level(edit.id, static_cast<uint32_t>(i), g.w, g.h, px);   // false: the level stays as decoded
            if (bc7) {
                st = a2b_encode_bc7(g.w, g.h, px, &eopt, dst, static_cast<size_t>(pl.size), nullptr);
                if (st != A2B_OK) return fail(std::string("BC7 encode of the edit failed: ") + a2b_status_string(st));
            }
            diag_progress();
        }
        const double ms = ms_since(t0);
        tex_edit_store(edit, out.format, out.data, ms);
        edit_guard.k = nullptr;
        tex_edit_note(t);
        NXLOG_I(kTag, "texture %u: edited by the host (id %016llx), %s %ux%u, %u levels: %.1f ms", t.id, static_cast<unsigned long long>(edit.id),
                bc7 ? "BC7" : "RGBA8", geo[0].w, geo[0].h, L.levels, ms);
        return true;
    }
    for (size_t i = 0; i < geo.size(); ++i) {
        const LevelGeom& g = geo[i];
        const PreparedLevel& pl = out.level[i];
        in.width = g.w;
        in.height = g.h;
        const uint64_t out_slice = pl.size / L.layers / g.d;   // bytes of one z slice of the output
        const uint64_t in_slice = g.slice / g.d;               // bytes of one z slice of the ASTC data (tight rows)
        if (bc7 && a2b_output_size(&in) != out_slice) return fail("internal: BC7 slice size mismatch");
        for (uint32_t layer = 0; layer < L.layers; ++layer)
            for (uint32_t z = 0; z < g.d; ++z) {
                const uint8_t* src = lin.data() + g.lin_off + static_cast<uint64_t>(layer) * g.slice + z * in_slice;
                uint8_t* dst = out.data.data() + pl.offset + (static_cast<uint64_t>(layer) * g.d + z) * out_slice;
                a2b_status st;
                if (bc7) {
                    a2b_format of;
                    st = a2b_transcode(&in, src, static_cast<size_t>(in_slice), &opt, &of, dst, static_cast<size_t>(out_slice),
                                       nullptr);
                } else {
                    st = a2b_decode_astc_rgba8(&in, src, static_cast<size_t>(in_slice), 0, dst, nullptr);
                }
                if (st != A2B_OK)
                    return fail(std::string("ASTC ") + (bc7 ? "transcode" : "decode") + " failed: " + a2b_status_string(st));
                diag_progress();   // a cold cache transcodes on the render thread for minutes: slow, not frozen (debug note 421)
            }
    }
    NXLOG_I(kTag, "texture %u: %s %ux%u, %u levels, %u layers: %.1f ms", t.id, bc7 ? "ASTC -> BC7" : "ASTC -> RGBA8",
            geo[0].w, geo[0].h, L.levels, L.layers, ms_since(t0));

    if (!cache_file.empty() && !cache_write(cache_file, key, out))
        NXLOG_W(kTag, "texture %u: could not write cache file %s", t.id, cache_file.c_str());
    return true;
}

}  // namespace nxrt::nvn::be
