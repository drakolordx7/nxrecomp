// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the texture upload hook (nxrt/tex_upload.h, src/gfx/nvn/nvn_tex_upload.cpp) through prepare_texture: the host sees the decoded levels,
// its edit reaches the output (RGBA8 compared texel by texel, BC7 by size and difference), the edited data is kept and reused per generation,
// it never reaches the BC7 disk cache, and textures the host does not match come out as before. Several subscribers keep their ids apart; a small BC7
// texture is replaced whole by the subscriber.
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#include <nxrt/config.h>
#include <nxrt/tex_upload.h>

#include "nvn_settings.h"
#include "nvn_core.h"
#include "nvn_format.h"
#include "nvn_texdata.h"
#include "nvn_texture_layout.h"
#include "tegra_tiling.h"
#include "test_framework.h"
NXTEST_GROUP(gfx_nvn_tex_upload)

using namespace nxrt;
using namespace nxrt::nvn;
using namespace nxrt::nvn::be;
namespace fs = std::filesystem;

namespace {

void set_env(const char* name, const char* value) { _putenv_s(name, value); }

struct Built {
    Ref<Texture> tex;
    TexLayout L;
    std::vector<uint8_t> lin, mem;
};

// 64x40 ASTC 8x5 UNORM, 3 levels, every block a constant colour (void-extent block) of (r, g, b) = (4x, 4y, 200) per block, so levels differ.
void build(Built& bt, int w = 64, int h = 40) {
    TextureBuilderInfo b;
    b.target = NVN_TEXTURE_TARGET_2D;
    b.width = w;
    b.height = h;
    b.depth = 1;
    b.levels = 3;
    b.format = NVN_FORMAT_RGBA_ASTC_8x5;
    bt.L = compute_texture_layout(b);
    REQUIRE(bt.L.valid && bt.L.block_linear);
    bt.lin.assign(bt.L.layout.linear_size, 0);
    for (const tegra::LevelLayout& lv : bt.L.layout.levels)
        for (uint32_t y = 0; y < lv.blocks_y; ++y)
            for (uint32_t x = 0; x < lv.blocks_x; ++x) {
                uint8_t* blk = bt.lin.data() + lv.linear_offset + y * lv.linear_row_stride + x * 16;
                blk[0] = 0xFC;
                blk[1] = 0xFD;
                std::memset(blk + 2, 0xFF, 6);
                const uint16_t c[4] = {static_cast<uint16_t>(4 * x * 257), static_cast<uint16_t>(4 * y * 257), 200 * 257, 0xFFFF};
                std::memcpy(blk + 8, c, 8);
            }
    bt.mem.assign(bt.L.storage_size, 0);
    tegra::tile(bt.L.desc, bt.L.layout, bt.lin.data(), bt.mem.data());
    bt.tex = Ref<Texture>(new Texture());
    Texture& t = *bt.tex;
    t.b = b;
    t.fmt = bt.L.fmt;
    t.mem = bt.mem.data();
    t.storage_size = bt.L.storage_size;
    t.block_linear = true;
    t.levels = bt.L.levels;
    t.layers = bt.L.layers;
    t.block_height_gobs = bt.L.layout.levels[0].gob_blocks_y;
}

struct Host {
    uint64_t gen = 1;
    uint32_t want_w = 64;
    int levels_seen = 0;
    int calls = 0;
};
Host g_host;

uint64_t h_match(const gfx::TexUploadReq& r, void* u) {
    Host* h = static_cast<Host*>(u);
    return r.astc && r.width == h->want_w && r.levels == 3 && r.block_w == 8 && r.block_h == 5 && r.tiled && r.tiled_size > 0 ? 42 : 0;
}
uint64_t h_gen(void* u) { return static_cast<Host*>(u)->gen; }
bool h_edit(uint64_t id, uint32_t level, uint32_t w, uint32_t h, uint8_t* rgba, void* u) {
    Host* hs = static_cast<Host*>(u);
    CHECK_EQ(id, 42u);
    CHECK_EQ(level, static_cast<uint32_t>(hs->levels_seen % 3));   // level 0 first, then 1, 2
    hs->levels_seen++;
    hs->calls++;
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {   // invert the colour, keep alpha
        rgba[i * 4 + 0] = static_cast<uint8_t>(255 - rgba[i * 4 + 0]);
        rgba[i * 4 + 1] = static_cast<uint8_t>(255 - rgba[i * 4 + 1]);
        rgba[i * 4 + 2] = static_cast<uint8_t>(255 - rgba[i * 4 + 2]);
    }
    return true;
}

size_t files_in(const fs::path& d) {
    size_t n = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(d, ec); it != fs::recursive_directory_iterator(); ++it)
        if (it->is_regular_file()) ++n;
    return n;
}

}  // namespace

