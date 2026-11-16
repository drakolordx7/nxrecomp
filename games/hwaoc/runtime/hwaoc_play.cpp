// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode, the Age of Calamity side (nxrt/game/play_game.h GamePlay): the display options (hwaoc_display.h) and the keyboard and mouse half
// (hwaoc_kbm.h: mouse camera, window service, context).
#include "hwaoc_display.h"
#include "hwaoc_kbm.h"
#include "nxrt/game/play_game.h"

namespace nxrt::play {

namespace {

void install_render(uint64_t base, const Settings& s, int window_w, int window_h) { display_install(base, s, window_w, window_h); }
void install(uint64_t base, GameHost* host, const Settings& s) { kbm_install(base, host, s); }
void remove() {
    kbm_remove();
    display_remove();
}
void frame(const GameFrameIn& in, const GameHost& host, GameFrame* out) { kbm_frame(in, host, out); }
void settings_changed(const Settings& s) { display_apply(s); }

}  // namespace

const GamePlay& hwaoc_game_play() {
    static const GamePlay t = [] {
        GamePlay p;
        p.install_render = &install_render;
        p.install = &install;
        p.remove = &remove;
        p.frame = &frame;
        p.status = &kbm_status;
        p.status_text = &kbm_status_text;
        p.settings_changed = &settings_changed;
        p.display_live = &display_live;
        p.display_hook_calls = &display_hook_calls;
        return p;
    }();
    return t;
}

}  // namespace nxrt::play
