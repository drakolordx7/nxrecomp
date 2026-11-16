// SPDX-License-Identifier: GPL-3.0-or-later
// Age of Calamity's options in the base's [display] section of controls.ini (hwaoc_settings.h). Pure data: the launcher links it too.
#include "hwaoc_settings.h"

namespace nxrt::play {

namespace {

const HwaocSettings& cg(const GameState& g) { return g.get<HwaocSettings>(); }
HwaocSettings& mg(GameState& g) { return g.get<HwaocSettings>(); }

const char* const kHudWords[] = {"centered", "stretched"};
const char* const kHudLabels[] = {"16:9 in the middle (the game's proportions)", "Stretched to the full width"};

const GameOption kOptions[] = {
    {.section = "display", .key = "hud", .kind = OptKind::Choice, .words = kHudWords, .word_count = 2,
     .get = [](const GameState& g) { return static_cast<double>(cg(g).hud); },
     .set = [](GameState& g, double v) { mg(g).hud = v != 0 ? HudMode::Stretched : HudMode::Centered; }, .ui = kOptF1 | kOptLauncher,
     .tab = OptTab::Display, .group = "Age of Calamity", .label = "HUD and menus on wide screens",
     .hint = "On a screen wider than 16:9 the 3D view always fills the width. The HUD and the menus stay in a 16:9 area in the middle, or stretch "
             "to the edges. Applies at the next start.",
     .labels = kHudLabels},
    {.section = "display", .key = "dynamic_resolution", .get = [](const GameState& g) { return cg(g).dynamic_resolution ? 1.0 : 0.0; },
     .set = [](GameState& g, double v) { mg(g).dynamic_resolution = v != 0; }, .ui = kOptF1 | kOptLauncher, .tab = OptTab::Display,
     .label = "Dynamic resolution (the game lowers the 3D resolution in busy scenes)"},
};

}  // namespace

const GameSection& hwaoc_settings_section() {
    static const GameSection s = [] {
        GameSection g;
        g.options = kOptions;
        g.option_count = static_cast<unsigned>(sizeof kOptions / sizeof kOptions[0]);
        return g;
    }();
    return s;
}

}  // namespace nxrt::play
