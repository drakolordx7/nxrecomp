// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for texcache.cpp (BC7 cache of the runtime's ASTC textures).
//   test_texcache                  full test suite (real-data parts are skipped with a note when the RomFS is missing)
//   test_texcache planonly [dir]   plan-only pass over every ASTC texture of the RomFS: counts by reject reason, no conversion
// Environment: MPRS_ROMFS (default D:\AI\Active\Metroid-Prime\game\romfs), MPRS_NVN_H (default the runtime's nvn.h),
// MPRS_TEXCACHE_LIMIT (real textures for the sample, default 80), MPRS_TEXCACHE_SCRATCH (default %TEMP%\mprs_texcache_test).
// The reference oracle for the hash is copied from runtime/src/gfx/nvn/nvn_texdata.cpp on purpose (independent of texcache.cpp).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "astc2bc7.h"
#include "check.h"
#include "common.h"
#include "fileio.h"
#include "pak.h"
#include "storage.h"
#include "tegra_tiling.h"
#include "texcache.h"
#include "txtr.h"

namespace mprs {
namespace detail {
const char* texcache_reject_reason(const TxtrInfo& info, size_t tiled_len, int quality);
}
}  // namespace mprs

namespace fs = std::filesystem;
using namespace mprs;

namespace {

// ---- helpers ------------------------------------------------------------------------------------------------------------------
std::string env_or(const char* name, const std::string& def) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : def;
}

struct Rng {
    u64 s;
    u64 next() {
        s += 0x9E3779B97F4A7C15ull;
        u64 z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    u8 byte() { return static_cast<u8>(next()); }
};

u32 rd32(const u8* p) { return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24; }
u64 rd64(const u8* p) { return u64(rd32(p)) | u64(rd32(p + 4)) << 32; }

void write_file(const std::string& path, const std::vector<u8>& bytes) {
    fs::create_directories(fs::path(widen(path)).parent_path());
    std::ofstream f(fs::path(widen(path)), std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Pak files of the RomFS in a fixed order; GuiSysMP1.pak first (it has the most ASTC textures).
std::vector<fs::path> romfs_paks(const fs::path& root) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2) && _wcsicmp(it->path().extension().wstring().c_str(), L".pak") == 0)
            out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    auto gui = std::find_if(out.begin(), out.end(), [](const fs::path& p) { return p.filename() == L"GuiSysMP1.pak"; });
    if (gui != out.end() && gui != out.begin()) std::rotate(out.begin(), gui, gui + 1);
    return out;
}

// Mip count the runtime keeps: min(mips, floor(log2(largest dimension)) + 1), at least 1.
u32 clamp_levels(u32 mips, u32 maxdim) {
    u32 top = 1;
    while (top < 32 && (u64(maxdim) >> top) != 0) ++top;
    return std::min(std::max<u32>(1, mips), top);
}

// Layout the runtime builds for a block-linear ASTC texture (test copy of nvn_texture_layout.cpp's rules).
tegra::TextureDesc test_desc(u32 w, u32 h, u32 depth, u32 levels, u32 layers, u32 bw, u32 bh, bool is3d) {
    tegra::TextureDesc d;
    d.width = w;
    d.height = h;
    d.depth = depth;
    d.levels = levels;
    d.layers = is3d ? 1 : layers;
    d.block_w = bw;
    d.block_h = bh;
    d.bytes_per_block = 16;
    d.linear_stride_align = 1;
    if (is3d) {
        d.gob_blocks_y = 1;
        d.gob_blocks_z = 16;
    } else {
        d.gob_blocks_y = tegra::default_gob_blocks_y((h + bh - 1) / bh);
        d.gob_blocks_z = 1;
    }
    return d;
}

// ---- oracle hash: copied from runtime/src/gfx/nvn/nvn_texdata.cpp (hash_seeded) ---------------------------------------------
namespace oracle {
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
}  // namespace oracle