NXTEST(edit_reaches_rgba8_output_and_is_reused) {
    set_env("NXRT_NVN_BC7_CACHE", "");
    nxrt::nvn::be::nvn_settings_reload();   // the [nvn] settings are read once; take the change
    config_set("nvn", "bc7_cache", "");
    nxrt::nvn::be::nvn_settings_reload();   // the [nvn] settings are read once; take the change
    Built bt;
    build(bt);
    TexCaps rgba;
    rgba.bc7_transcode = false;
    PreparedTexture plain;
    REQUIRE(prepare_texture(*bt.tex, rgba, plain));

    g_host = Host{};
    gfx::TexUploadHooks hk;
    hk.match = &h_match;
    hk.generation = &h_gen;
    hk.edit = &h_edit;
    hk.user = &g_host;
    const int sub = gfx::texture_upload_subscribe(hk);
    REQUIRE(sub > 0);

    PreparedTexture ed;
    REQUIRE(prepare_texture(*bt.tex, rgba, ed));
    CHECK_EQ(g_host.calls, 3);   // three levels
    REQUIRE(ed.data.size() == plain.data.size());
    CHECK(ed.format == plain.format);
    bool all_inverted = true;
    for (size_t i = 0; i < plain.data.size(); ++i) {
        const uint8_t want = (i % 4 == 3) ? plain.data[i] : static_cast<uint8_t>(255 - plain.data[i]);
        if (ed.data[i] != want) all_inverted = false;
    }
    CHECK(all_inverted);

    // the same generation: kept data, no new edit
    const gfx::TexUploadStats s0 = gfx::texture_upload_stats();
    PreparedTexture again;
    REQUIRE(prepare_texture(*bt.tex, rgba, again));
    CHECK_EQ(g_host.calls, 3);
    CHECK(again.data == ed.data);
    CHECK_EQ(gfx::texture_upload_stats().reuses, s0.reuses + 1);

    // a new generation: edited again
    g_host.gen = 2;
    PreparedTexture third;
    REQUIRE(prepare_texture(*bt.tex, rgba, third));
    CHECK_EQ(g_host.calls, 6);
    CHECK(third.data == ed.data);

    // a texture the host does not match comes out as before
    g_host.want_w = 32;
    PreparedTexture other;
    REQUIRE(prepare_texture(*bt.tex, rgba, other));
    CHECK(other.data == plain.data);
    CHECK_EQ(g_host.calls, 6);

    gfx::texture_upload_unsubscribe(sub);
    PreparedTexture off;
    REQUIRE(prepare_texture(*bt.tex, rgba, off));
    CHECK(off.data == plain.data);
}

