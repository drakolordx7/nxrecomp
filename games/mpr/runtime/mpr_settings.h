// SPDX-License-Identifier: GPL-3.0-or-later
// Metroid Prime Remastered's own section of controls.ini, [tweaks] (the base's nxrt/game/settings.h): the glow and paint colours
// (play_tweaks.h, S44 and S45) and the start-up and menu tweaks (mpr_settings_fastboot.h, S46). The values sit in Settings::game.
// The options the game adds to the base's sections ([aim] [camera] and keys of [mouse] [pad] [display]) are a table (GameOption) in the same
// library, so the base reads, writes and shows them. The reader and writer are in the data library (the launcher links them); the F1 tab and
// rows are in the game library.
#pragma once
#include <cstdint>

#include "nxrt/game/play_types.h"
#include "nxrt/game/settings.h"
#include "mpr_settings_fastboot.h"
#include "play_tweaks.h"

namespace nxrt::play {

// [aim] lock_mode: classic is the game's own lock-on (the view turns to the target); free starts the same way (the game centres the target)
// and then lets the mouse and the right stick move the view, as long as the target stays on the screen (free_lock_margin).
enum class LockMode : uint8_t { Classic = 0, Free };
struct AimSettings {
    LockMode lock_mode = LockMode::Classic;
    double free_lock_margin = 0.15;   // [aim] free_lock_margin (0..0.4): with the free lock the view may move, but the locked target stays inside the screen with this
                                      // fraction of the screen size kept free along every edge (0 = right at the edge, 0.15 = the middle 70 percent)
};
struct CameraSettings {
    bool view_sway = true;   // [camera] gun_drives_view (view_sway): 1 is the game's own behaviour (the held item's idle drift, walking bob and landing dip move
                             // the view); 0 keeps the view still
};
// The morph ball's camera: the mouse and the right stick orbit it ([mouse] ball_camera ball_sensitivity morph_yaw morph_pitch, [pad] ball_orbit).
struct BallCameraSettings {
    double yaw_rad = 0.000767945;     // radians per count
    double pitch_rad = 0.000767945;
    bool on = true;
    double orbit = 2.0;               // rad/s at full right-stick deflection, 0 = off (0..6)
};

struct MprSettings {
    TweakSettings tweaks;          // [tweaks] glow_color suit_glow ball_glow gun_glow suit_color gun_color
    FastbootSettings fastboot;     // [tweaks] skip_intro skip_boot_logos fast_menus menu_speed language
    AimSettings aim;
    CameraSettings camera;
    BallCameraSettings ball;
    bool dynamic_resolution = false;   // [display] dynamic_resolution: the game's own (it lowers the render size when the GPU is late)
    bool bloom = true;                 // [display] bloom
    bool chromatic_aberration = true;  // [display] chromatic_aberration
};

inline const MprSettings& mpr_settings(const Settings& s) { return s.game.get<MprSettings>(); }
inline MprSettings& mpr_settings(Settings& s) { return s.game.get<MprSettings>(); }

const GameSection& mpr_settings_section_data();   // mpr_settings.cpp: [tweaks] reader and writer (what the launcher links)
const GameSection& mpr_settings_section();        // mpr_settings_ui.cpp: the same plus the F1 tab and Extras rows

}
