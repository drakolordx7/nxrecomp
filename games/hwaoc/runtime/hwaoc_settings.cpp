// SPDX-License-Identifier: GPL-3.0-or-later
// Age of Calamity's options in the base's [display] section of controls.ini (hwaoc_settings.h). Pure data: the launcher links it too.
#include "hwaoc_settings.h"

#include <atomic>
#include <string>

namespace nxrt::games::hwaoc {
// The value the frame-step hooks read (set whenever the settings are loaded or changed in this process).
std::atomic<bool> g_retail_step{false};
bool retail_step_chosen() { return g_retail_step.load(std::memory_order_relaxed); }
}  // namespace nxrt::games::hwaoc

namespace nxrt::play {

namespace {

using games::hwaoc::g_retail_step;

const HwaocSettings& cg(const GameState& g) { return g.get<HwaocSettings>(); }
HwaocSettings& mg(GameState& g) { return g.get<HwaocSettings>(); }

const char* const kStepWords[] = {"real", "retail"};
const char* const kStepLabels[] = {"Real time (any frame rate)", "Original (30 fps)"};
const char* const kHudWords[] = {"centered", "stretched"};
const char* const kHudLabels[] = {"16:9 in the middle (the game's proportions)", "Stretched to the full width"};

const GameOption kOptions[] = {
    {.section = "display", .key = "game_step", .kind = OptKind::Choice, .words = kStepWords, .word_count = 2,
     .get = [](const GameState& g) { return cg(g).retail_step ? 1.0 : 0.0; },
     .set =
         [](GameState& g, double v) {
             mg(g).retail_step = v != 0;
             g_retail_step.store(v != 0);
         },
     .ui = kOptF1 | kOptLauncher, .tab = OptTab::Display, .group = "Game speed", .label = "Game step",
     .hint = "Real time: the game, its camera and input advance by the real time of every frame, so it runs at the frame rate limit "
             "above at normal speed. In-engine cutscenes keep their 30 fps frames. Original: the Switch's 30 fps.",
     .labels = kStepLabels},
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

bool apply(GameState&, const std::string&, const std::string&, bool* known, std::string*) {
    *known = false;
    return true;
}
void format(const GameState&, std::string*) {}

}  // namespace

const GameSection& hwaoc_settings_section() {
    static const GameSection s = [] {
        GameSection g;
        g.name = "hwaoc";
        g.apply = &apply;
        g.format = &format;
        g.options = kOptions;
        g.option_count = static_cast<unsigned>(sizeof kOptions / sizeof kOptions[0]);
        return g;
    }();
    return s;
}

}  // namespace nxrt::play
