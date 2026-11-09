// SPDX-License-Identifier: GPL-3.0-or-later
// Moved from the base's runtime/tests (launcher/test_tweaks.cpp) in the untangle: these cases check Metroid Prime Remastered's tables (title defaults,
// play-mode vocabulary and factory settings, test-tooling probes), so they run with the mpr module registered.
// [tweaks] in controls.ini (play_tweaks.h, S44): the glow colour keys parse, round-trip through the settings file and resolve per part.
#include "test_framework.h"

#include <string>
#include <vector>

#include "mpr_settings.h"
#include "nxrt/game/play_types.h"

NXTEST_GROUP(mpr_launcher_tweaks)

namespace {

using nxrt::play::glow_effective;
using nxrt::play::GlowMode;
using nxrt::play::GlowRgb;
using nxrt::play::parse_glow_rgb;
using nxrt::play::parse_settings;
using nxrt::play::format_settings;
using nxrt::play::Settings;
using nxrt::play::TweakSettings;
using nxrt::play::mpr_settings;

}  // namespace

NXTEST(tweaks_factory_file_has_the_section_off) {
    Settings s;
    std::string err;
    std::vector<std::string> w;
    REQUIRE(parse_settings(nxrt::play::default_settings_text(), &s, &err, &w));
    CHECK(!mpr_settings(s).tweaks.glow_on);
    CHECK(mpr_settings(s).tweaks.suit.mode == GlowMode::Auto && mpr_settings(s).tweaks.ball.mode == GlowMode::Auto && mpr_settings(s).tweaks.gun.mode == GlowMode::Auto);
    CHECK(std::string(nxrt::play::default_settings_text()).find("[tweaks]") != std::string::npos);
}

// Moved from the base's tests/launcher/test_tweaks.cpp: the section reader and writer now belong to this module.
NXTEST(tweaks_default_is_off) {
    const TweakSettings t;
    CHECK(!t.glow_on);
    GlowRgb c;
    CHECK(!glow_effective(t, t.ball, &c));
    CHECK(!glow_effective(t, t.gun, &c));
    Settings s;
    std::string err;
    std::vector<std::string> w;
    REQUIRE(parse_settings("", &s, &err, &w));
    CHECK(!mpr_settings(s).tweaks.glow_on);
    CHECK(mpr_settings(s).tweaks.ball.mode == GlowMode::Auto);
}

NXTEST(tweaks_rgb_forms) {
    GlowRgb c;
    REQUIRE(parse_glow_rgb("255,128,0", &c));
    CHECK(c.r == 255 && c.g == 128 && c.b == 0);
    REQUIRE(parse_glow_rgb(" 1 , 2 , 3 ", &c));
    CHECK(c.r == 1 && c.g == 2 && c.b == 3);
    REQUIRE(parse_glow_rgb("#00FFa0", &c));
    CHECK(c.r == 0 && c.g == 255 && c.b == 0xa0);
    CHECK(!parse_glow_rgb("256,0,0", &c));
    CHECK(!parse_glow_rgb("1,2", &c));
    CHECK(!parse_glow_rgb("1,2,3,4", &c));
    CHECK(!parse_glow_rgb("red", &c));
    CHECK(!parse_glow_rgb("#12345", &c));
}

NXTEST(tweaks_parse_and_round_trip) {
    Settings s;
    std::string err;
    std::vector<std::string> w;
    REQUIRE(parse_settings("[tweaks]\nglow_color = 0,170,255\nball_glow = off\ngun_glow = 10,20,30\n", &s, &err, &w));
    CHECK(w.empty());
    CHECK(mpr_settings(s).tweaks.glow_on);
    CHECK(mpr_settings(s).tweaks.glow == (GlowRgb{0, 170, 255}));
    CHECK(mpr_settings(s).tweaks.ball.mode == GlowMode::Off);
    CHECK(mpr_settings(s).tweaks.gun.mode == GlowMode::Color);
    GlowRgb c;
    CHECK(!glow_effective(mpr_settings(s).tweaks, mpr_settings(s).tweaks.ball, &c));
    REQUIRE(glow_effective(mpr_settings(s).tweaks, mpr_settings(s).tweaks.gun, &c));
    CHECK(c == (GlowRgb{10, 20, 30}));

    const std::string text = format_settings(s);
    CHECK(text.find("[tweaks]") != std::string::npos);
    CHECK(text.find("glow_color = 0,170,255") != std::string::npos);
    CHECK(text.find("ball_glow = off") != std::string::npos);
    CHECK(text.find("gun_glow = 10,20,30") != std::string::npos);
    Settings r;
    REQUIRE(parse_settings(text, &r, &err, &w));
    CHECK(mpr_settings(r).tweaks.glow_on && mpr_settings(r).tweaks.glow == mpr_settings(s).tweaks.glow);
    CHECK(mpr_settings(r).tweaks.ball == mpr_settings(s).tweaks.ball);
    CHECK(mpr_settings(r).tweaks.gun == mpr_settings(s).tweaks.gun);
}

NXTEST(tweaks_auto_follows_glow_color) {
    Settings s;
    std::string err;
    REQUIRE(parse_settings("[tweaks]\nglow_color = 16,32,48\n", &s, &err));
    GlowRgb c;
    REQUIRE(glow_effective(mpr_settings(s).tweaks, mpr_settings(s).tweaks.ball, &c));
    CHECK(c == (GlowRgb{0x10, 0x20, 0x30}));
    REQUIRE(parse_settings("[tweaks]\nglow_color = off\n", &s, &err));
    CHECK(!glow_effective(mpr_settings(s).tweaks, mpr_settings(s).tweaks.ball, &c));
}

NXTEST(tweaks_bad_value_is_an_error_and_unknown_key_a_warning) {
    Settings s;
    std::string err;
    std::vector<std::string> w;
    CHECK(!parse_settings("[tweaks]\nglow_color = blue\n", &s, &err, &w));
    CHECK(err.find("glow_color") != std::string::npos);
    w.clear();
    REQUIRE(parse_settings("[tweaks]\nsomething_else = 1\n", &s, &err, &w));
    CHECK(w.size() == 1);
}

NXTEST(tweaks_suit_part_and_reset) {
    Settings s;
    std::string err;
    std::vector<std::string> w;
    REQUIRE(parse_settings("[tweaks]\nglow_color = 255,0,255\nsuit_glow = off\n", &s, &err, &w));
    CHECK(w.empty());
    GlowRgb c;
    CHECK(!glow_effective(mpr_settings(s).tweaks, mpr_settings(s).tweaks.suit, &c));
    REQUIRE(glow_effective(mpr_settings(s).tweaks, mpr_settings(s).tweaks.gun, &c));
    CHECK(c == (GlowRgb{255, 0, 255}));
    CHECK(format_settings(s).find("suit_glow = off") != std::string::npos);
    // Reset (the launcher's and the F1 page's button): the defaults, which write the game's own colours back.
    mpr_settings(s).tweaks = TweakSettings{};
    const std::string text = format_settings(s);
    CHECK(text.find("glow_color = off") != std::string::npos);
    CHECK(text.find("suit_glow = auto") != std::string::npos);
    CHECK(text.find("ball_glow = auto") != std::string::npos);
    CHECK(text.find("gun_glow = auto") != std::string::npos);
    Settings r;
    REQUIRE(parse_settings(text, &r, &err, &w));
    CHECK(!mpr_settings(r).tweaks.glow_on);
    CHECK(!glow_effective(mpr_settings(r).tweaks, mpr_settings(r).tweaks.ball, &c));
}

