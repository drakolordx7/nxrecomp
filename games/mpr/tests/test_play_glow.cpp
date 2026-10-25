// SPDX-License-Identifier: GPL-3.0-or-later
// S44 glow colour (src/gfx/play/play_glow.cpp): the colour math that recolours the game's linear glow colours.
#include "test_framework.h"

#include <cmath>

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
