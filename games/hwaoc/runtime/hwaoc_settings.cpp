// SPDX-License-Identifier: GPL-3.0-or-later
// Age of Calamity's settings of controls.ini (nxrt/game/settings.h): one option the game adds to the base's [display] section, shown on the
// launcher's Display page and the F1 Display tab next to the base's frame rate limit and VSync. Pure data: the launcher links it too.
//
//   [display] game_step = real | retail   real (default): the game is stepped by the real time of each frame at any frame rate
//                                         (docs/FRAMESTEP.md); retail: the Switch's own 30 fps step and wait (the frame rate stays at 30)
#include "hwaoc_settings.h"

#include <atomic>

#include "nxrt/game/settings.h"

namespace nxrt::games::hwaoc {

namespace {

using play::GameOption;
using play::GameSection;
using play::GameState;
using play::OptKind;
using play::OptTab;

struct HwaocSettings {
    bool retail_step = false;
};

// The value the game's hooks read (set whenever the settings are loaded or changed in this process).
std::atomic<bool> g_retail_step{false};

const char* const kStepWords[] = {"real", "retail"};
const char* const kStepLabels[] = {"Real time (any frame rate)", "Original (30 fps)"};

const GameOption kOptions[] = {
    {.section = "display", .key = "game_step", .kind = OptKind::Choice, .words = kStepWords, .word_count = 2,
     .get = [](const GameState& g) { return g.get<HwaocSettings>().retail_step ? 1.0 : 0.0; },
     .set =
         [](GameState& g, double v) {
             g.get<HwaocSettings>().retail_step = v != 0;
             g_retail_step.store(v != 0);
         },
     .ui = play::kOptF1 | play::kOptLauncher, .tab = OptTab::Display, .group = "Game speed", .label = "Game step",
     .hint = "Real time: the game, its camera and input advance by the real time of every frame, so it runs at the frame rate limit "
             "above at normal speed. In-engine cutscenes keep their 30 fps frames. Original: the Switch's 30 fps.",
     .labels = kStepLabels},
};

bool apply(GameState&, const std::string&, const std::string&, bool* known, std::string*) {
    *known = false;
    return true;
}
void format(const GameState&, std::string*) {}

}  // namespace

const play::GameSection& settings_section() {
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

bool retail_step_chosen() { return g_retail_step.load(std::memory_order_relaxed); }

}  // namespace nxrt::games::hwaoc
