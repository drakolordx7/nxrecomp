// SPDX-License-Identifier: GPL-3.0-or-later
// The Metroid Prime Remastered game module (docs/GAME_BOUNDARY.md): everything the game-independent base needs to run this one title, as one
// nxrt::game::Info. Three libraries (games/mpr/CMakeLists.txt), each a superset of the one before:
//   nxrt_game_mpr_data   identity, branding, factory settings (pure data)            the launcher           nxrt_register_game_mpr_data()
//   nxrt_game_mpr_core   + title facts, frame-step sites, test probes (needs nxrt)   the service tests      nxrt_register_game_mpr_core()
//   nxrt_game_mpr        + play-mode hooks, button prompts (needs nxrt_gfx)          mpr.exe, gfx tests     nxrt_register_game_mpr()
// The game exe (game-build/src/mpr_main.cpp) calls nxrt_register_game_mpr() before nxrt::init().
#pragma once

#include "nxrt/game.h"

void nxrt_register_game_mpr();
void nxrt_register_game_mpr_core();
void nxrt_register_game_mpr_data();

namespace nxrt::games::mpr {
const game::Info& data_info();     // mpr_data.cpp
const game::Info& core_info();     // mpr_core.cpp
const game::Info& full_info();     // mpr_game.cpp
}  // namespace nxrt::games::mpr
