// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the CPU-side texture data preparation (src/gfx/nvn/nvn_texdata.cpp): untiling, pass-through of BCn, the pitch-linear
// copy, ASTC transcode to BC7 and decode to RGBA8 (through the astc2bc7 library), the disk cache, the caps and the failure paths.
// Textures are built by hand (no NVN pool): the image is tiled with tegra::tile into the storage, as the game's data sits in
// memory, and prepare_texture must give the image back.
// ASTC test blocks are constant-colour void-extent blocks (0xFC 0xFD 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF, then the RGBA16 colour):
// valid blocks of every footprint, with a known colour. The BC7 output is checked for format, sizes, layout and cache, and
// the RGBA8 decode is checked against the library's own decode. No BC7 pixel decoder is available here, so the BC7 pixels
// themselves are not compared (the tools/textures oracle does that).
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include <nxrt/config.h>

#include "astc2bc7.h"
#include "nvn_core.h"
#include "nvn_format.h"
#include "nvn_texdata.h"
#include "nvn_texture_layout.h"
#include "tegra_tiling.h"
#include "test_framework.h"
NXTEST_GROUP(gfx_nvn_texdata)

using namespace nxrt;
using namespace nxrt::nvn;
using namespace nxrt::nvn::be;
namespace fs = std::filesystem;

