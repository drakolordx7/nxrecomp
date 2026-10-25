// SPDX-License-Identifier: GPL-3.0-or-later
// S45: the suit and arm cannon colour (games/mpr/mpr_tint.cpp): the paint mask, the recolour, the paint reference, the texture table and the
// [tweaks] suit_color / gun_color keys. Synthetic texels only; the game's textures are checked in game (docs/S45.md).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "test_framework.h"

#include "mpr/mpr_tint.h"
#include "play/play_tweaks.h"

NXTEST_GROUP(play_tint)

namespace {

namespace play = nxrt::play;
namespace tint = nxrt::play::tint;
using play::GlowRgb;
using tint::Profile;

std::vector<uint8_t> image(uint32_t n, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    std::vector<uint8_t> v(static_cast<size_t>(n) * 4);
    for (uint32_t i = 0; i < n; ++i) {
        v[i * 4 + 0] = r;
        v[i * 4 + 1] = g;
        v[i * 4 + 2] = b;
        v[i * 4 + 3] = a;
    }
    return v;
}

double lin(uint8_t c) {
    const double x = c / 255.0;
    return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
}
double luma(const uint8_t* p) { return 0.2126 * lin(p[0]) + 0.7152 * lin(p[1]) + 0.0722 * lin(p[2]); }

}  // namespace

// The paint of each suit is inside its window; the visor glass and the lights are outside every window.
NXTEST(mask_takes_paint_and_leaves_lights) {
    // Power and Varia paint: red, orange, beige (values from the decoded textures)
    CHECK(tint::weight(170, 70, 75, Profile::Warm) > 0.95f);    // Power Suit red-pink
    CHECK(tint::weight(190, 110, 60, Profile::Warm) > 0.95f);   // Varia orange
    CHECK(tint::weight(205, 180, 130, Profile::Warm) > 0.9f);   // beige panels
    CHECK(tint::weight(140, 70, 40, Profile::Warm) > 0.95f);    // brown-red under parts
    // lights and glass stay
    CHECK_EQ(tint::weight(90, 220, 60, Profile::Warm), 0.f);    // green visor and lights
    CHECK_EQ(tint::weight(40, 200, 230, Profile::Warm), 0.f);   // cyan light
    CHECK_EQ(tint::weight(150, 220, 40, Profile::Warm), 0.f);   // lime ball stripes
    CHECK_EQ(tint::weight(110, 90, 200, Profile::Warm), 0.f);   // purple is the Gravity Suit's, not Power's
    // greys, black seams and near-black shadows stay
    CHECK_EQ(tint::weight(128, 128, 128, Profile::Warm), 0.f);
    CHECK_EQ(tint::weight(5, 2, 2, Profile::Warm), 0.f);
    CHECK_EQ(tint::weight(0, 0, 0, Profile::Warm), 0.f);
    // Gravity: purple and the warm parts, not its cyan visor
    CHECK(tint::weight(110, 90, 200, Profile::Gravity) > 0.95f);
    CHECK(tint::weight(170, 70, 75, Profile::Gravity) > 0.95f);
    CHECK_EQ(tint::weight(40, 200, 230, Profile::Gravity), 0.f);
    // Phazon: light grey panels, not the dark under-suit, not its red lights
    CHECK(tint::weight(200, 200, 205, Profile::Phazon) > 0.95f);
    CHECK_EQ(tint::weight(40, 40, 40, Profile::Phazon), 0.f);
    CHECK_EQ(tint::weight(230, 40, 20, Profile::Phazon), 0.f);
    // the beam gun's grey-green
    CHECK(tint::weight(130, 165, 155, Profile::Teal) > 0.95f);
    CHECK_EQ(tint::weight(200, 120, 60, Profile::Teal), 0.f);
    // the weight is continuous: a little less saturation gives a little less weight, never a jump to 0
    float prev = 1.f;
    for (int s = 60; s >= 0; s -= 4) {
        const float w = tint::weight(static_cast<uint8_t>(150 + s), 150, 150, Profile::Warm);
        CHECK(w <= prev + 1e-6f);
        prev = w;
    }
}