// ---- 1. hash ----------------------------------------------------------------------------------------------------------------
void test_hash() {
    const size_t lens[] = {0, 1, 3, 4, 7, 8, 31, 32, 33, 100, 4096 + 5};
    const u64 seeds[] = {0, 1, 0x0123456789ABCDEFull, 0xFFFFFFFFFFFFFFFFull, 0x9E3779B185EBCA87ull};
    std::vector<u8> buf(4096 + 5);
    for (size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<u8>((i * 131u + 7u) ^ (i >> 3));
    for (size_t len : lens)
        for (u64 seed : seeds) {
            const u64 want = oracle::hash_seeded(buf.data(), len, seed);
            CHECK_EQ(tex_hash_seeded(buf.data(), len, seed), want);
        }
    CHECK_EQ(tex_hash_bytes(buf.data(), 80), oracle::hash_seeded(buf.data(), 80, 0));
    CHECK_EQ(tex_hash_bytes(buf.data(), 0), oracle::hash_seeded(buf.data(), 0, 0));
}

// ---- 2. NVN enum table against nvn.h ----------------------------------------------------------------------------------------
std::map<std::string, unsigned> parse_nvn_astc(const std::string& path) {
    std::map<std::string, unsigned> m;
    std::ifstream f(fs::path(widen(path)));
    std::string line;
    while (std::getline(f, line)) {
        const size_t p = line.find("NVN_FORMAT_RGBA_ASTC_");
        if (p == std::string::npos) continue;
        const size_t eq = line.find(" = 0x", p);
        if (eq == std::string::npos) continue;
        m[line.substr(p, eq - p)] = static_cast<unsigned>(std::strtoul(line.c_str() + eq + 5, nullptr, 16));
    }
    return m;
}

void test_nvn_table(const std::string& nvn_h) {
    const auto table = parse_nvn_astc(nvn_h);
    if (table.empty()) {
        std::printf("NOTE: no NVN_FORMAT_RGBA_ASTC lines in %s, enum check skipped\n", nvn_h.c_str());
        return;
    }
    int astc_ids = 0;
    for (u32 id = 0; id < 85; ++id) {
        const TxtrFormat& f = txtr_format(id);
        if (!f.astc) continue;
        ++astc_ids;
        const std::string name = std::string("NVN_FORMAT_RGBA_ASTC_") + std::to_string(f.block_w) + "x" +
                                 std::to_string(f.block_h) + (f.srgb ? "_SRGB" : "");
        const auto it = table.find(name);
        CHECK(it != table.end());
        if (it != table.end()) CHECK_EQ(tex_nvn_astc_format(id), static_cast<int>(it->second));
    }
    CHECK_EQ(astc_ids, 28);
    CHECK_EQ(tex_nvn_astc_format(0), 0);
    CHECK_EQ(tex_nvn_astc_format(52), 0);   // "None"
    CHECK_EQ(tex_nvn_astc_format(9999), 0);
}

// ---- 3. synthetic textures, end to end -------------------------------------------------------------------------------------
struct Synth {
    TxtrInfo info;
    std::vector<u8> tiled;
    tegra::TextureDesc desc;
    tegra::Layout layout;
};

// Constant-colour void-extent ASTC blocks with random colours, tiled by tegra::tile into a zero-filled buffer of the untrimmed
// layer stride times the layers (the size the game's buffer has).
void fill_void_extent(std::vector<u8>& linear, const tegra::Layout& L, const tegra::TextureDesc& d, Rng& rng) {
    for (const tegra::LevelLayout& lv : L.levels)
        for (u32 layer = 0; layer < d.layers; ++layer)
            for (u32 z = 0; z < lv.depth; ++z) {
                const u64 slice = lv.linear_slice_size / lv.depth;
                const u64 base = lv.linear_offset + layer * lv.linear_slice_size + z * slice;
                for (u32 by = 0; by < lv.blocks_y; ++by)
                    for (u32 bx = 0; bx < lv.blocks_x; ++bx) {
                        u8* b = &linear[base + (u64(by) * lv.blocks_x + bx) * 16];
                        b[0] = 0xFC;
                        b[1] = 0xFD;
                        std::memset(b + 2, 0xFF, 6);
                        for (int k = 8; k < 16; ++k) b[k] = rng.byte();
                    }
            }
}

Synth make_synth(TxtrKind kind, u32 fmt_id, u32 w, u32 h, u32 n, u32 mips, u64 seed) {
    const TxtrFormat& f = txtr_format(fmt_id);
    const bool is3d = kind == TxtrKind::D3;
    u32 layers = 1;
    if (kind == TxtrKind::Cube) layers = 6;
    if (kind == TxtrKind::D2Array) layers = n;
    const u32 depth = is3d ? n : 1;
    const u32 maxdim = is3d ? std::max(std::max(w, h), depth) : std::max(w, h);
    const u32 levels = clamp_levels(mips, maxdim);

    Synth s;
    s.desc = test_desc(w, h, depth, levels, layers, f.block_w, f.block_h, is3d);
    s.layout = tegra::compute_layout(s.desc);
    if (!s.layout.valid) {
        CHECK(s.layout.valid);
        return s;
    }
    Rng rng{seed};
    std::vector<u8> linear(static_cast<size_t>(s.layout.linear_size));
    for (u8& b : linear) b = rng.byte();
    fill_void_extent(linear, s.layout, s.desc, rng);
    const u64 storage = s.layout.tiled_layer_stride * s.desc.layers;
    s.tiled.assign(static_cast<size_t>(storage), 0);
    CHECK(tegra::tile(s.desc, s.layout, linear.data(), s.tiled.data()));

    s.info.id.b[0] = static_cast<u8>(seed);
    s.info.kind = kind;
    s.info.format = fmt_id;
    s.info.width = w;
    s.info.height = h;
    s.info.layers = is3d ? depth : layers;
    s.info.mips = mips;
    s.info.gpu_size = storage;
    return s;
}

// The key as the runtime computes it: meta (ten u64) hashed, then the storage bytes hashed with that seed.
u64 expected_key(const Synth& s, u32 nvn, u32 levels, u32 layers, int quality) {
    const tegra::LevelLayout& l0 = s.layout.levels[0];
    const u64 meta[10] = {1, nvn, l0.width, l0.height, l0.depth, levels, layers, l0.gob_blocks_y, 0, u64(quality)};
    const u64 mh = oracle::hash_seeded(reinterpret_cast<const u8*>(meta), sizeof meta, 0);
    return oracle::hash_seeded(s.tiled.data(), static_cast<size_t>(s.layout.tiled_layer_stride * s.desc.layers), mh);
}

void check_synth(const char* name, TxtrKind kind, u32 fmt_id, u32 w, u32 h, u32 n, u32 mips, u32 want_levels,
                 u32 want_layers, u32 want_vk, u64 seed) {
    std::printf("synthetic %s\n", name);
    const fs::path dir = fs::temp_directory_path() / "mprs_test_texcache" / "synth";
    const int quality = 3;
    const Synth s = make_synth(kind, fmt_id, w, h, n, mips, seed);
    if (s.tiled.empty()) return;
    const bool is3d = kind == TxtrKind::D3;

    const TexCachePlan plan = texcache_plan(s.info, s.tiled.data(), s.tiled.size(), quality);
    CHECK(plan.ok);
    if (!plan.ok) return;
    CHECK_EQ(plan.levels, want_levels);
    CHECK_EQ(plan.layers, want_layers);
    CHECK_EQ(plan.vk_format, want_vk);
    CHECK_EQ(plan.level.size(), static_cast<size_t>(want_levels));

    // expected BC7 level table: 4x4 blocks per slice, all layers
    u64 off = 0;
    for (u32 l = 0; l < want_levels && l < plan.level.size(); ++l) {
        const u32 lw = std::max(1u, w >> l), lh = std::max(1u, h >> l), ld = is3d ? std::max(1u, s.desc.depth >> l) : 1u;
        const u64 size = u64((lw + 3) / 4) * ((lh + 3) / 4) * 16 * ld * want_layers;
        CHECK_EQ(plan.level[l].width, lw);
        CHECK_EQ(plan.level[l].height, lh);
        CHECK_EQ(plan.level[l].depth, ld);
        CHECK_EQ(plan.level[l].offset, off);
        CHECK_EQ(plan.level[l].size, size);
        off += size;
    }
    CHECK_EQ(plan.data_bytes, off);
    CHECK_EQ(plan.file_bytes, u64(28 + 28 * want_levels) + off);
    CHECK_EQ(plan.key, expected_key(s, static_cast<u32>(tex_nvn_astc_format(fmt_id)), want_levels, want_layers, quality));

    const std::vector<u8> bytes = texcache_build_file(s.info, plan, s.tiled.data(), s.tiled.size(), quality);
    CHECK_EQ(bytes.size(), plan.file_bytes);
    if (bytes.size() != plan.file_bytes) return;
    CHECK(std::memcmp(bytes.data(), "NXB7", 4) == 0);
    CHECK_EQ(rd32(&bytes[4]), 1u);
    CHECK_EQ(rd64(&bytes[8]), plan.key);
    CHECK_EQ(rd32(&bytes[16]), want_vk);
    CHECK_EQ(rd32(&bytes[20]), want_levels);
    CHECK_EQ(rd32(&bytes[24]), want_layers);
    for (size_t i = 0; i < plan.level.size(); ++i) {
        const u8* e = &bytes[28 + i * 28];
        CHECK_EQ(rd32(e), plan.level[i].width);
        CHECK_EQ(rd32(e + 4), plan.level[i].height);
        CHECK_EQ(rd32(e + 8), plan.level[i].depth);
        CHECK_EQ(rd64(e + 12), plan.level[i].offset);
        CHECK_EQ(rd64(e + 20), plan.level[i].size);
    }

    // written under its real cache path: valid, and invalid when damaged
    const std::string path = texcache_path((dir / "cache").string(), plan.key);
    write_file(path, bytes);
    CHECK(texcache_file_valid(path, plan));
    {
        std::vector<u8> bad = bytes;
        bad[8] ^= 0x01;   // key byte
        const std::string p2 = path + ".badkey";
        write_file(p2, bad);
        CHECK(!texcache_file_valid(p2, plan));
    }
    {
        std::vector<u8> bad = bytes;
        bad[16] ^= 0x01;  // VkFormat
        const std::string p2 = path + ".badfmt";
        write_file(p2, bad);
        CHECK(!texcache_file_valid(p2, plan));
    }
    {
        std::vector<u8> cut(bytes.begin(), bytes.end() - 1);
        const std::string p2 = path + ".cut";
        write_file(p2, cut);
        CHECK(!texcache_file_valid(p2, plan));
    }
    CHECK(!texcache_file_valid(path + ".missing", plan));

    // deterministic: same input, same bytes; a damaged source gives another key
    CHECK(texcache_build_file(s.info, plan, s.tiled.data(), s.tiled.size(), quality) == bytes);
    std::vector<u8> mutated = s.tiled;
    mutated[mutated.size() / 2] ^= 0x5A;
    const TexCachePlan other = texcache_plan(s.info, mutated.data(), mutated.size(), quality);
    CHECK(other.ok && other.key != plan.key);
}

void test_synthetic() {
    check_synth("Astc5x4 sRGB 64x40, 4 levels, 1 layer", TxtrKind::D2, 68, 64, 40, 1, 4, 4, 1, 146, 11);
    check_synth("Astc4x4 cube 64x64, 3 mips, 6 layers", TxtrKind::Cube, 53, 64, 64, 6, 3, 3, 6, 145, 12);
    check_synth("Astc8x5 sRGB 2D array 40x24, 3 layers, 2 mips", TxtrKind::D2Array, 72, 40, 24, 3, 2, 2, 3, 146, 13);
    check_synth("Astc8x8 sRGB 16x8, 9 mips requested (clamped to 5)", TxtrKind::D2, 74, 16, 8, 1, 9, 5, 1, 146, 14);
    check_synth("Astc4x4 3D 16x16x4, 1 mip", TxtrKind::D3, 53, 16, 16, 4, 1, 1, 1, 145, 15);
}

// ---- real data helpers -------------------------------------------------------------------------------------------------------
struct AstcRef {
    std::shared_ptr<const Pak> pak;
    size_t entry = 0;
    TxtrInfo info;
};

// ASTC textures of the RomFS paks (headers only), in pak order, at most `limit`, optionally filtered.
template <class Pred>
std::vector<AstcRef> collect_astc(const std::vector<fs::path>& paks, size_t limit, Pred keep) {
    std::vector<AstcRef> out;
    for (const fs::path& p : paks) {
        if (out.size() >= limit) break;
        std::shared_ptr<const Pak> pak;
        try {
            pak = Pak::open(open_file_storage(narrow(p.wstring())));
        } catch (const std::exception& e) {
            std::printf("NOTE: skipped %s: %s\n", narrow(p.wstring()).c_str(), e.what());
            continue;
        }
        const std::vector<PakEntry>& assets = pak->assets();
        for (size_t i = 0; i < assets.size() && out.size() < limit; ++i) {
            if (std::strcmp(assets[i].type, "TXTR") != 0) continue;
            try {
                TxtrInfo info = txtr_read_info(*pak, assets[i]);
                if (!txtr_format(info.format).astc) continue;
                if (!keep(info)) continue;
                out.push_back(AstcRef{pak, i, info});
            } catch (const std::exception&) {
                continue;
            }
        }
    }
    return out;
}

// Level 0, layer 0 of a 2D texture decoded with the ASTC decoder (the reference for the BC7 check).
bool decode_level0(const TxtrInfo& info, const std::vector<u8>& tiled, std::vector<u8>& rgba, u32& w, u32& h) {
    const TxtrFormat& f = txtr_format(info.format);
    u32 layers = 1;
    if (info.kind == TxtrKind::Cube) layers = 6;
    if (info.kind == TxtrKind::D2Array) layers = info.layers;
    const u32 levels = clamp_levels(info.mips, std::max(info.width, info.height));
    const tegra::TextureDesc d = test_desc(info.width, info.height, 1, levels, layers, f.block_w, f.block_h, false);
    const tegra::Layout L = tegra::compute_layout(d);
    if (!L.valid) return false;
    std::vector<u8> astc(static_cast<size_t>(L.levels[0].linear_slice_size));
    tegra::untile_level(d, L.levels[0], tiled.data(), astc.data());
    a2b_input in{};
    in.format = f.srgb ? A2B_ASTC_SRGB : A2B_ASTC_UNORM;
    in.width = info.width;
    in.height = info.height;
    in.block_w = f.block_w;
    in.block_h = f.block_h;
    w = info.width;
    h = info.height;
    rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    return a2b_decode_astc_rgba8(&in, astc.data(), astc.size(), 1, rgba.data(), nullptr) == A2B_OK;
}

// ---- 4. real data sample: plan, build, write, sidecar reference ------------------------------------------------------------
void test_real_sample(const std::string& romfs, const fs::path& scratch) {
    const fs::path root(widen(romfs));
    if (!fs::is_directory(root)) {
        std::printf("NOTE: RomFS %s not found, real-data sample skipped\n", romfs.c_str());
        return;
    }
    const size_t limit = static_cast<size_t>(std::atoi(env_or("MPRS_TEXCACHE_LIMIT", "80").c_str()));
    const auto paks = romfs_paks(root);
    const auto refs = collect_astc(paks, limit, [](const TxtrInfo&) { return true; });
    std::printf("real sample: %zu ASTC textures taken from the RomFS\n", refs.size());

    const fs::path cache = scratch / "real" / "cache";
    const fs::path refdir = scratch / "real" / "refs";
    std::error_code ec;
    fs::remove_all(scratch / "real", ec);
    fs::create_directories(refdir);

    std::set<u64> keys;
    u32 converted = 0, duplicates = 0, rejected = 0, ref_written = 0;
    double build_s = 0, mpix = 0;
    for (const AstcRef& r : refs) {
        const PakEntry& e = r.pak->assets()[r.entry];
        const std::vector<u8> tiled = txtr_read_tiled(*r.pak, e, r.info);
        const TexCachePlan plan = texcache_plan(r.info, tiled.data(), tiled.size(), 3);
        if (!plan.ok) {
            ++rejected;
            CHECK(plan.ok);
            std::printf("  texture %s rejected: %s\n", r.info.id.str().c_str(),
                        detail::texcache_reject_reason(r.info, tiled.size(), 3) ? "see reason" : "no reason");
            continue;
        }
        if (!keys.insert(plan.key).second) {
            ++duplicates;
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        const std::vector<u8> bytes = texcache_build_file(r.info, plan, tiled.data(), tiled.size(), 3);
        build_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        u64 texels = 0;
        for (const TexOutLevel& lv : plan.level) texels += u64(lv.width) * lv.height * lv.depth;
        mpix += double(texels) * plan.layers / 1e6;

        const std::string path = texcache_path(cache.string(), plan.key);
        write_file(path, bytes);
        CHECK(texcache_file_valid(path, plan));
        ++converted;

        if (r.info.kind != TxtrKind::D3) {
            std::vector<u8> rgba;
            u32 w = 0, h = 0;
            CHECK(decode_level0(r.info, tiled, rgba, w, h));
            std::vector<u8> side(8);
            for (int i = 0; i < 4; ++i) side[i] = static_cast<u8>(w >> (8 * i));
            for (int i = 0; i < 4; ++i) side[4 + i] = static_cast<u8>(h >> (8 * i));
            side.insert(side.end(), rgba.begin(), rgba.end());
            char name[32];
            std::snprintf(name, sizeof name, "%016llx.ref", static_cast<unsigned long long>(plan.key));
            write_file((refdir / name).string(), side);
            ++ref_written;
        }
    }
    std::printf("real sample: %u converted, %u duplicate keys, %u rejected, %u references written\n", converted, duplicates,
                rejected, ref_written);
    if (build_s > 0)
        std::printf("real sample throughput, 1 thread: %.2f s, %.1f textures/s, %.2f MPix/s (all levels and layers)\n",
                    build_s, converted / build_s, mpix / build_s);
    CHECK(converted > 0);
}

// ---- 5. thread count does not change the bytes ------------------------------------------------------------------------------
void test_thread_determinism(const std::string& romfs) {
    const fs::path root(widen(romfs));
    if (!fs::is_directory(root)) return;
    const auto refs = collect_astc(romfs_paks(root), 1, [](const TxtrInfo& i) {
        return i.kind == TxtrKind::D2 && i.width == 512 && i.height == 512;
    });
    if (refs.empty()) {
        std::printf("NOTE: no 512x512 ASTC texture found, thread determinism check skipped\n");
        return;
    }
    const AstcRef& r = refs[0];
    const TxtrFormat& f = txtr_format(r.info.format);
    const std::vector<u8> tiled = txtr_read_tiled(*r.pak, r.pak->assets()[r.entry], r.info);
    const tegra::TextureDesc d = test_desc(512, 512, 1, 1, 1, f.block_w, f.block_h, false);
    const tegra::Layout L = tegra::compute_layout(d);
    CHECK(L.valid);
    if (!L.valid) return;
    std::vector<u8> astc(static_cast<size_t>(L.levels[0].linear_slice_size));
    tegra::untile_level(d, L.levels[0], tiled.data(), astc.data());

    a2b_input in{};
    in.format = f.srgb ? A2B_ASTC_SRGB : A2B_ASTC_UNORM;
    in.width = 512;
    in.height = 512;
    in.block_w = f.block_w;
    in.block_h = f.block_h;
    std::vector<u8> out1(static_cast<size_t>(a2b_output_size(&in))), out8(out1.size()), out8b(out1.size());
    a2b_options o1{};
    o1.quality = 3;
    o1.threads = 1;
    a2b_options o8 = o1;
    o8.threads = 8;
    a2b_format of = A2B_BC7_UNORM;
    CHECK_EQ(a2b_transcode(&in, astc.data(), astc.size(), &o1, &of, out1.data(), out1.size(), nullptr), A2B_OK);
    CHECK_EQ(a2b_transcode(&in, astc.data(), astc.size(), &o8, &of, out8.data(), out8.size(), nullptr), A2B_OK);
    CHECK_EQ(a2b_transcode(&in, astc.data(), astc.size(), &o8, &of, out8b.data(), out8b.size(), nullptr), A2B_OK);
    CHECK(out1 == out8);
    CHECK(out8 == out8b);
    std::printf("thread determinism: 512x512 %s, threads 1 vs 8: %s\n", r.info.id.str().c_str(),
                out1 == out8 ? "identical" : "DIFFERENT");
}

// ---- planonly: counts by reject reason over the whole RomFS -----------------------------------------------------------------
int planonly(const std::string& romfs) {
    const fs::path root(widen(romfs));
    if (!fs::is_directory(root)) {
        std::printf("RomFS %s not found\n", romfs.c_str());
        return 1;
    }
    std::map<std::string, u32> by_reason;
    std::map<std::string, u32> by_kind;
    u32 astc_total = 0, accepted = 0, plan_mismatch = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (const fs::path& p : romfs_paks(root)) {
        std::shared_ptr<const Pak> pak;
        try {
            pak = Pak::open(open_file_storage(narrow(p.wstring())));
        } catch (const std::exception& e) {
            std::printf("skipped %s: %s\n", narrow(p.wstring()).c_str(), e.what());
            continue;
        }
        const std::vector<PakEntry>& assets = pak->assets();
        for (size_t i = 0; i < assets.size(); ++i) {
            if (std::strcmp(assets[i].type, "TXTR") != 0) continue;
            TxtrInfo info;
            try {
                info = txtr_read_info(*pak, assets[i]);
                if (!txtr_format(info.format).astc) continue;
            } catch (const std::exception& e) {
                by_reason[std::string("header unreadable: ") + e.what()]++;
                ++astc_total;
                continue;
            }
            ++astc_total;
            const char* kind_names[] = {"D1", "D2", "D3", "Cube", "D1Array", "D2Array", "D2MS", "D2MSArray", "CubeArray"};
            by_kind[kind_names[static_cast<u32>(info.kind)]]++;
            if (info.kind == TxtrKind::D3) {   // 3D: the runtime allocates the untrimmed layer stride; show both sizes
                const TxtrFormat& f = txtr_format(info.format);
                const u32 md = std::max(std::max(info.width, info.height), info.layers);
                tegra::TextureDesc d = test_desc(info.width, info.height, info.layers, clamp_levels(info.mips, md), 1,
                                                 f.block_w, f.block_h, true);
                const tegra::Layout L = tegra::compute_layout(d);
                std::printf("3D %s %ux%ux%u mips %u: gpu_size %llu, layout storage %llu\n", info.id.str().c_str(), info.width,
                            info.height, info.layers, info.mips, static_cast<unsigned long long>(info.gpu_size),
                            static_cast<unsigned long long>(L.tiled_layer_stride));
            }
            const char* why = detail::texcache_reject_reason(info, static_cast<size_t>(info.gpu_size), 3);
            if (why) {
                by_reason[why]++;
                continue;
            }
            ++accepted;
            try {
                const std::vector<u8> tiled = txtr_read_tiled(*pak, assets[i], info);
                if (!texcache_plan(info, tiled.data(), tiled.size(), 3).ok) ++plan_mismatch;
            } catch (const std::exception& e) {
                by_reason[std::string("tiled read failed: ") + e.what()]++;
            }
        }
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("ASTC TXTR entries: %u (%.1f s)\n", astc_total, s);
    std::printf("plan accepted: %u, of which texcache_plan(ok) false: %u\n", accepted, plan_mismatch);
    std::printf("by kind:\n");
    for (const auto& kv : by_kind) std::printf("  %-12s %u\n", kv.first.c_str(), kv.second);
    std::printf("rejected by reason:\n");
    u32 rejected = 0;
    for (const auto& kv : by_reason) {
        std::printf("  %5u  %s\n", kv.second, kv.first.c_str());
        rejected += kv.second;
    }
    std::printf("rejected total: %u\n", rejected);
    return 0;
}

// keysdump <romfs> <out.tsv>: the plan (key, format, levels, layers, file size) of every ASTC texture, for comparing with
// the cache files the runtime wrote (tools/setup/build and work/run bc7_cache folders)
int keysdump(const std::string& romfs, const std::string& out_path) {
    const fs::path root(widen(romfs));
    std::ofstream out(fs::path(widen(out_path)));
    if (!out) return 1;
    out << "key\tvk\tlevels\tlayers\tfile_bytes\tkind\tw\th\tmips\tpak\tid\n";
    u32 n = 0, rejected = 0;
    for (const fs::path& p : romfs_paks(root)) {
        std::shared_ptr<const Pak> pak;
        try {
            pak = Pak::open(open_file_storage(narrow(p.wstring())));
        } catch (const std::exception&) {
            continue;
        }
        const std::vector<PakEntry>& assets = pak->assets();
        for (size_t i = 0; i < assets.size(); ++i) {
            if (std::strcmp(assets[i].type, "TXTR") != 0) continue;
            TxtrInfo info;
            try {
                info = txtr_read_info(*pak, assets[i]);
                if (!txtr_format(info.format).astc) continue;
                const std::vector<u8> tiled = txtr_read_tiled(*pak, assets[i], info);
                const TexCachePlan plan = texcache_plan(info, tiled.data(), tiled.size(), 3);
                if (!plan.ok) {
                    ++rejected;
                    continue;
                }
                ++n;
                out << std::hex << plan.key << std::dec << '\t' << plan.vk_format << '\t' << plan.levels << '\t' << plan.layers
                    << '\t' << plan.file_bytes << '\t' << static_cast<u32>(info.kind) << '\t' << info.width << '\t' << info.height
                    << '\t' << info.mips << '\t' << narrow(p.filename().wstring()) << '\t' << info.id.str() << '\n';
            } catch (const std::exception&) {
                ++rejected;
            }
        }
    }
    std::printf("keysdump: %u planned, %u rejected or unreadable -> %s\n", n, rejected, out_path.c_str());
    return 0;
}

// buildcache <romfs> <cache dir> [threads]: runs build_texture_cache itself (used on a subset folder, the full run is done separately)
int buildcache(const std::string& romfs, const std::string& cache, int threads) {
    TexCacheOptions o;
    o.romfs_dir = romfs;
    o.cache_dir = cache;
    o.quality = 3;
    o.threads = threads;
    const TexCacheStats st = build_texture_cache(
        o,
        [](u64 done, u64 total) {
            if (done % 25 == 0 || done == total) std::printf("progress %llu/%llu\n", static_cast<unsigned long long>(done), static_cast<unsigned long long>(total));
        },
        [] { return false; });
    std::printf("build_texture_cache: textures_total %u, converted %u, skipped %u, failed %u, bytes_written %llu, %.2f s\n",
                st.textures_total, st.converted, st.skipped, st.failed, static_cast<unsigned long long>(st.bytes_written),
                st.seconds);
    return 0;
}

// mkrefs <romfs> <cache dir> <ref dir> <every k> [quality]: for every k-th ASTC texture of the RomFS whose cache file exists in
// <cache dir>, writes the sidecar reference (ASTC decode of level 0, layer 0) for scripts/check_texcache.py.
int mkrefs(const std::string& romfs, const std::string& cache, const std::string& refdir, int every, int quality) {
    const fs::path root(widen(romfs));
    const auto refs = collect_astc(romfs_paks(root), static_cast<size_t>(-1), [](const TxtrInfo&) { return true; });
    fs::create_directories(widen(refdir));
    u32 n = 0, seen = 0;
    for (const AstcRef& r : refs) {
        if (r.info.kind == TxtrKind::D3) continue;
        if ((seen++ % static_cast<u32>(every)) != 0) continue;
        const std::vector<u8> tiled = txtr_read_tiled(*r.pak, r.pak->assets()[r.entry], r.info);
        const TexCachePlan plan = texcache_plan(r.info, tiled.data(), tiled.size(), quality);
        if (!plan.ok || !texcache_file_valid(texcache_path(cache, plan.key), plan)) continue;
        std::vector<u8> rgba;
        u32 w = 0, h = 0;
        if (!decode_level0(r.info, tiled, rgba, w, h)) continue;
        std::vector<u8> side(8);
        for (int i = 0; i < 4; ++i) side[i] = static_cast<u8>(w >> (8 * i));
        for (int i = 0; i < 4; ++i) side[4 + i] = static_cast<u8>(h >> (8 * i));
        side.insert(side.end(), rgba.begin(), rgba.end());
        char name[32];
        std::snprintf(name, sizeof name, "%016llx.ref", static_cast<unsigned long long>(plan.key));
        write_file((fs::path(widen(refdir)) / name).string(), side);
        ++n;
    }
    std::printf("mkrefs: %u references written to %s\n", n, refdir.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string romfs = env_or("MPRS_ROMFS", "D:\\AI\\Active\\Metroid-Prime\\game\\romfs");
    if (argc >= 2 && std::string(argv[1]) == "planonly")
        return planonly(argc >= 3 ? std::string(argv[2]) : romfs);
    if (argc >= 6 && std::string(argv[1]) == "mkrefs")
        return mkrefs(argv[2], argv[3], argv[4], std::atoi(argv[5]), argc >= 7 ? std::atoi(argv[6]) : 3);
    if (argc >= 4 && std::string(argv[1]) == "keysdump") return keysdump(argv[2], argv[3]);
    if (argc >= 4 && std::string(argv[1]) == "buildcache") {
        try {
            return buildcache(argv[2], argv[3], argc >= 5 ? std::atoi(argv[4]) : 0);
        } catch (const std::exception& e) {
            std::printf("FAIL: build_texture_cache threw: %s\n", e.what());
            return 1;
        }
    }

    const std::string nvn_h = env_or("MPRS_NVN_H", "D:\\AI\\Active\\Metroid-Prime\\runtime\\src\\gfx\\nvn\\third_party\\nvn\\nvn.h");
    const fs::path scratch(widen(env_or("MPRS_TEXCACHE_SCRATCH",
                                        (fs::temp_directory_path() / "mprs_test_texcache").string())));
    try {
        if (a2b_init(nullptr) != A2B_OK) {
            std::printf("FAIL: astcdec-*-shared.dll not found next to the test executable\n");
            return 1;
        }
        test_hash();
        test_nvn_table(nvn_h);
        test_synthetic();
        test_real_sample(romfs, scratch);
        test_thread_determinism(romfs);
    } catch (const std::exception& e) {
        std::printf("FAIL: unexpected exception: %s\n", e.what());
        ++mprs_test::failures();
    }
    return TEST_SUMMARY("test_texcache");
}
