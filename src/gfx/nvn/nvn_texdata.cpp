// SPDX-License-Identifier: GPL-3.0-or-later
// CPU-side texture data preparation (stream S4d). See nvn_texdata.h for what comes out. No Vulkan calls in this file.
// Libraries: tegra_tiling (untile) and astc2bc7 (ASTC -> BC7 transcode, ASTC -> RGBA8 decode; tools/textures).
// Config: [nvn] bc7_quality (env NXRT_NVN_BC7_QUALITY, default 3), [nvn] bc7_cache (env NXRT_NVN_BC7_CACHE, default
// "bc7_cache" in the current directory, "" disables the cache).
#include "nvn_texdata.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#include <nxrt/config.h>
#include <nxrt/log.h>

#include "astc2bc7.h"
#include "nvn_format.h"
#include "nvn_texture_layout.h"
#include "tegra_tiling.h"

namespace nxrt::nvn::be {
namespace {

constexpr const char* kTag = "nvn.texdata";
constexpr uint32_t kCacheVersion = 1;
constexpr uint8_t kCacheMagic[4] = {'N', 'X', 'B', '7'};
constexpr size_t kCacheHeadBytes = 4 + 4 + 8 + 4 + 4 + 4;   // magic, version, key hash, VkFormat, level count, layer count
constexpr size_t kCacheLevelBytes = 3 * 4 + 2 * 8;          // per level: width, height, depth, offset, size
constexpr int kDefaultQuality = 3;

uint64_t div_up(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// ---- hash -----------------------------------------------------------------------------------------------------------------
// xxHash64-style mix: four lanes over 32-byte stripes, then a final avalanche. Not byte compatible with xxHash; it only has
// to be fast and to separate textures. The cache also compares its header, so a collision cannot return wrong data silently.
constexpr uint64_t kP1 = 0x9E3779B185EBCA87ull, kP2 = 0xC2B2AE3D27D4EB4Full, kP3 = 0x165667B19E3779F9ull,
                   kP4 = 0x85EBCA77C2B2AE63ull, kP5 = 0x27D4EB2F165667C5ull;

inline uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
inline uint64_t read64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline uint32_t read32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint64_t lane(uint64_t acc, uint64_t in) { acc += in * kP2; acc = rotl64(acc, 31); return acc * kP1; }
inline uint64_t merge_lane(uint64_t h, uint64_t v) { h ^= lane(0, v); return h * kP1 + kP4; }

uint64_t hash_seeded(const uint8_t* p, size_t n, uint64_t seed) {
    const uint8_t* const end = p + n;
    uint64_t h;
    if (n >= 32) {
        uint64_t v1 = seed + kP1 + kP2, v2 = seed + kP2, v3 = seed, v4 = seed - kP1;
        do {
            v1 = lane(v1, read64(p));
            v2 = lane(v2, read64(p + 8));
            v3 = lane(v3, read64(p + 16));
            v4 = lane(v4, read64(p + 24));
            p += 32;
        } while (end - p >= 32);
        h = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        h = merge_lane(h, v1);
        h = merge_lane(h, v2);
        h = merge_lane(h, v3);
        h = merge_lane(h, v4);
    } else {
        h = seed + kP5;
    }
    h += n;
    while (end - p >= 8) {
        h ^= lane(0, read64(p));
        h = rotl64(h, 27) * kP1 + kP4;
        p += 8;
    }
    if (end - p >= 4) {
        h ^= static_cast<uint64_t>(read32(p)) * kP1;
        h = rotl64(h, 23) * kP2 + kP3;
        p += 4;
    }
    while (p < end) {
        h ^= static_cast<uint64_t>(*p) * kP5;
        h = rotl64(h, 11) * kP1;
        ++p;
    }
    h ^= h >> 33;
    h *= kP2;
    h ^= h >> 29;
    h *= kP3;
    h ^= h >> 32;
    return h;
}

// ---- little-endian helpers for the cache file ------------------------------------------------------------------------------
void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
void put64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
uint32_t get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t get64(const uint8_t* p) { return static_cast<uint64_t>(get32(p)) | (static_cast<uint64_t>(get32(p + 4)) << 32); }

std::string cache_dir() { return config_get_str("nvn", "bc7_cache", "bc7_cache"); }

int bc7_quality() {
    const int64_t q = config_get_int("nvn", "bc7_quality", kDefaultQuality);
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
    head.insert(head.end(), kCacheMagic, kCacheMagic + 4);
    put32(head, kCacheVersion);
    put64(head, key);
    put32(head, static_cast<uint32_t>(out.format));
    put32(head, static_cast<uint32_t>(out.level.size()));
    put32(head, out.layers);
    for (const PreparedLevel& l : out.level) {
        put32(head, l.width);
        put32(head, l.height);
        put32(head, l.depth);
        put64(head, l.offset);
        put64(head, l.size);
    }

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
    bool ok = std::memcmp(p, kCacheMagic, 4) == 0 && get32(p + 4) == kCacheVersion && get64(p + 8) == key &&
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

// ---- level geometry --------------------------------------------------------------------------------------------------------
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

enum class Mode { Passthrough, Bc7, Rgba8 };

}  // namespace

uint64_t hash_bytes(const void* data, size_t size) {
    return hash_seeded(static_cast<const uint8_t*>(data), size, 0);
}

std::string bc7_cache_path(const Texture& t, uint64_t hash) {
    (void)t;
    const std::string dir = cache_dir();
    if (dir.empty()) return std::string();
    char name[32];
    std::snprintf(name, sizeof name, "%016llx", static_cast<unsigned long long>(hash));
    const std::filesystem::path p = std::filesystem::path(dir) / std::string(name, 2) / (std::string(name) + ".bc7");
    return p.string();
}

bool prepare_texture(const Texture& t, const TexCaps& caps, PreparedTexture& out) {
    out = PreparedTexture();
    const auto fail = [&out, &t](std::string msg) {
        NXLOG_W(kTag, "texture %u: %s", t.id, msg.c_str());
        out.error = std::move(msg);
        return false;
    };

    if (!t.mem) return fail("no storage (mem is null)");
    const TexLayout L = compute_texture_layout(t.b);
    if (!L.valid || !L.fmt) return fail("layout is invalid (format, size, target or layout)");
    if (t.b.samples > 1) return fail("multisample textures are not supported");
    if (t.storage_size < L.storage_size) return fail("storage_size is smaller than the layout needs");
    const FormatInfo& fmt = *L.fmt;

    // ---- what the GPU gets ------------------------------------------------------------------------------------------------
    Mode mode = Mode::Passthrough;
    if (fmt.astc) {
        if (caps.astc) mode = Mode::Passthrough;
        else if (caps.bc7_transcode) mode = Mode::Bc7;
        else mode = Mode::Rgba8;
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

    // ---- output level table ---------------------------------------------------------------------------------------------
    const std::vector<LevelGeom> geo = level_geometry(t.b, L);
    if (geo.size() != L.levels) return fail("internal: level count mismatch");
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

    // ---- disk cache (BC7 only) -------------------------------------------------------------------------------------------
    const bool bc7 = mode == Mode::Bc7;
    const int quality = bc7 ? bc7_quality() : 0;
    std::string cache_file;   // empty: cache off (or not a BC7 output)
    uint64_t key = 0;
    if (bc7 && !cache_dir().empty()) {
        // key = hash of the source bytes, seeded with what else decides the output: format, size, layout and quality
        const LevelGeom& g0 = geo[0];
        const uint64_t block_height = L.block_linear ? L.layout.levels[0].gob_blocks_y : 0;
        const std::vector<uint64_t> meta = {kCacheVersion,
                                            static_cast<uint64_t>(fmt.nvn),
                                            g0.w,
                                            g0.h,
                                            g0.d,
                                            L.levels,
                                            L.layers,
                                            block_height,
                                            L.pitch,
                                            static_cast<uint64_t>(quality)};
        const uint64_t meta_hash = hash_bytes(meta.data(), meta.size() * sizeof(uint64_t));
        key = hash_seeded(t.mem, static_cast<size_t>(t.storage_size), meta_hash);
        cache_file = bc7_cache_path(t, key);
        if (cache_read(cache_file, key, out)) {
            out.from_cache = true;
            NXLOG_I(kTag, "texture %u: BC7 data from cache %s", t.id, cache_file.c_str());
            return true;
        }
    }

    // ---- the source image: tight linear buffer --------------------------------------------------------------------------
    const LevelGeom& last = geo.back();
    const uint64_t lin_total = last.lin_off + last.slice * L.layers;
    std::vector<uint8_t> lin(static_cast<size_t>(lin_total));
    if (L.block_linear) {
        if (!tegra::untile(L.desc, L.layout, t.mem, lin.data())) return fail("untile failed");
    } else {
        const LevelGeom& g = geo[0];
        const uint64_t row = static_cast<uint64_t>(g.bx) * fmt.bytes;
        for (uint32_t layer = 0; layer < L.layers; ++layer)
            for (uint32_t z = 0; z < g.d; ++z)
                for (uint32_t y = 0; y < g.by; ++y) {
                    const uint64_t r = static_cast<uint64_t>(z) * g.by + y;   // row index in the level
                    const uint8_t* src = t.mem + static_cast<uint64_t>(layer) * (L.pitch * g.by * g.d) + r * L.pitch;
                    std::memcpy(lin.data() + static_cast<uint64_t>(layer) * g.slice + r * row, src, static_cast<size_t>(row));
                }
    }

    if (mode == Mode::Passthrough) {
        // button prompt icons and the like: a host hook may replace the pixels of a small single-level BC7 sRGB texture
        if (fmt.vk == VK_FORMAT_BC7_SRGB_BLOCK && L.levels == 1 && L.layers == 1 && geo[0].d == 1 && geo[0].w <= 128 &&
            geo[0].h <= 128 && texture_override_active() && texture_override_apply(t, geo[0].w, geo[0].h, lin))
            NXLOG_I(kTag, "texture %u: content replaced by the host (%ux%u BC7)", t.id, geo[0].w, geo[0].h);
        out.data = std::move(lin);
        return true;
    }

    // ---- transcode or decode, one 2D slice at a time (the library threads inside a slice) ------------------------------
    out.data.assign(static_cast<size_t>(out_total), 0);
    a2b_options opt{};
    opt.quality = quality;
    opt.threads = 0;             // all hardware threads
    opt.perceptual = 0;
    opt.allow_opaque_mode6 = 0;
    a2b_input in{};
    in.format = fmt.srgb ? A2B_ASTC_SRGB : A2B_ASTC_UNORM;
    in.block_w = fmt.block_w;
    in.block_h = fmt.block_h;

    const auto t0 = std::chrono::steady_clock::now();
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
            }
    }
    NXLOG_I(kTag, "texture %u: %s %ux%u, %u levels, %u layers: %.1f ms", t.id, bc7 ? "ASTC -> BC7" : "ASTC -> RGBA8",
            geo[0].w, geo[0].h, L.levels, L.layers, ms_since(t0));

    if (!cache_file.empty() && !cache_write(cache_file, key, out))
        NXLOG_W(kTag, "texture %u: could not write cache file %s", t.id, cache_file.c_str());
    return true;
}

}  // namespace nxrt::nvn::be
