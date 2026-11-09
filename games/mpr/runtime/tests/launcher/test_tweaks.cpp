// SPDX-License-Identifier: GPL-3.0-or-later
// Moved from the base's runtime/tests (launcher/test_tweaks.cpp) in the untangle: these cases check Metroid Prime Remastered's tables (title defaults,
// play-mode vocabulary and factory settings, test-tooling probes), so they run with the mpr module registered.
// [tweaks] in controls.ini (src/gfx/play/play_tweaks.h, S44): the glow colour keys parse, round-trip through the settings file and resolve per part.
#include "test_framework.h"

#include <string>
#include <vector>

#include "play/play_types.h"

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

}  // namespace

NXTEST(tweaks_factory_file_has_the_section_off) {
    Settings s;
    std::string err;
    std::vector<std::string> w;
    REQUIRE(parse_settings(nxrt::play::default_settings_text(), &s, &err, &w));
    CHECK(!s.tweaks.glow_on);
    CHECK(s.tweaks.suit.mode == GlowMode::Auto && s.tweaks.ball.mode == GlowMode::Auto && s.tweaks.gun.mode == GlowMode::Auto);
    CHECK(std::string(nxrt::play::default_settings_text()).find("[tweaks]") != std::string::npos);
}

