// SPDX-License-Identifier: GPL-3.0-or-later
// Age of Calamity's settings of controls.ini (hwaoc_settings.cpp): [display] game_step. In the data library (the launcher links it).
#pragma once

namespace nxrt::play { struct GameSection; }

namespace nxrt::games::hwaoc {

const play::GameSection& settings_section();
// [display] game_step = retail: the frame-step hooks leave the game's own 30 fps step and wait alone.
bool retail_step_chosen();

}  // namespace nxrt::games::hwaoc
