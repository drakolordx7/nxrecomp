// SPDX-License-Identifier: GPL-3.0-or-later
// S44 glow colour (games/mpr/play_glow.cpp): the colour math, the glow textures' identity and the shared texture edit hook.
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef near
#undef far

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

// ---- glow texture edit ids (S44c): the edited data must follow the texture's content, not only its tail ------------------------------

NXTEST(glow_texture_id_changes_with_level_0_and_keeps_the_row) {
    using namespace nxrt::play;
    std::vector<uint8_t> tex(1 << 20);
    for (size_t i = 0; i < tex.size(); ++i) tex[i] = static_cast<uint8_t>(i * 131 + (i >> 9));
    std::vector<uint8_t> early = tex;
    std::fill(early.begin(), early.begin() + early.size() / 2, uint8_t{0});   // level 0 not streamed in yet: same tail
    CHECK_EQ(glow_texture_tail_hash(early.data(), early.size(), 2048, 2048, 12), glow_texture_tail_hash(tex.data(), tex.size(), 2048, 2048, 12));
    const uint64_t a = glow_texture_id(3, tex.data(), tex.size());
    const uint64_t b = glow_texture_id(3, early.data(), early.size());
    CHECK(a != b);                                       // the complete texture never gets the data edited from the early upload
    CHECK_EQ(a, glow_texture_id(3, tex.data(), tex.size()));   // the same content keeps its id (its edited data is reused)
    CHECK_EQ(glow_texture_row(a), 3);
    CHECK_EQ(glow_texture_row(b), 3);
    CHECK(a < (1ull << 55) && b < (1ull << 55));         // below the discovery ids and the texture edit hook's client bits
    CHECK(glow_texture_id(0, tex.data(), tex.size()) != glow_texture_id(1, tex.data(), tex.size()));
    CHECK_EQ(glow_texture_row(glow_texture_id(0, nullptr, 0)), 0);
}

NXTEST(glow_texture_row_rejects_discovery_and_unknown_ids) {
    using namespace nxrt::play;
    size_t n = 0;
    glow_textures(&n);
    CHECK_EQ(glow_texture_row(0), -1);
    CHECK_EQ(glow_texture_row((1ull << 55) | 1001), -1);   // NXRT_GLOW_LOG_TEXTURES discovery ids change nothing
    CHECK_EQ(glow_texture_row((1ull << 55) | 1), -1);
    CHECK_EQ(glow_texture_row(static_cast<uint64_t>(n) + 1), -1);   // past the table
    CHECK_EQ(glow_texture_row(static_cast<uint64_t>(n)), static_cast<int>(n) - 1);
}

NXTEST(glow_textures_table_has_every_suit_and_not_the_hangar_lights) {
    using namespace nxrt::play;
    size_t n = 0;
    const GlowTexture* t = glow_textures(&n);
    int suit_bodies = 0, suit_arms = 0, gun = 0, small = 0;
    for (size_t i = 0; i < n; ++i) {
        // the Gravity Suit's shoulder stripes (7f5c390c) and the Phazon Suit's glow ramp (b9513805), uploaded only while that suit is worn
        if (t[i].part == GlowTexPart::Suit && ((t[i].w == 8 && t[i].h == 8 && t[i].tail == 0x6b0345201e4c882bull) ||
                                                (t[i].w == 128 && t[i].h == 4 && t[i].tail == 0x5b14cbbac87c70bbull)))
            ++small;
        // the 1024 x 1024 glow texture that the suit shader also samples is shared with the hangar's signal lights: never recoloured
        CHECK(t[i].tail != 0x315648d5a9d07890ull);
        CHECK(!(t[i].w == 1024 && t[i].h == 1024));
        for (size_t j = 0; j < i; ++j) CHECK(t[j].tail != t[i].tail);
        if (t[i].part == GlowTexPart::Suit && t[i].w == 2048 && t[i].h == 2048) ++suit_bodies;
        if (t[i].part == GlowTexPart::Suit && t[i].w == 2048 && t[i].h == 1024) ++suit_arms;
        if (t[i].part == GlowTexPart::Gun) ++gun;
    }
    CHECK_EQ(suit_bodies, 4);   // Power, Varia, Gravity, Phazon
    CHECK_EQ(suit_arms, 4);
    CHECK_EQ(small, 2);
    CHECK_EQ(gun, 2);
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

// S47: the ball colour tables are written only from the thread that runs the game's frame boundary (the first caller after install).
NXTEST(glow_ball_tables_are_written_only_from_the_frame_thread) {
    constexpr uint64_t kBase = 0x5D00000000ull;
    constexpr uint64_t kTables[5] = {0x4b24a48, 0x4b24a98, 0x4b24ae8, 0x4b24b38, 0x4b24b88};
    REQUIRE(VirtualAlloc(reinterpret_cast<void*>(kBase + 0x4b24000), 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != nullptr);
    auto table = [&](int i, int e, int k) { return reinterpret_cast<volatile float*>(kBase + kTables[i] + 16u * e)[k]; };
    for (int i = 0; i < 5; ++i)
        for (int e = 0; e < 5; ++e) {
            const float v[4] = {0.5f, 0.25f, 0.125f, 1.f};
            std::memcpy(reinterpret_cast<void*>(kBase + kTables[i] + 16u * e), v, 16);
        }
    nxrt::play::glow_install(kBase);
    nxrt::play::TweakSettings t;
    t.glow_on = true;
    t.glow = GlowRgb{255, 0, 0};
    t.ball.mode = nxrt::play::GlowMode::Color;
    t.ball.rgb = GlowRgb{255, 0, 0};
    nxrt::play::glow_set(t);
    // the thread that calls first after install owns the tables
    nxrt::play::glow_frame();
    CHECK(nxrt::play::glow_stats().ball_written);
    CHECK(table(0, 0, 0) == 0.5f && table(0, 0, 1) == 0.f && table(0, 0, 3) == 1.f);   // red at the game's brightest channel, alpha kept
    t.ball.rgb = GlowRgb{0, 0, 255};
    nxrt::play::glow_set(t);
    std::thread other([] { nxrt::play::glow_frame(); });
    other.join();
    CHECK(table(0, 0, 0) == 0.5f && table(0, 0, 2) == 0.f);                               // unchanged by the stranger
    nxrt::play::glow_frame();                                                              // the owner applies it
    CHECK(table(0, 0, 0) == 0.f && table(0, 0, 2) == 0.5f);
    nxrt::play::glow_remove();
}

