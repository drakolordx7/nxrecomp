// SPDX-License-Identifier: GPL-3.0-or-later
// The F1 overlay's half of the [tweaks] section (mpr_settings.h): the Tweaks tab (play_glow_ui.cpp) and the start-up rows of the Extras tab
// (mpr_ui_fastboot.cpp).
#include "mpr_settings.h"

namespace nxrt::play {

bool glow_tab(Settings& edit);      // play_glow_ui.cpp
bool fastboot_ui(Settings& edit);   // mpr_ui_fastboot.cpp

const GameSection& mpr_settings_section() {
    static const GameSection s = [] {
        GameSection g = mpr_settings_section_data();
        g.tab_title = "Tweaks";
        g.tab = &glow_tab;
        g.extras_rows = &fastboot_ui;
        return g;
    }();
    return s;
}

}  // namespace nxrt::play