NXTEST(paint_reference_of_a_flat_image) {
    const auto img = image(64 * 64, 170, 70, 75);
    const tint::PaintRef r = tint::paint_ref(img.data(), 64, 64, Profile::Warm);
    // OKLab of sRGB (170,70,75): L about 0.53, C about 0.13
    CHECK(std::fabs(r.L - 0.53f) < 0.02f);
    CHECK(std::fabs(r.C - 0.13f) < 0.02f);
    // no paint at all: the fallbacks, no division by zero later
    const auto green = image(16 * 16, 90, 220, 60);
    const tint::PaintRef g = tint::paint_ref(green.data(), 16, 16, Profile::Warm);
    CHECK(g.L > 0.f && g.C >= 0.02f);
}

// A texel of the reference paint becomes the chosen colour itself; lights, alpha and unpainted texels stay.
NXTEST(recolour_hits_the_colour_and_keeps_the_rest) {
    auto img = image(4, 170, 70, 75, 200);
    img[4 * 1 + 0] = 90, img[4 * 1 + 1] = 220, img[4 * 1 + 2] = 60;   // a green light
    img[4 * 2 + 0] = 128, img[4 * 2 + 1] = 128, img[4 * 2 + 2] = 128;  // grey metal
    const auto ref_img = image(64, 170, 70, 75);
    const tint::PaintRef ref = tint::paint_ref(ref_img.data(), 8, 8, Profile::Warm);
    const GlowRgb blue{30, 80, 255};
    tint::recolour(img.data(), 4, Profile::Warm, ref, blue);
    CHECK(std::abs(img[0] - 30) <= 4 && std::abs(img[1] - 80) <= 4 && std::abs(img[2] - 255) <= 4);
    CHECK_EQ(img[3], 200);                                            // alpha
    CHECK(img[4] == 90 && img[5] == 220 && img[6] == 60);             // the light
    CHECK(img[8] == 128 && img[9] == 128 && img[10] == 128);          // the metal
    CHECK(std::abs(img[12] - 30) <= 4 && std::abs(img[14] - 255) <= 4);
}

// Black and white keep the detail: texels that differ in the game still differ, in the same order of brightness.
NXTEST(recolour_keeps_shading_from_black_to_white) {
    const uint8_t shades[][3] = {{60, 22, 24}, {110, 45, 48}, {170, 70, 75}, {215, 120, 120}, {240, 190, 170}};
    const int n = 5;
    const auto ref_img = image(64, 170, 70, 75);
    const tint::PaintRef ref = tint::paint_ref(ref_img.data(), 8, 8, Profile::Warm);
    const GlowRgb colours[] = {{0, 0, 0}, {255, 255, 255}, {30, 80, 255}, {200, 30, 30}, {20, 200, 60}};
    for (const GlowRgb& c : colours) {
        std::vector<uint8_t> img(n * 4);
        for (int i = 0; i < n; ++i) {
            std::memcpy(&img[i * 4], shades[i], 3);
            img[i * 4 + 3] = 255;
        }
        tint::recolour(img.data(), n, Profile::Warm, ref, c);
        for (int i = 1; i < n; ++i) CHECK_MSG(luma(&img[i * 4]) > luma(&img[(i - 1) * 4]), "shade order kept");
        // black reads dark but not flat: the reference texel is a dark charcoal, the darkest shade is darker still
        if (c.r == 0 && c.g == 0 && c.b == 0) {
            CHECK(luma(&img[2 * 4]) < 0.05);
            CHECK(luma(&img[2 * 4]) > 0.005);
        }
        // white keeps its shading: the darkest shade is clearly darker than the reference one
        if (c.r == 255) CHECK(luma(&img[0]) < 0.5 * luma(&img[2 * 4]));
    }
}

