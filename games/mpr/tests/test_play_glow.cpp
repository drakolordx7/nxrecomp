// SPDX-License-Identifier: GPL-3.0-or-later
// S44 glow colour (games/mpr/play_glow.cpp): the colour math, the glow textures' identity and the shared texture edit hook.
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "mpr/mpr_texedit.h"
#include "mpr/play_glow.h"

NXTEST_GROUP(gfx_play_glow)

namespace {

using nxrt::play::glow_recolor;
using nxrt::play::glow_srgb_to_linear;
using nxrt::play::GlowRgb;

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

}  // namespace

NXTEST(glow_srgb_to_linear_ends_and_middle) {
    CHECK(glow_srgb_to_linear(0) == 0.f);
    CHECK(near(glow_srgb_to_linear(255), 1.f));
    CHECK(near(glow_srgb_to_linear(128), 0.21586f));
    CHECK(near(glow_srgb_to_linear(10), 10.f / 255.f / 12.92f));
}

NXTEST(glow_recolor_keeps_peak_and_alpha) {
    const float power[4] = {0.8f, 0.4f, 0.05f, 0.75f};   // an orange of the game's, linear, with its alpha
    float out[4];
    glow_recolor(power, GlowRgb{0, 0, 255}, out);
    CHECK(near(out[0], 0.f) && near(out[1], 0.f));
    CHECK(near(out[2], 0.8f));     // the game's brightest channel
    CHECK(out[3] == 0.75f);
    glow_recolor(power, GlowRgb{255, 255, 255}, out);
    CHECK(near(out[0], 0.8f) && near(out[1], 0.8f) && near(out[2], 0.8f));
    glow_recolor(power, GlowRgb{128, 0, 0}, out);
    CHECK(near(out[0], 0.8f * 0.21586f));
}

NXTEST(glow_recolor_black_stays_black) {
    const float black[4] = {0.f, 0.f, 0.f, 1.f};
    float out[4];
    glow_recolor(black, GlowRgb{255, 0, 0}, out);
    CHECK(out[0] == 0.f && out[1] == 0.f && out[2] == 0.f && out[3] == 1.f);
}

NXTEST(glow_recolor_hdr_peak) {
    const float hdr[4] = {4.f, 2.f, 1.f, 1.f};   // particle colours can exceed 1
    float out[4];
    glow_recolor(hdr, GlowRgb{0, 255, 0}, out);
    CHECK(near(out[1], 4.f) && out[0] == 0.f && out[2] == 0.f);
}

// ---- glow ramp texels (S44b): the cannon's panel ramps are sRGB textures recoloured texel by texel -----------------------------------

NXTEST(glow_linear_to_srgb_round_trips_bytes) {
    for (int v = 0; v < 256; ++v) CHECK_EQ(static_cast<int>(nxrt::play::glow_linear_to_srgb(glow_srgb_to_linear(static_cast<uint8_t>(v)))), v);
    CHECK_EQ(static_cast<int>(nxrt::play::glow_linear_to_srgb(-1.f)), 0);
    CHECK_EQ(static_cast<int>(nxrt::play::glow_linear_to_srgb(4.f)), 255);
}

NXTEST(glow_recolor_texel_replaces_the_hue_and_keeps_the_brightness) {
    using nxrt::play::glow_recolor_texel;
    // the Power Beam ramp's bright end (yellow) takes the setting's colour exactly
    uint8_t px[4] = {255, 219, 43, 200};
    glow_recolor_texel(px, true, GlowRgb{255, 0, 255});
    CHECK(px[0] == 255 && px[1] == 0 && px[2] == 255 && px[3] == 200);
    // a darker texel keeps its brightness: its brightest channel in linear light becomes the setting's
    uint8_t dark[4] = {120, 66, 10, 255};
    glow_recolor_texel(dark, true, GlowRgb{0, 0, 255});
    CHECK(dark[0] == 0 && dark[1] == 0 && dark[2] == 120 && dark[3] == 255);
    // black stays black
    uint8_t black[4] = {0, 0, 0, 255};
    glow_recolor_texel(black, true, GlowRgb{255, 255, 255});
    CHECK(black[0] == 0 && black[1] == 0 && black[2] == 0);
    // a linear (UNORM) texture is scaled without the sRGB curve
    uint8_t lin[4] = {200, 100, 0, 255};
    glow_recolor_texel(lin, false, GlowRgb{0, 255, 0});
    CHECK(lin[0] == 0 && lin[1] == 200 && lin[2] == 0);
}