namespace {

uint64_t div_up(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

void fill_random(std::vector<uint8_t>& v, uint32_t seed) {
    std::mt19937 rng(seed);
    for (uint8_t& b : v) b = static_cast<uint8_t>(rng());
}

// Sets an environment variable; an empty value removes it (both on Windows and POSIX).
void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    if (*value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

// The disk cache off for a test: no file may be written into the tree. A developer's environment setting is removed too.
void no_cache() {
    set_env("NXRT_NVN_BC7_CACHE", "");
    config_set("nvn", "bc7_cache", "");
}

TextureBuilderInfo make_info(int target, int w, int h, int depth, int levels, int format, int flags = 0, ptrdiff_t stride = 0) {
    TextureBuilderInfo b;
    b.target = target;
    b.width = w;
    b.height = h;
    b.depth = depth;
    b.levels = levels;
    b.format = format;
    b.flags = flags;
    b.stride = stride;
    return b;
}

// A texture built by hand: its layout, the untiled image (ground truth) and the storage the game would hold.
struct Built {
    Ref<Texture> tex;
    TexLayout L;
    std::vector<uint8_t> lin;   // the image: levels back to back, layers, z slices, tight rows
    std::vector<uint8_t> mem;   // storage: tiled (block-linear) or pitch-padded (pitch-linear)
};

void make_texture(Built& bt, const TextureBuilderInfo& info) {
    bt.tex = Ref<Texture>(new Texture());
    Texture& t = *bt.tex;
    t.b = info;
    t.fmt = bt.L.fmt;
    t.mem = bt.mem.data();
    t.storage_size = bt.L.storage_size;
    t.block_linear = bt.L.block_linear;
    t.levels = bt.L.levels;
    t.layers = bt.L.layers;
    t.block_height_gobs = bt.L.block_linear ? bt.L.layout.levels[0].gob_blocks_y : 0;
}

// Block-linear texture: the image bt.lin is zero, the caller fills it, then finish_block_linear tiles it into the storage.
bool begin_block_linear(Built& bt, const TextureBuilderInfo& info) {
    bt.L = compute_texture_layout(info);
    if (!bt.L.valid || !bt.L.block_linear) return false;
    bt.lin.assign(bt.L.layout.linear_size, 0);
    return true;
}

void finish_block_linear(Built& bt, const TextureBuilderInfo& info) {
    bt.mem.assign(bt.L.storage_size, 0);
    tegra::tile(bt.L.desc, bt.L.layout, bt.lin.data(), bt.mem.data());
    make_texture(bt, info);
}

// Pitch-linear texture (2D, one level): the storage is random, padding included; the image is its tight rows.
void build_pitch(Built& bt, const TextureBuilderInfo& info, uint32_t seed) {
    bt.L = compute_texture_layout(info);
    CHECK(bt.L.valid && !bt.L.block_linear);
    bt.mem.assign(bt.L.storage_size, 0);
    fill_random(bt.mem, seed);
    const uint64_t bx = div_up(static_cast<uint64_t>(info.width), bt.L.fmt->block_w);
    const uint64_t by = div_up(static_cast<uint64_t>(info.height), bt.L.fmt->block_h);
    const uint64_t row = bx * bt.L.fmt->bytes;
    bt.lin.assign(row * by * bt.L.layers, 0);
    for (uint32_t layer = 0; layer < bt.L.layers; ++layer)
        for (uint64_t y = 0; y < by; ++y)
            std::memcpy(bt.lin.data() + layer * row * by + y * row,
                        bt.mem.data() + layer * bt.L.pitch * by + y * bt.L.pitch, static_cast<size_t>(row));
    make_texture(bt, info);
}

// A texture with storage but no image, for the failure paths.
Ref<Texture> bare_texture(const TextureBuilderInfo& info, std::vector<uint8_t>& buf) {
    buf.assign(1 << 16, 0);
    Ref<Texture> t(new Texture());
    t->b = info;
    t->mem = buf.data();
    t->storage_size = buf.size();
    return t;
}

// The image comes back: out holds the same levels and layers, each layer at its place in the untiled buffer.
void expect_image(const Built& bt, const PreparedTexture& out, const char* what) {
    CHECK_MSG(out.error.empty(), out.error);
    CHECK_EQ(out.levels, bt.L.levels);
    CHECK_EQ(out.layers, bt.L.layers);
    CHECK(!out.transcoded);
    CHECK(!out.from_cache);
    CHECK_MSG(out.data.size() == bt.lin.size(), what);
    for (size_t i = 0; i < bt.L.layout.levels.size() && i < out.level.size(); ++i) {
        const tegra::LevelLayout& lv = bt.L.layout.levels[i];
        CHECK_EQ(out.level[i].offset, lv.linear_offset);
        CHECK_EQ(out.level[i].size, lv.linear_slice_size * bt.L.layers);
        CHECK_EQ(out.level[i].width, lv.width);
        CHECK_EQ(out.level[i].height, lv.height);
        CHECK_EQ(out.level[i].depth, lv.depth);
        for (uint32_t layer = 0; layer < bt.L.layers; ++layer) {
            const uint64_t at = lv.linear_offset + layer * lv.linear_slice_size;
            CHECK_MSG(std::memcmp(out.data.data() + at, bt.lin.data() + at, static_cast<size_t>(lv.linear_slice_size)) == 0,
                      what);
        }
    }
}

// ASTC: fills layer `layer` of level `level` with constant-colour void-extent blocks; colours gets the RGBA16 of each block.
void fill_astc_void_level(Built& bt, size_t level, uint32_t layer, std::vector<uint16_t>& colours, uint32_t seed) {
    const tegra::LevelLayout& lv = bt.L.layout.levels[level];
    std::mt19937 rng(seed);
    colours.assign(static_cast<size_t>(lv.blocks_x) * lv.blocks_y * 4, 0);
    for (uint32_t y = 0; y < lv.blocks_y; ++y)
        for (uint32_t x = 0; x < lv.blocks_x; ++x) {
            uint8_t* blk = bt.lin.data() + lv.linear_offset + layer * lv.linear_slice_size + y * lv.linear_row_stride + x * 16;
            blk[0] = 0xFC;
            blk[1] = 0xFD;
            std::memset(blk + 2, 0xFF, 6);
            uint16_t c[4];
            for (uint16_t& v : c) v = static_cast<uint16_t>(rng());
            std::memcpy(blk + 8, c, 8);
            for (int i = 0; i < 4; ++i) colours[(static_cast<size_t>(y) * lv.blocks_x + x) * 4 + i] = c[i];
        }
}

// The RGBA8 decode of every level and layer must equal the library's own decode of the same ASTC slice.
void expect_decode_matches_library(const Built& bt, const PreparedTexture& d) {
    const FormatInfo* fmt = bt.L.fmt;
    for (size_t i = 0; i < bt.L.layout.levels.size(); ++i) {
        const tegra::LevelLayout& lv = bt.L.layout.levels[i];
        a2b_input in{};
        in.format = fmt->srgb ? A2B_ASTC_SRGB : A2B_ASTC_UNORM;
        in.width = lv.width;
        in.height = lv.height;
        in.block_w = fmt->block_w;
        in.block_h = fmt->block_h;
        const size_t slice_bytes = static_cast<size_t>(lv.linear_slice_size / lv.depth);
        const size_t pixel_bytes = static_cast<size_t>(lv.width) * lv.height * 4;
        std::vector<uint8_t> ref(pixel_bytes);
        for (uint32_t layer = 0; layer < bt.L.layers; ++layer)
            for (uint32_t z = 0; z < lv.depth; ++z) {
                const uint64_t src = lv.linear_offset + layer * lv.linear_slice_size + z * slice_bytes;
                CHECK_EQ(a2b_decode_astc_rgba8(&in, bt.lin.data() + src, slice_bytes, 0, ref.data(), nullptr), A2B_OK);
                const uint64_t dst = d.level[i].offset + (static_cast<uint64_t>(layer) * lv.depth + z) * pixel_bytes;
                CHECK_MSG(std::memcmp(d.data.data() + dst, ref.data(), pixel_bytes) == 0, "RGBA8 decode differs from library");
            }
    }
}

// Each BC7 slice must be the library's transcode of the same ASTC slice (placement of every slice in the output). The
// options are the defaults of the back end: quality 3 (the config default), all threads, no perceptual weights.
void expect_bc7_matches_library(const Built& bt, const PreparedTexture& out) {
    const FormatInfo* fmt = bt.L.fmt;
    a2b_options opt{};
    opt.quality = 3;
    opt.threads = 0;
    opt.perceptual = 0;
    opt.allow_opaque_mode6 = 0;
    for (size_t i = 0; i < bt.L.layout.levels.size(); ++i) {
        const tegra::LevelLayout& lv = bt.L.layout.levels[i];
        a2b_input in{};
        in.format = fmt->srgb ? A2B_ASTC_SRGB : A2B_ASTC_UNORM;
        in.width = lv.width;
        in.height = lv.height;
        in.block_w = fmt->block_w;
        in.block_h = fmt->block_h;
        const size_t in_slice = static_cast<size_t>(lv.linear_slice_size / lv.depth);
        const size_t out_slice = static_cast<size_t>(a2b_output_size(&in));
        std::vector<uint8_t> ref(out_slice);
        for (uint32_t layer = 0; layer < bt.L.layers; ++layer)
            for (uint32_t z = 0; z < lv.depth; ++z) {
                const uint64_t src = lv.linear_offset + layer * lv.linear_slice_size + z * in_slice;
                a2b_format of{};
                CHECK_EQ(a2b_transcode(&in, bt.lin.data() + src, in_slice, &opt, &of, ref.data(), out_slice, nullptr), A2B_OK);
                const uint64_t dst = out.level[i].offset + (static_cast<uint64_t>(layer) * lv.depth + z) * out_slice;
                CHECK_MSG(std::memcmp(out.data.data() + dst, ref.data(), out_slice) == 0, "BC7 slice differs from library");
            }
    }
}

// Constant-colour blocks decode to their colour at the origin pixel of each block (within one 8-bit step, linear formats).
void expect_void_colours(const uint8_t* rgba, uint32_t width, uint32_t bw, uint32_t bh, uint32_t bx, uint32_t by,
                         const std::vector<uint16_t>& col) {
    for (uint32_t y = 0; y < by; ++y)
        for (uint32_t x = 0; x < bx; ++x) {
            const size_t px = ((static_cast<size_t>(y) * bh) * width + static_cast<size_t>(x) * bw) * 4;
            for (int c = 0; c < 4; ++c) {
                const int want = (col[(static_cast<size_t>(y) * bx + x) * 4 + c] + 128) / 257;
                const int got = rgba[px + c];
                CHECK_MSG(std::abs(got - want) <= 1, "void-extent colour differs");
            }
        }
}

std::vector<fs::path> cache_files(const fs::path& dir) {
    std::vector<fs::path> files;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return files;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".bc7") files.push_back(e.path());
    return files;
}

}  // namespace

// ---- untiling and pass-through ----------------------------------------------------------------------------------------------

NXTEST(untile_rgba8_r8_2d_array_six_levels) {
    no_cache();
    const int formats[] = {NVN_FORMAT_RGBA8, NVN_FORMAT_R8};
    for (int f : formats) {
        // 2D array with 4 layers, and a plain 2D texture: 100x37, 6 levels (odd sizes, the tail levels are tiny)
        for (int target : {NVN_TEXTURE_TARGET_2D_ARRAY, NVN_TEXTURE_TARGET_2D}) {
            const TextureBuilderInfo info = make_info(target, 100, 37, target == NVN_TEXTURE_TARGET_2D_ARRAY ? 4 : 1, 6, f);
            Built bt;
            CHECK_MSG(begin_block_linear(bt, info), "layout");
            fill_random(bt.lin, 1000 + static_cast<uint32_t>(f) + static_cast<uint32_t>(target));
            finish_block_linear(bt, info);
            PreparedTexture out;
            const bool ok = prepare_texture(*bt.tex, TexCaps(), out);
            CHECK_MSG(ok, out.error);
            CHECK_EQ(out.format, bt.L.fmt->vk);
            CHECK_EQ(out.block_w, 1u);
            CHECK_EQ(out.block_bytes, bt.L.fmt->bytes);
            expect_image(bt, out, "untile differs");
        }
    }
}

NXTEST(untile_cube_and_cube_array) {
    no_cache();
    const TextureBuilderInfo cube = make_info(NVN_TEXTURE_TARGET_CUBEMAP, 64, 64, 0, 4, NVN_FORMAT_RGBA8);
    const TextureBuilderInfo cube_array = make_info(NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, 40, 40, 2, 3, NVN_FORMAT_RGBA8);
    for (const TextureBuilderInfo& info : {cube, cube_array}) {
        Built bt;
        CHECK_MSG(begin_block_linear(bt, info), "layout");
        CHECK_EQ(bt.L.layers, info.target == NVN_TEXTURE_TARGET_CUBEMAP ? 6u : 12u);
        fill_random(bt.lin, 77);
        finish_block_linear(bt, info);
        PreparedTexture out;
        CHECK_MSG(prepare_texture(*bt.tex, TexCaps(), out), out.error);
        expect_image(bt, out, "cube data differs");
    }
}

NXTEST(untile_3d_volume_33_cubed) {
    no_cache();
    const TextureBuilderInfo info = make_info(NVN_TEXTURE_TARGET_3D, 33, 33, 33, 6, NVN_FORMAT_RGBA8);
    Built bt;
    CHECK_MSG(begin_block_linear(bt, info), "layout");
    fill_random(bt.lin, 33);
    finish_block_linear(bt, info);
    PreparedTexture out;
    CHECK_MSG(prepare_texture(*bt.tex, TexCaps(), out), out.error);
    CHECK_EQ(out.levels, 6u);
    CHECK_EQ(out.layers, 1u);
    expect_image(bt, out, "3D data differs");
}

NXTEST(passthrough_bc1_and_bc7_unchanged) {
    no_cache();
    const struct {
        int format;
        int w, h;
    } cases[] = {{NVN_FORMAT_RGBA_DXT1, 64, 48}, {NVN_FORMAT_BPTC_UNORM, 37, 21}, {NVN_FORMAT_BPTC_UNORM_SRGB, 64, 64}};
    for (const auto& c : cases) {
        const TextureBuilderInfo info = make_info(NVN_TEXTURE_TARGET_2D, c.w, c.h, 1, 3, c.format);
        Built bt;
        CHECK_MSG(begin_block_linear(bt, info), "layout");
        fill_random(bt.lin, 500 + static_cast<uint32_t>(c.format));   // random blocks: any bytes are a valid BCn block for pass-through
        finish_block_linear(bt, info);
        PreparedTexture out;
        CHECK_MSG(prepare_texture(*bt.tex, TexCaps(), out), out.error);
        CHECK_EQ(out.format, bt.L.fmt->vk);
        CHECK_EQ(out.block_w, 4u);
        CHECK_EQ(out.block_h, 4u);
        CHECK_EQ(out.block_bytes, bt.L.fmt->bytes);
        CHECK_EQ(out.level[0].row_blocks, static_cast<uint32_t>(div_up(c.w, 4)));
        expect_image(bt, out, "BCn data differs");
    }
}

NXTEST(pitch_linear_copies_tight_rows) {
    no_cache();
    // 37 texels of RGBA8 are 148 bytes, the pitch is 160 (32-byte aligned): the copy must drop the padding
    const TextureBuilderInfo info = make_info(NVN_TEXTURE_TARGET_2D, 37, 9, 1, 1, NVN_FORMAT_RGBA8, NVN_TEXTURE_FLAGS_LINEAR);
    Built bt;
    build_pitch(bt, info, 5);
    CHECK_EQ(bt.L.pitch, 160u);
    PreparedTexture out;
    CHECK_MSG(prepare_texture(*bt.tex, TexCaps(), out), out.error);
    CHECK(out.data == bt.lin);
    CHECK_EQ(out.level[0].row_blocks, 37u);
    CHECK_EQ(out.level[0].size, 37u * 4 * 9);
    CHECK(!out.transcoded);
}

// ---- ASTC -------------------------------------------------------------------------------------------------------------------

NXTEST(astc_transcode_bc7_keep_and_decode) {
    no_cache();
    const int formats[] = {NVN_FORMAT_RGBA_ASTC_5x4,      NVN_FORMAT_RGBA_ASTC_5x4_SRGB, NVN_FORMAT_RGBA_ASTC_5x5,
                           NVN_FORMAT_RGBA_ASTC_5x5_SRGB, NVN_FORMAT_RGBA_ASTC_8x5,      NVN_FORMAT_RGBA_ASTC_8x5_SRGB};
    for (int f : formats) {
        const TextureBuilderInfo info = make_info(NVN_TEXTURE_TARGET_2D, 64, 64, 1, 3, f);
        Built bt;
        CHECK_MSG(begin_block_linear(bt, info), "layout");
        std::vector<uint16_t> col0, scratch;
        for (uint32_t i = 0; i < 3; ++i) fill_astc_void_level(bt, i, 0, i == 0 ? col0 : scratch, 900 + i + static_cast<uint32_t>(f));
        finish_block_linear(bt, info);
        const bool srgb = bt.L.fmt->srgb;
        const char* name = bt.L.fmt->name;

        // default caps: BC7 transcode, timed
        PreparedTexture out;
        const auto t0 = std::chrono::steady_clock::now();
        CHECK_MSG(prepare_texture(*bt.tex, TexCaps(), out), out.error);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("texdata: %s 64x64, 3 levels, ASTC -> BC7 in %.2f ms\n", name, ms);
        CHECK_EQ(out.format, srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK);
        CHECK(out.transcoded);
        CHECK(!out.from_cache);
        CHECK_EQ(out.levels, 3u);
        CHECK_EQ(out.layers, 1u);
        CHECK_EQ(out.block_w, 4u);
        CHECK_EQ(out.block_h, 4u);
        CHECK_EQ(out.block_bytes, 16u);
        uint64_t pos = 0;
        uint32_t w = 64, h = 64;
        for (size_t i = 0; i < out.level.size(); ++i) {
            const uint64_t want = div_up(w, 4) * div_up(h, 4) * 16;
            CHECK_EQ(out.level[i].width, w);
            CHECK_EQ(out.level[i].height, h);
            CHECK_EQ(out.level[i].size, want);
            CHECK_EQ(out.level[i].offset, pos);
            CHECK_EQ(out.level[i].row_blocks, static_cast<uint32_t>(div_up(w, 4)));
            pos += want;
            w = std::max(1u, w / 2);
            h = std::max(1u, h / 2);
        }
        CHECK_EQ(out.data.size(), pos);
        expect_bc7_matches_library(bt, out);

        // keep ASTC when the GPU samples it: the data is the untiled source, unchanged
        TexCaps keep;
        keep.astc = true;
        PreparedTexture k;
        CHECK_MSG(prepare_texture(*bt.tex, keep, k), k.error);
        CHECK_EQ(k.format, bt.L.fmt->vk);
        CHECK(!k.transcoded);
        CHECK(k.data == bt.lin);

        // no BC7 transcode: RGBA8 decode, equal to the library's decode, and the constant colours at the block origins
        TexCaps rgba;
        rgba.bc7_transcode = false;
        PreparedTexture d;
        CHECK_MSG(prepare_texture(*bt.tex, rgba, d), d.error);
        CHECK_EQ(d.format, srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM);
        CHECK(d.transcoded);
        CHECK_EQ(d.block_bytes, 4u);
        expect_decode_matches_library(bt, d);
        if (!srgb) expect_void_colours(d.data.data() + d.level[0].offset, 64, bt.L.fmt->block_w, bt.L.fmt->block_h,
                                       static_cast<uint32_t>(bt.L.layout.levels[0].blocks_x),
                                       static_cast<uint32_t>(bt.L.layout.levels[0].blocks_y), col0);
    }
}

NXTEST(astc_odd_size_two_layers) {
    no_cache();
    // 61x37 with 8x5 blocks (partial blocks at both edges), 2 layers, 2 levels
    const TextureBuilderInfo info = make_info(NVN_TEXTURE_TARGET_2D_ARRAY, 61, 37, 2, 2, NVN_FORMAT_RGBA_ASTC_8x5);
    Built bt;
    CHECK_MSG(begin_block_linear(bt, info), "layout");
    std::vector<uint16_t> scratch;
    for (uint32_t layer = 0; layer < 2; ++layer)
        for (uint32_t i = 0; i < 2; ++i) fill_astc_void_level(bt, i, layer, scratch, 40 + layer * 7 + i);
    finish_block_linear(bt, info);

    PreparedTexture out;
    CHECK_MSG(prepare_texture(*bt.tex, TexCaps(), out), out.error);
    CHECK_EQ(out.layers, 2u);
    // level 0: 61x37 -> 16x10 BC7 blocks, 2 layers; level 1: 30x18 -> 8x5 blocks, 2 layers
    CHECK_EQ(out.level[0].size, 16u * 10 * 16 * 2);
    CHECK_EQ(out.level[1].size, 8u * 5 * 16 * 2);
    CHECK_EQ(out.level[1].offset, out.level[0].size);
    CHECK_EQ(out.data.size(), out.level[0].size + out.level[1].size);
    expect_bc7_matches_library(bt, out);

    TexCaps rgba;
    rgba.bc7_transcode = false;
    PreparedTexture d;
    CHECK_MSG(prepare_texture(*bt.tex, rgba, d), d.error);
    expect_decode_matches_library(bt, d);
}

// ---- disk cache -------------------------------------------------------------------------------------------------------------

NXTEST(bc7_disk_cache_hit_and_truncated_file) {
    const fs::path dir = fs::temp_directory_path() /
                         ("nxrt_texdata_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::create_directories(dir, ec);
    set_env("NXRT_NVN_BC7_CACHE", dir.string().c_str());
    CHECK_EQ(config_get_str("nvn", "bc7_cache", "x"), dir.string());

    const TextureBuilderInfo info = make_info(NVN_TEXTURE_TARGET_2D, 64, 64, 1, 3, NVN_FORMAT_RGBA_ASTC_5x4_SRGB);
    Built bt;
    CHECK_MSG(begin_block_linear(bt, info), "layout");
    std::vector<uint16_t> scratch;
    for (uint32_t i = 0; i < 3; ++i) fill_astc_void_level(bt, i, 0, scratch, 300 + i);
    finish_block_linear(bt, info);

    PreparedTexture a;
    CHECK_MSG(prepare_texture(*bt.tex, TexCaps(), a), a.error);
    CHECK(!a.from_cache);
    const std::vector<fs::path> files = cache_files(dir);
    CHECK_EQ(files.size(), 1u);
    if (files.size() != 1) {
        set_env("NXRT_NVN_BC7_CACHE", "");
        fs::remove_all(dir, ec);
        return;
    }
    // file = 28-byte header + 28 bytes per level + the data
    CHECK_EQ(fs::file_size(files[0]), 28u + 3 * 28 + a.data.size());

    PreparedTexture b;
    CHECK_MSG(prepare_texture(*bt.tex, TexCaps(), b), b.error);
    CHECK(b.from_cache);
    CHECK(b.data == a.data);
    CHECK(b.level.size() == a.level.size());

    // a truncated file is ignored: the texture is transcoded again and the file is rewritten whole
    fs::resize_file(files[0], fs::file_size(files[0]) / 2, ec);
    PreparedTexture c;
    CHECK_MSG(prepare_texture(*bt.tex, TexCaps(), c), c.error);
    CHECK(!c.from_cache);
    CHECK(c.data == a.data);
    CHECK_EQ(fs::file_size(files[0]), 28u + 3 * 28 + a.data.size());

    // a file with a wrong magic is ignored the same way
    {
        std::fstream f(files[0], std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(0);
        f.put('X');
    }
    PreparedTexture e;
    CHECK_MSG(prepare_texture(*bt.tex, TexCaps(), e), e.error);
    CHECK(!e.from_cache);
    CHECK(e.data == a.data);

    // with the cache disabled nothing is read or written
    set_env("NXRT_NVN_BC7_CACHE", "");
    config_set("nvn", "bc7_cache", "");
    PreparedTexture g;
    CHECK_MSG(prepare_texture(*bt.tex, TexCaps(), g), g.error);
    CHECK(!g.from_cache);
    CHECK_EQ(cache_files(dir).size(), 1u);

    fs::remove_all(dir, ec);
    no_cache();
}

// ---- hash -------------------------------------------------------------------------------------------------------------------

NXTEST(hash_bytes_separates_inputs) {
    std::vector<uint8_t> v(100);
    fill_random(v, 7);
    const uint64_t h = hash_bytes(v.data(), v.size());
    CHECK_EQ(h, hash_bytes(v.data(), v.size()));
    // every prefix length hashes differently
    std::vector<uint64_t> prefixes;
    for (size_t n = 0; n <= v.size(); ++n) prefixes.push_back(hash_bytes(v.data(), n));
    std::sort(prefixes.begin(), prefixes.end());
    CHECK(std::adjacent_find(prefixes.begin(), prefixes.end()) == prefixes.end());
    // flipping one bit anywhere changes the hash
    for (size_t i = 0; i < v.size(); ++i) {
        v[i] ^= 0x10;
        CHECK_MSG(hash_bytes(v.data(), v.size()) != h, "one bit did not change the hash");
        v[i] ^= 0x10;
    }
}

// ---- caps and failures ------------------------------------------------------------------------------------------------------

NXTEST(failures_return_false_without_crashing) {
    no_cache();
    std::vector<uint8_t> buf;
    PreparedTexture out;

    // no storage
    {
        Built bt;
        CHECK(begin_block_linear(bt, make_info(NVN_TEXTURE_TARGET_2D, 16, 16, 1, 1, NVN_FORMAT_RGBA8)));
        finish_block_linear(bt, make_info(NVN_TEXTURE_TARGET_2D, 16, 16, 1, 1, NVN_FORMAT_RGBA8));
        bt.tex->mem = nullptr;
        CHECK(!prepare_texture(*bt.tex, TexCaps(), out));
        CHECK(!out.error.empty());
    }
    // storage smaller than the layout
    {
        const TextureBuilderInfo info = make_info(NVN_TEXTURE_TARGET_2D, 16, 16, 1, 1, NVN_FORMAT_RGBA8);
        Ref<Texture> t = bare_texture(info, buf);
        t->storage_size = 16 * 16 * 4 - 1;
        CHECK(!prepare_texture(*t, TexCaps(), out));
        CHECK(!out.error.empty());
    }
    // layouts that do not exist
    const TextureBuilderInfo bad[] = {
        make_info(NVN_TEXTURE_TARGET_2D, 16, 16, 1, 1, NVN_FORMAT_NONE),
        make_info(NVN_TEXTURE_TARGET_3D, 33, 33, 0, 1, NVN_FORMAT_RGBA8),
        make_info(NVN_TEXTURE_TARGET_BUFFER, 64, 1, 1, 1, NVN_FORMAT_RGBA8),
        make_info(NVN_TEXTURE_TARGET_2D, 0, 16, 1, 1, NVN_FORMAT_RGBA8),
        make_info(NVN_TEXTURE_TARGET_2D_ARRAY, 16, 16, 0, 1, NVN_FORMAT_RGBA8),
    };
    for (const TextureBuilderInfo& info : bad) {
        Ref<Texture> t = bare_texture(info, buf);
        CHECK(!prepare_texture(*t, TexCaps(), out));
        CHECK(!out.error.empty());
    }
    // multisample with samples: a real layout, but not supported
    {
        TextureBuilderInfo info = make_info(NVN_TEXTURE_TARGET_2D_MULTISAMPLE, 16, 16, 1, 1, NVN_FORMAT_RGBA8);
        info.samples = 4;
        Ref<Texture> t = bare_texture(info, buf);
        CHECK(!prepare_texture(*t, TexCaps(), out));
        CHECK(!out.error.empty());
    }
    // BCn without BCn support in the GPU (there is no CPU decoder)
    {
        Ref<Texture> t = bare_texture(make_info(NVN_TEXTURE_TARGET_2D, 16, 16, 1, 1, NVN_FORMAT_RGBA_DXT1), buf);
        TexCaps none;
        none.bc = false;
        CHECK(!prepare_texture(*t, none, out));
        CHECK(!out.error.empty());
    }
    // a bad texture object (never initialised) is refused
    {
        Texture empty;
        CHECK(!prepare_texture(empty, TexCaps(), out));
        CHECK(!out.error.empty());
    }
}

NXTEST(caps_select_output_format) {
    no_cache();
    const TextureBuilderInfo info = make_info(NVN_TEXTURE_TARGET_2D, 32, 32, 1, 1, NVN_FORMAT_RGBA_ASTC_5x5);
    Built bt;
    CHECK(begin_block_linear(bt, info));
    std::vector<uint16_t> scratch;
    fill_astc_void_level(bt, 0, 0, scratch, 11);
    finish_block_linear(bt, info);

    PreparedTexture out;
    CHECK_MSG(prepare_texture(*bt.tex, TexCaps(), out), out.error);
    CHECK_EQ(out.format, VK_FORMAT_BC7_UNORM_BLOCK);
    TexCaps rgba;
    rgba.bc7_transcode = false;
    CHECK_MSG(prepare_texture(*bt.tex, rgba, out), out.error);
    CHECK_EQ(out.format, VK_FORMAT_R8G8B8A8_UNORM);
    CHECK_EQ(out.level[0].size, 32u * 32 * 4);
    TexCaps astc;
    astc.astc = true;
    CHECK_MSG(prepare_texture(*bt.tex, astc, out), out.error);
    CHECK_EQ(out.format, VK_FORMAT_ASTC_5x5_UNORM_BLOCK);
    CHECK_EQ(out.block_w, 5u);
    CHECK_EQ(out.block_h, 5u);
    CHECK_EQ(out.block_bytes, 16u);
}
