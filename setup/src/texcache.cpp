// SPDX-License-Identifier: GPL-3.0-or-later
// BC7 texture cache builder. Port of the runtime's CPU-side texture preparation (runtime/src/gfx/nvn/nvn_texdata.cpp:
// hash_seeded, cache_write / cache_read, prepare_texture's Mode::Bc7 branch) and of nvn_texture_layout.cpp (layout rules for
// block-linear 2D, cube, 2D array and 3D). See texcache.h for the contract. Libraries: tegra_tiling (untile) and astc2bc7
// (ASTC -> BC7 transcode).
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

#include <cwchar>

#include "astc2bc7.h"
#include "fileio.h"
#include "log.h"
#include "pak.h"
#include "storage.h"
#include "tegra_tiling.h"
#include "txtr.h"

namespace mprs {
namespace {

// ---- constants of the runtime (nvn_texdata.cpp) -----------------------------------------------------------------------------
constexpr u32 kCacheVersion = 1;
constexpr u8 kCacheMagic[4] = {'N', 'X', 'B', '7'};
constexpr size_t kCacheHeadBytes = 4 + 4 + 8 + 4 + 4 + 4;   // magic, version, key, VkFormat, level count, layer count
constexpr size_t kCacheLevelBytes = 3 * 4 + 2 * 8;          // per level: width, height, depth, offset, size
constexpr u32 kVkBc7Unorm = 145;                            // VK_FORMAT_BC7_UNORM_BLOCK
constexpr u32 kVkBc7Srgb = 146;                             // VK_FORMAT_BC7_SRGB_BLOCK

// ---- hash (nvn_texdata.cpp, xxHash64-style mix; must stay bit-identical) -----------------------------------------------------
constexpr u64 kP1 = 0x9E3779B185EBCA87ull, kP2 = 0xC2B2AE3D27D4EB4Full, kP3 = 0x165667B19E3779F9ull,
              kP4 = 0x85EBCA77C2B2AE63ull, kP5 = 0x27D4EB2F165667C5ull;

inline u64 rotl64(u64 x, int r) { return (x << r) | (x >> (64 - r)); }
inline u64 read64(const u8* p) { u64 v; std::memcpy(&v, p, 8); return v; }
inline u32 read32(const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; }
inline u64 lane(u64 acc, u64 in) { acc += in * kP2; acc = rotl64(acc, 31); return acc * kP1; }
inline u64 merge_lane(u64 h, u64 v) { h ^= lane(0, v); return h * kP1 + kP4; }

u64 hash_seeded_impl(const u8* p, size_t n, u64 seed) {
    const u8* const end = p + n;
    u64 h;
    if (n >= 32) {
        u64 v1 = seed + kP1 + kP2, v2 = seed + kP2, v3 = seed, v4 = seed - kP1;
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
        h ^= static_cast<u64>(read32(p)) * kP1;
        h = rotl64(h, 23) * kP2 + kP3;
        p += 4;
    }
    while (p < end) {
        h ^= static_cast<u64>(*p) * kP5;
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

// ---- little-endian helpers for the cache file -----------------------------------------------------------------------------
void put32(std::vector<u8>& v, u32 x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<u8>(x >> (8 * i)));
}
void put64(std::vector<u8>& v, u64 x) {
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<u8>(x >> (8 * i)));
}
u32 get32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}
u64 get64(const u8* p) { return static_cast<u64>(get32(p)) | (static_cast<u64>(get32(p + 4)) << 32); }

u64 div_up(u64 a, u64 b) { return (a + b - 1) / b; }

// ---- NVN format enum of the 28 ASTC LDR formats (nvn.h: NVN_FORMAT_RGBA_ASTC_<w>x<h>[_SRGB]) --------------------------------
struct AstcEnum {
    u32 w, h;
    bool srgb;
    u32 nvn;
};
constexpr AstcEnum kAstcEnum[] = {
    {4, 4, false, 0x79},   {5, 4, false, 0x7A},   {5, 5, false, 0x7B},   {6, 5, false, 0x7C},
    {6, 6, false, 0x7D},   {8, 5, false, 0x7E},   {8, 6, false, 0x7F},   {8, 8, false, 0x80},
    {10, 5, false, 0x81},  {10, 6, false, 0x82},  {10, 8, false, 0x83},  {10, 10, false, 0x84},
    {12, 10, false, 0x85}, {12, 12, false, 0x86},
    {4, 4, true, 0x87},    {5, 4, true, 0x88},    {5, 5, true, 0x89},    {6, 5, true, 0x8A},
    {6, 6, true, 0x8B},    {8, 5, true, 0x8C},    {8, 6, true, 0x8D},    {8, 8, true, 0x8E},
    {10, 5, true, 0x8F},   {10, 6, true, 0x90},   {10, 8, true, 0x91},   {10, 10, true, 0x92},
    {12, 10, true, 0x93},  {12, 12, true, 0x94},
};

// ---- layout (nvn_texture_layout.cpp, block-linear case) ----------------------------------------------------------------------
struct CacheLayout {
    tegra::TextureDesc desc;
    tegra::Layout layout;
    u32 levels = 0, layers = 0;
    u64 storage = 0;   // layout.tiled_layer_stride * layers: the bytes the runtime hashes and allocates
};

// Builds the layout the runtime builds for this TXTR. Returns nullptr when accepted, else why not.
const char* build_layout(const TxtrInfo& info, const TxtrFormat& f, CacheLayout& out) {
    if (info.kind != TxtrKind::D2 && info.kind != TxtrKind::Cube && info.kind != TxtrKind::D2Array &&
        info.kind != TxtrKind::D3)
        return "texture kind is not cached (only 2D, cube, 2D array and 3D)";
    if (info.width == 0 || info.height == 0) return "width or height is zero";
    const bool is3d = info.kind == TxtrKind::D3;
    const u32 w = info.width, h = info.height;
    const u32 d = is3d ? info.layers : 1;
    if (is3d && d == 0) return "3D texture has depth zero";

    u32 layers = 1;
    if (info.kind == TxtrKind::Cube) {
        layers = 6;
    } else if (info.kind == TxtrKind::D2Array) {
        if (info.layers == 0) return "2D array has zero layers";
        layers = info.layers;
    }

    // levels = min(mips, floor(log2(largest dimension)) + 1), at least 1
    const u32 maxdim = is3d ? std::max({w, h, d}) : std::max(w, h);
    u32 top = 1;
    while (top < 32 && (static_cast<u64>(maxdim) >> top) != 0) ++top;
    const u32 levels = std::min<u32>(std::max<u32>(1, info.mips), top);

    if (f.bytes != 16) return "ASTC block is not 16 bytes";
    tegra::TextureDesc& ds = out.desc;
    ds.width = w;
    ds.height = h;
    ds.depth = d;
    ds.levels = levels;
    ds.layers = layers;   // 3D: the layout requires 1 (the slices are depth)
    if (is3d) ds.layers = 1;
    ds.block_w = f.block_w;
    ds.block_h = f.block_h;
    ds.bytes_per_block = f.bytes;
    ds.linear_stride_align = 1;   // tight rows, as the runtime's untile target
    if (is3d) {
        ds.gob_blocks_y = 1;
        ds.gob_blocks_z = 16;
    } else {
        const u32 blocks_y = static_cast<u32>(div_up(h, f.block_h));
        ds.gob_blocks_y = tegra::default_gob_blocks_y(blocks_y);   // no packaged layout for TXTR textures
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
const char* check_texture(const TxtrInfo& info, size_t tiled_len, int quality, CacheLayout& L, const TxtrFormat*& fmt) {
    if (quality < 0 || quality > 6) return "quality is outside 0..6";
    try {
        fmt = &txtr_format(info.format);
    } catch (const Error&) {
        return "unknown TXTR format id";
    }
    if (!fmt->astc) return "not an ASTC format (BCn and uncompressed textures need no cache entry)";
    if (tex_nvn_astc_format(info.format) == 0) return "no NVN format for this ASTC footprint";
    if (const char* why = build_layout(info, *fmt, L)) return why;
    if (info.kind != TxtrKind::D3 && L.storage != info.gpu_size)
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

// Removes a claimed key again unless the file for it was written (or already existed).
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

}  // namespace

// ---- public -----------------------------------------------------------------------------------------------------------------
u64 tex_hash_bytes(const void* data, size_t len) { return hash_seeded_impl(static_cast<const u8*>(data), len, 0); }

u64 tex_hash_seeded(const void* data, size_t len, u64 seed) {
    return hash_seeded_impl(static_cast<const u8*>(data), len, seed);
}

int tex_nvn_astc_format(u32 txtr_format_id) {
    const TxtrFormat* f = nullptr;
    try {
        f = &txtr_format(txtr_format_id);
    } catch (const Error&) {
        return 0;
    }
    if (!f->astc) return 0;
    for (const AstcEnum& e : kAstcEnum)
        if (e.w == f->block_w && e.h == f->block_h && e.srgb == f->srgb) return static_cast<int>(e.nvn);
    return 0;
}

namespace detail {

// Why texcache_plan refuses this texture (nullptr when it accepts it). Used by the build log and the test.
const char* texcache_reject_reason(const TxtrInfo& info, size_t tiled_len, int quality) {
    CacheLayout L;
    const TxtrFormat* f = nullptr;
    return check_texture(info, tiled_len, quality, L, f);
}

}  // namespace detail

TexCachePlan texcache_plan(const TxtrInfo& info, const u8* tiled, size_t tiled_len, int quality) {
    TexCachePlan p;
    if (!tiled) return p;
    CacheLayout L;
    const TxtrFormat* f = nullptr;
    if (check_texture(info, tiled_len, quality, L, f) != nullptr) return p;

    // key = hash_seeded(tiled bytes, storage, seed = hash_bytes(meta)), meta = ten u64 as in prepare_texture
    const tegra::LevelLayout& l0 = L.layout.levels[0];
    const u64 meta[10] = {kCacheVersion,
                          static_cast<u64>(tex_nvn_astc_format(info.format)),
                          l0.width,
                          l0.height,
                          l0.depth,
                          L.levels,
                          L.layers,
                          l0.gob_blocks_y,
                          0,   // pitch: block-linear
                          static_cast<u64>(quality)};
    const u64 meta_hash = tex_hash_bytes(meta, sizeof meta);
    p.key = tex_hash_seeded(tiled, static_cast<size_t>(L.storage), meta_hash);
    p.vk_format = f->srgb ? kVkBc7Srgb : kVkBc7Unorm;
    p.levels = L.levels;
    p.layers = L.layers;
    fill_levels(L, p.level, p.data_bytes);
    p.file_bytes = kCacheHeadBytes + p.level.size() * kCacheLevelBytes + p.data_bytes;
    p.ok = true;
    return p;
}

std::string texcache_path(const std::string& cache_dir_utf8, u64 key) {
    char name[32];
    std::snprintf(name, sizeof name, "%016llx", static_cast<unsigned long long>(key));
    const std::filesystem::path dir(widen(cache_dir_utf8));
    const std::filesystem::path p = dir / std::string(name, 2) / (std::string(name) + ".bc7");
    return narrow(p.wstring());
}

std::vector<u8> texcache_build_file(const TxtrInfo& info, const TexCachePlan& plan, const u8* tiled, size_t tiled_len,
                                    int quality) {
    if (!plan.ok) fail(Fail::Generic, "Texture %s: the cache plan was rejected, nothing to build.", info.id.str().c_str());
    CacheLayout L;
    const TxtrFormat* f = nullptr;
    if (const char* why = check_texture(info, tiled_len, quality, L, f))
        fail(Fail::Generic, "Texture %s cannot be built: %s.", info.id.str().c_str(), why);
    std::vector<TexOutLevel> levels;
    u64 data_bytes = 0;
    fill_levels(L, levels, data_bytes);
    if (levels.size() != plan.level.size() || data_bytes != plan.data_bytes || L.levels != plan.levels ||
        L.layers != plan.layers)
        fail(Fail::Generic, "Texture %s: the plan does not match the texture.", info.id.str().c_str());
    for (size_t i = 0; i < levels.size(); ++i) {
        const TexOutLevel& a = levels[i];
        const TexOutLevel& b = plan.level[i];
        if (a.width != b.width || a.height != b.height || a.depth != b.depth || a.offset != b.offset || a.size != b.size)
            fail(Fail::Generic, "Texture %s: the plan does not match the texture.", info.id.str().c_str());
    }
    const u32 vk = f->srgb ? kVkBc7Srgb : kVkBc7Unorm;
    if (vk != plan.vk_format) fail(Fail::Generic, "Texture %s: the plan has the wrong format.", info.id.str().c_str());

    const a2b_status init = ensure_transcoder();
    if (init != A2B_OK)
        fail(Fail::Generic, "The ASTC transcoder is not available (%s).", a2b_status_string(init));

    // untile into a tight linear buffer (the same layout the runtime's untile produces)
    const tegra::LevelLayout& last = L.layout.levels.back();
    const u64 lin_total = last.linear_offset + last.linear_slice_size * L.layers;
    std::vector<u8> lin(static_cast<size_t>(lin_total));
    if (!tegra::untile(L.desc, L.layout, tiled, lin.data()))
        fail(Fail::Generic, "Texture %s: untile failed.", info.id.str().c_str());

    std::vector<u8> data(static_cast<size_t>(data_bytes));
    a2b_options opt{};
    opt.quality = quality;
    opt.threads = 1;             // callers parallelise over textures
    opt.perceptual = 0;
    opt.allow_opaque_mode6 = 0;

    for (size_t i = 0; i < L.layout.levels.size(); ++i) {
        const tegra::LevelLayout& g = L.layout.levels[i];
        const TexOutLevel& out = plan.level[i];
        a2b_input in{};
        in.format = f->srgb ? A2B_ASTC_SRGB : A2B_ASTC_UNORM;
        in.width = g.width;
        in.height = g.height;
        in.block_w = f->block_w;
        in.block_h = f->block_h;
        const u64 out_slice = out.size / L.layers / g.depth;
        const u64 in_slice = g.linear_slice_size / g.depth;
        if (a2b_output_size(&in) != out_slice)
            fail(Fail::Generic, "Texture %s: internal error, BC7 slice size mismatch.", info.id.str().c_str());
        for (u32 layer = 0; layer < L.layers; ++layer) {
            for (u32 z = 0; z < g.depth; ++z) {
                const u8* src = lin.data() + g.linear_offset + static_cast<u64>(layer) * g.linear_slice_size + z * in_slice;
                u8* dst = data.data() + out.offset + (static_cast<u64>(layer) * g.depth + z) * out_slice;
                a2b_format of = A2B_BC7_UNORM;
                const a2b_status st = a2b_transcode(&in, src, static_cast<size_t>(in_slice), &opt, &of, dst,
                                                    static_cast<size_t>(out_slice), nullptr);
                if (st != A2B_OK)
                    fail(Fail::Generic, "Texture %s: ASTC to BC7 transcode failed (%s).", info.id.str().c_str(),
                         a2b_status_string(st));
            }
        }
    }

    // header and level table (cache_write), little endian
    std::vector<u8> out;
    out.reserve(static_cast<size_t>(plan.file_bytes));
    out.insert(out.end(), kCacheMagic, kCacheMagic + 4);
    put32(out, kCacheVersion);
    put64(out, plan.key);
    put32(out, plan.vk_format);
    put32(out, static_cast<u32>(levels.size()));
    put32(out, L.layers);
    for (const TexOutLevel& l : levels) {
        put32(out, l.width);
        put32(out, l.height);
        put32(out, l.depth);
        put64(out, l.offset);
        put64(out, l.size);
    }
    out.insert(out.end(), data.begin(), data.end());
    if (out.size() != plan.file_bytes)
        fail(Fail::Generic, "Texture %s: internal error, file size mismatch.", info.id.str().c_str());
    return out;
}

bool texcache_file_valid(const std::string& path_utf8, const TexCachePlan& plan) {
    if (!plan.ok) return false;
    std::ifstream f(std::filesystem::path(widen(path_utf8)), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    f.seekg(0, std::ios::beg);
    if (size < 0 || static_cast<u64>(size) != plan.file_bytes) return false;

    const size_t nlevels = plan.level.size();
    const size_t head_bytes = kCacheHeadBytes + nlevels * kCacheLevelBytes;
    std::vector<u8> head(head_bytes);
    if (!f.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head_bytes))) return false;
    const u8* p = head.data();
    if (std::memcmp(p, kCacheMagic, 4) != 0) return false;
    if (get32(p + 4) != kCacheVersion) return false;
    if (get64(p + 8) != plan.key) return false;
    if (get32(p + 16) != plan.vk_format) return false;
    if (get32(p + 20) != nlevels) return false;
    if (get32(p + 24) != plan.layers) return false;
    for (size_t i = 0; i < nlevels; ++i) {
        const u8* e = p + kCacheHeadBytes + i * kCacheLevelBytes;
        const TexOutLevel& want = plan.level[i];
        if (get32(e) != want.width || get32(e + 4) != want.height || get32(e + 8) != want.depth ||
            get64(e + 12) != want.offset || get64(e + 20) != want.size)
            return false;
    }
    return true;
}

TexCacheStats build_texture_cache(const TexCacheOptions& o, const std::function<void(u64 done, u64 total)>& progress,
                                  const std::function<bool()>& cancelled) {
    namespace fs = std::filesystem;
    const auto t0 = std::chrono::steady_clock::now();
    TexCacheStats stats;
    if (o.cache_dir.empty()) fail(Fail::Input, "No cache folder was given.");
    if (o.quality < 0 || o.quality > 6) fail(Fail::Generic, "The BC7 quality must be 0 to 6.");
    if (const a2b_status st = ensure_transcoder(); st != A2B_OK)
        fail(Fail::Generic, "The ASTC transcoder is not available (%s).", a2b_status_string(st));

    // 1. the .pak files under the RomFS folder, in a fixed order
    std::error_code ec;
    const fs::path root(widen(o.romfs_dir));
    if (!fs::is_directory(root, ec)) fail(Fail::Input, "The game files folder %s does not exist.", o.romfs_dir.c_str());
    std::vector<fs::path> pak_paths;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        if (_wcsicmp(it->path().extension().wstring().c_str(), L".pak") == 0) pak_paths.push_back(it->path());
    }
    std::sort(pak_paths.begin(), pak_paths.end());

    // 2. the ASTC textures (headers only)
    struct Job {
        size_t pak;
        size_t entry;
        TxtrInfo info;
        u64 weight;
    };
    std::vector<std::shared_ptr<const Pak>> paks;
    std::vector<Job> jobs;
    for (const fs::path& pp : pak_paths) {
        const std::string utf8 = narrow(pp.wstring());
        std::shared_ptr<const Pak> pak;
        try {
            pak = Pak::open(open_file_storage(utf8));
        } catch (const std::exception& e) {
            MPRS_LOGW("Skipping %s: %s", utf8.c_str(), e.what());
            continue;
        }
        const size_t pi = paks.size();
        paks.push_back(pak);
        const std::vector<PakEntry>& assets = pak->assets();
        for (size_t ei = 0; ei < assets.size(); ++ei) {
            const PakEntry& e = assets[ei];
            if (std::strcmp(e.type, "TXTR") != 0) continue;
            try {
                TxtrInfo info = txtr_read_info(*pak, e);
                if (!txtr_format(info.format).astc) continue;
                const u64 weight = static_cast<u64>(info.width) * info.height * std::max<u32>(1, info.layers);
                jobs.push_back(Job{pi, ei, info, weight});
            } catch (const std::exception& err) {
                ++stats.failed;
                MPRS_LOGW("Texture %s in %s: header unreadable, not cached: %s", e.id.str().c_str(), utf8.c_str(), err.what());
            }
        }
    }
    std::stable_sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.weight > b.weight; });
    const size_t total = jobs.size();
    u64 total_weight = 0, done_weight = 0;
    for (const Job& j : jobs) total_weight += j.weight;

    // 3. convert in parallel, largest first
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
    };

    const auto run_job = [&](const Job& j) -> Result {
        const Pak& pak = *paks[j.pak];
        const PakEntry& e = pak.assets()[j.entry];
        const std::vector<u8> tiled = txtr_read_tiled(pak, e, j.info);
        const TexCachePlan plan = texcache_plan(j.info, tiled.data(), tiled.size(), o.quality);
        if (!plan.ok) {
            const char* why = detail::texcache_reject_reason(j.info, tiled.size(), o.quality);
            MPRS_LOGW("Texture %s: not cached, the cache plan rejected it: %s", j.info.id.str().c_str(),
                      why ? why : "unknown reason");
            return {Outcome::Rejected, 0};
        }
        {
            std::lock_guard<std::mutex> lk(m);
            if (!claimed.insert(plan.key).second) return {Outcome::Skipped, 0};   // same texture in another pak
        }
        Claim claim{m, claimed, plan.key};
        const std::string path = texcache_path(o.cache_dir, plan.key);
        if (texcache_file_valid(path, plan)) {
            claim.keep = true;
            return {Outcome::Skipped, 0};
        }
        const std::vector<u8> bytes = texcache_build_file(j.info, plan, tiled.data(), tiled.size(), o.quality);
        {
            AtomicFileWriter w(path);
            w.write(bytes.data(), bytes.size());
            w.finish();
        }
        claim.keep = true;
        return {Outcome::Converted, bytes.size()};
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
                MPRS_LOGE("Texture %s: %s", j.info.id.str().c_str(), err.c_str());
                std::lock_guard<std::mutex> lk(m);
                if (disk_error.empty()) disk_error = err;
                stop = true;
                return;
            }
            if (failed_job) MPRS_LOGW("Texture %s: not cached: %s", j.info.id.str().c_str(), err.c_str());

            std::lock_guard<std::mutex> lk(m);
            if (failed_job) {
                ++stats.failed;
            } else if (r.outcome == Outcome::Converted) {
                ++stats.converted;
                stats.bytes_written += r.bytes;
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

}  // namespace mprs