// A grey chosen colour gives a grey suit (no hue left), the Phazon profile colours its grey panels.
NXTEST(recolour_grey_and_phazon) {
    auto img = image(1, 170, 70, 75);
    const auto ref_img = image(64, 170, 70, 75);
    const tint::PaintRef ref = tint::paint_ref(ref_img.data(), 8, 8, Profile::Warm);
    tint::recolour(img.data(), 1, Profile::Warm, ref, GlowRgb{128, 128, 128});
    CHECK(std::abs(img[0] - img[1]) <= 2 && std::abs(img[1] - img[2]) <= 2);

    auto ph = image(1, 200, 200, 205);
    const auto ph_ref_img = image(64, 200, 200, 205);
    const tint::PaintRef pref = tint::paint_ref(ph_ref_img.data(), 8, 8, Profile::Phazon);
    tint::recolour(ph.data(), 1, Profile::Phazon, pref, GlowRgb{30, 80, 255});
    CHECK(ph[2] > ph[0] + 60);   // blue now
}

NXTEST(texture_table_is_well_formed) {
    size_t n = 0;
    const tint::Target* t = tint::targets(&n);
    CHECK(n >= 15);
    std::set<std::string> ids;
    int suit = 0, gun = 0;
    for (size_t i = 0; i < n; ++i) {
        const std::string u = t[i].uuid;
        CHECK_EQ(u.size(), 36u);
        CHECK(u[8] == '-' && u[13] == '-' && u[18] == '-' && u[23] == '-');
        CHECK(ids.insert(u).second);
        CHECK(std::string(t[i].pak).find(".pak") != std::string::npos);
        (t[i].part == tint::Part::Suit ? suit : gun)++;
    }
    CHECK(suit >= 4 && gun >= 1);
    // the hash tells buffers apart and is stable
    std::vector<uint8_t> a(4099, 7), b = a;
    CHECK_EQ(tint::hash(a.data(), a.size()), tint::hash(b.data(), b.size()));
    b[4098] = 8;
    CHECK(tint::hash(a.data(), a.size()) != tint::hash(b.data(), b.size()));
    b = a;
    b[0] = 9;
    CHECK(tint::hash(a.data(), a.size()) != tint::hash(b.data(), b.size()));
    CHECK(tint::hash(a.data(), 4096) != tint::hash(a.data(), 4097));
}

NXTEST(tweaks_keys_suit_and_gun_color) {
    play::TweakSettings t;
    CHECK(!t.suit_color_on && !t.gun_color_on);   // off by default
    bool known = false;
    std::string msg;
    CHECK(play::apply_tweaks_key("suit_color", " 0,0,0 ", &t, &known, &msg));
    CHECK(known);
    CHECK(t.suit_color_on);
    CHECK(t.suit_color == (GlowRgb{0, 0, 0}));
    CHECK(play::apply_tweaks_key("Gun_Color", "#1E50FF", &t, &known, &msg));
    CHECK(t.gun_color_on && t.gun_color == (GlowRgb{30, 80, 255}));
    // a bad value is refused and leaves the setting as it was
    CHECK(!play::apply_tweaks_key("suit_color", "300,0,0", &t, &known, &msg));
    CHECK(msg.find("suit_color") != std::string::npos);
    CHECK(t.suit_color_on && t.suit_color == (GlowRgb{0, 0, 0}));
    CHECK(!play::apply_tweaks_key("gun_color", "blue", &t, &known, &msg));
    // round trip through the file text
    const std::string text = play::format_tweaks(t);
    CHECK(text.find("suit_color = 0,0,0\n") != std::string::npos);
    CHECK(text.find("gun_color = 30,80,255\n") != std::string::npos);
    play::TweakSettings r;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t nl = text.find('\n', pos);
        const std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        const size_t eq = line.find('=');
        CHECK(play::apply_tweaks_key(line.substr(0, eq), line.substr(eq + 1), &r, &known, &msg));
        CHECK(known);
    }
    CHECK(r.suit_color_on && r.suit_color == t.suit_color && r.gun_color_on && r.gun_color == t.gun_color);
    // off, and the glow keys are not touched by these
    CHECK(play::apply_tweaks_key("suit_color", "off", &r, &known, &msg));
    CHECK(!r.suit_color_on);
    CHECK(!r.glow_on);
    CHECK(play::format_tweaks(r).find("suit_color = off\n") != std::string::npos);
}