// ---- glow texture identity (S44b): the game streams a glow map's level 0 in after its smaller levels -------------------------------

NXTEST(glow_texture_tail_hash_ignores_level_0_and_sees_the_tail) {
    using nxrt::play::glow_texture_tail_hash;
    std::vector<uint8_t> tex(1 << 20);
    for (size_t i = 0; i < tex.size(); ++i) tex[i] = static_cast<uint8_t>(i * 131 + (i >> 9));
    const uint64_t full = glow_texture_tail_hash(tex.data(), tex.size(), 2048, 2048, 12);
    std::vector<uint8_t> early = tex;
    std::fill(early.begin(), early.begin() + early.size() / 2, uint8_t{0});   // level 0 not loaded yet
    CHECK_EQ(glow_texture_tail_hash(early.data(), early.size(), 2048, 2048, 12), full);
    std::vector<uint8_t> other = tex;
    other[other.size() - 100] ^= 1;   // another texture: its small levels differ
    CHECK(glow_texture_tail_hash(other.data(), other.size(), 2048, 2048, 12) != full);
    CHECK(glow_texture_tail_hash(tex.data(), tex.size(), 2048, 1024, 12) != full);   // the shape is part of it
    // a small texture is hashed whole
    const uint64_t small = glow_texture_tail_hash(tex.data(), 2048, 128, 16, 8);
    std::vector<uint8_t> s2(tex.begin(), tex.begin() + 2048);
    s2[0] ^= 1;
    CHECK(glow_texture_tail_hash(s2.data(), s2.size(), 128, 16, 8) != small);
    CHECK_EQ(glow_texture_tail_hash(nullptr, 0, 1, 1, 1), uint64_t{0});
}

// ---- the shared texture edit hook (mpr_texedit.h): the tint and the glow colour each get their own textures -------------------------

namespace {
uint64_t tint_m(const nxrt::gfx::TexEditReq& r, void*) { return r.width == 1 ? 7 : 0; }
uint64_t glow_m(const nxrt::gfx::TexEditReq& r, void*) { return r.width <= 2 ? 3 : 0; }
uint64_t tint_g(void*) { return 10; }
uint64_t glow_g(void*) { return 5; }
int tint_edits = 0, glow_edits = 0;
uint64_t last_id = 0;
void tint_e(uint64_t id, uint32_t, uint32_t, uint32_t, uint8_t*, void*) { ++tint_edits; last_id = id; }
void glow_e(uint64_t id, uint32_t, uint32_t, uint32_t, uint8_t*, void*) { ++glow_edits; last_id = id; }
}  // namespace

NXTEST(texedit_dispatch_routes_ids_to_their_client) {
    using namespace nxrt::play;
    nxrt::gfx::TexEditHooks t{&tint_m, &tint_g, &tint_e, nullptr}, gl{&glow_m, &glow_g, &glow_e, nullptr};
    texedit_set(TexEditClient::Tint, &t);
    texedit_set(TexEditClient::Glow, &gl);
    nxrt::gfx::TexEditReq r;
    r.width = 1;
    const uint64_t a = texedit::match(r, nullptr);   // both want it: the tint comes first
    r.width = 2;
    const uint64_t b = texedit::match(r, nullptr);
    r.width = 3;
    CHECK_EQ(texedit::match(r, nullptr), uint64_t{0});
    CHECK(a != 0 && b != 0 && a != b);
    CHECK_EQ(texedit::generation(nullptr), uint64_t{15});
    texedit::edit(a, 0, 1, 1, nullptr, nullptr);
    CHECK(tint_edits == 1 && glow_edits == 0 && last_id == 7);
    texedit::edit(b, 0, 1, 1, nullptr, nullptr);
    CHECK(tint_edits == 1 && glow_edits == 1 && last_id == 3);
    texedit::edit(3, 0, 1, 1, nullptr, nullptr);   // no client slot: ignored
    CHECK(tint_edits == 1 && glow_edits == 1);
    texedit_set(TexEditClient::Glow, nullptr);
    CHECK_EQ(texedit::match(r, nullptr), uint64_t{0});
    r.width = 2;
    CHECK_EQ(texedit::match(r, nullptr), uint64_t{0});
    CHECK_EQ(texedit::generation(nullptr), uint64_t{10});
    texedit_set(TexEditClient::Tint, nullptr);   // the last client: the runtime hook goes
}