// BC7 output: the edit changes the blocks, the sizes stay, and the shared disk cache gets nothing from the edit.
NXTEST(edited_bc7_stays_out_of_the_disk_cache) {
    const fs::path dir = fs::temp_directory_path() / ("nxrt_texedit_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::create_directories(dir, ec);
    set_env("NXRT_NVN_BC7_CACHE", dir.string().c_str());
    nxrt::nvn::be::nvn_settings_reload();   // the [nvn] settings are read once; take the change
    Built bt;
    build(bt);

    g_host = Host{};
    gfx::TexUploadHooks hk;
    hk.match = &h_match;
    hk.generation = &h_gen;
    hk.edit = &h_edit;
    hk.user = &g_host;
    const int sub = gfx::texture_upload_subscribe(hk);
    REQUIRE(sub > 0);
    PreparedTexture ed;
    REQUIRE(prepare_texture(*bt.tex, TexCaps(), ed));
    CHECK(ed.format == VK_FORMAT_BC7_UNORM_BLOCK);
    CHECK(!ed.from_cache);
    CHECK_EQ(g_host.calls, 3);
    CHECK_EQ(files_in(dir), 0u);   // nothing written for the edited texture

    gfx::texture_upload_unsubscribe(sub);
    PreparedTexture plain;
    REQUIRE(prepare_texture(*bt.tex, TexCaps(), plain));
    CHECK_EQ(files_in(dir), 1u);   // the game's own data is cached as always
    CHECK_EQ(plain.data.size(), ed.data.size());
    CHECK(plain.data != ed.data);
    PreparedTexture cached;
    REQUIRE(prepare_texture(*bt.tex, TexCaps(), cached));
    CHECK(cached.from_cache);
    CHECK(cached.data == plain.data);

    // with the hooks back, the cache entry of the game's data is not used for the edited texture
    g_host.gen = 5;
    const int sub2 = gfx::texture_upload_subscribe(hk);
    REQUIRE(sub2 > 0);
    PreparedTexture ed2;
    REQUIRE(prepare_texture(*bt.tex, TexCaps(), ed2));
    CHECK(!ed2.from_cache);
    CHECK(ed2.data == ed.data);
    gfx::texture_upload_unsubscribe(sub2);

    set_env("NXRT_NVN_BC7_CACHE", "");
    nxrt::nvn::be::nvn_settings_reload();   // the [nvn] settings are read once; take the change
    fs::remove_all(dir, ec);
}

// Two subscribers: the first that matches wins, and each gets its own ids back (the slot the runtime adds in the top byte is not seen).
namespace {
struct Seen {
    uint64_t id = 0;
    int calls = 0;
};
uint64_t m_any64(const gfx::TexUploadReq& r, void*) { return r.astc && r.width == 64 ? 7 : 0; }
uint64_t m_any32(const gfx::TexUploadReq& r, void*) { return r.astc && (r.width == 64 || r.width == 32) ? 9 : 0; }
bool e_seen(uint64_t id, uint32_t, uint32_t, uint32_t, uint8_t*, void* u) {
    Seen* s = static_cast<Seen*>(u);
    s->id = id;
    s->calls++;
    return true;
}
}  // namespace

NXTEST(subscribers_are_asked_in_order_and_see_their_own_ids) {
    set_env("NXRT_NVN_BC7_CACHE", "");
    nxrt::nvn::be::nvn_settings_reload();
    Built bt;
    build(bt);
    TexCaps rgba;
    rgba.bc7_transcode = false;
    Seen a, b;
    gfx::TexUploadHooks ha, hb;
    ha.match = &m_any64;
    ha.edit = &e_seen;
    ha.user = &a;
    hb.match = &m_any32;
    hb.edit = &e_seen;
    hb.user = &b;
    const int sa = gfx::texture_upload_subscribe(ha);
    const int sb = gfx::texture_upload_subscribe(hb);
    REQUIRE(sa > 0);
    REQUIRE(sb > 0);
    CHECK(sa != sb);
    PreparedTexture out;
    REQUIRE(prepare_texture(*bt.tex, rgba, out));
    CHECK_EQ(a.calls, 3);   // the first subscriber took the 64 wide texture, all three levels
    CHECK_EQ(a.id, 7u);
    CHECK_EQ(b.calls, 0);
    gfx::texture_upload_unsubscribe(sa);   // the second one takes it over, with its own id
    PreparedTexture out2;
    REQUIRE(prepare_texture(*bt.tex, rgba, out2));
    CHECK_EQ(b.calls, 3);
    CHECK_EQ(b.id, 9u);
    gfx::texture_upload_unsubscribe(sb);
    gfx::texture_upload_unsubscribe(sb);   // twice: nothing
}
