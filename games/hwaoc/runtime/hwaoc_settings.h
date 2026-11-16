// SPDX-License-Identifier: GPL-3.0-or-later
// Age of Calamity's settings of controls.ini (the base's nxrt/game/settings.h): the options the game adds to the base's [display] section, as one
// table (GameOption), so the base reads, writes and shows them (F1 page and the launcher's Display tab). The values sit in Settings::game.
// The reader and writer are in the data library (the launcher links it).
//   [display] game_step = real | retail          the frame-step hooks (docs/FRAMESTEP.md)
//   [display] hud = centered | stretched          the 2D HUD and menus on displays wider than 16:9
//   [display] dynamic_resolution = 0 | 1         the game's own dynamic resolution
#pragma once
#include <cstdint>

#include "nxrt/game/play_types.h"
#include "nxrt/game/settings.h"

namespace nxrt::play {

// [display] hud: where the 2D HUD and menus go on a display wider than 16:9.
enum class HudMode : uint8_t { Centered = 0, Stretched };

struct HwaocSettings {
    bool dynamic_resolution = false;   // [display] dynamic_resolution: the game's own (lowers the 3D scene's size when the frame is late)
    HudMode hud = HudMode::Centered;   // [display] hud
    bool retail_step = false;          // [display] game_step = retail
};

inline const HwaocSettings& hwaoc_settings(const Settings& s) { return s.game.get<HwaocSettings>(); }
inline HwaocSettings& hwaoc_settings(Settings& s) { return s.game.get<HwaocSettings>(); }

const GameSection& hwaoc_settings_section();   // hwaoc_settings.cpp: no section of its own, only the [display] options

}  // namespace nxrt::play

namespace nxrt::games::hwaoc {

inline const play::GameSection& settings_section() { return play::hwaoc_settings_section(); }
// [display] game_step = retail: the frame-step hooks leave the game's own 30 fps step and wait alone.
bool retail_step_chosen();

}  // namespace nxrt::games::hwaoc
