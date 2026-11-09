// SPDX-License-Identifier: GPL-3.0-or-later
// Metroid Prime Remastered's own section of controls.ini, [tweaks] (the base's nxrt/game/settings.h): the glow and paint colours
// (play_tweaks.h, S44 and S45) and the start-up and menu tweaks (mpr_settings_fastboot.h, S46). The values sit in Settings::game.
// The reader and writer are in the data library (the launcher links them); the F1 tab and rows are in the game library.
#pragma once
#include "nxrt/game/play_types.h"
#include "nxrt/game/settings.h"
#include "mpr_settings_fastboot.h"
#include "play_tweaks.h"

namespace nxrt::play {

struct MprSettings {
    TweakSettings tweaks;          // [tweaks] glow_color suit_glow ball_glow gun_glow suit_color gun_color
    FastbootSettings fastboot;     // [tweaks] skip_intro skip_boot_logos fast_menus menu_speed language
};

inline const MprSettings& mpr_settings(const Settings& s) { return s.game.get<MprSettings>(); }
inline MprSettings& mpr_settings(Settings& s) { return s.game.get<MprSettings>(); }

const GameSection& mpr_settings_section_data();   // mpr_settings.cpp: [tweaks] reader and writer (what the launcher links)
const GameSection& mpr_settings_section();        // mpr_settings_ui.cpp: the same plus the F1 tab and Extras rows

}  // namespace nxrt::play
