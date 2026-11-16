// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode, the Age of Calamity side (nxrt/game/play_game.h GamePlay): so far the display options (hwaoc_display.h). Input and camera hooks
// join this table as they are found.
#include "hwaoc_display.h"
#include "nxrt/game/play_game.h"

namespace nxrt::play {

namespace {

void install_render(uint64_t base, const Settings& s, int window_w, int window_h) { display_install(base, s, window_w, window_h); }
void remove() { display_remove(); }
void settings_changed(const Settings& s) { display_apply(s); }

}  // namespace

const GamePlay& hwaoc_game_play() {
    static const GamePlay t = [] {
        GamePlay p;
        p.install_render = &install_render;
        p.remove = &remove;
        p.settings_changed = &settings_changed;
        p.display_live = &display_live;
        p.display_hook_calls = &display_hook_calls;
        return p;
    }();
    return t;
}

}  // namespace nxrt::play
