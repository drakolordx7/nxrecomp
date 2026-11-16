// SPDX-License-Identifier: GPL-3.0-or-later
// The Hyrule Warriors: Age of Calamity game module (docs/GAME_BOUNDARY.md): what the game-independent base needs to run this title, as one
// nxrt::game::Info. Libraries (runtime/CMakeLists.txt), each a superset of the one before:
//   nxrt_game_hwaoc_data   identity, branding (pure data)   the launcher           nxrt_register_game_hwaoc_data()
//   nxrt_game_hwaoc_core   + title facts (needs nxrt)       the service tests      nxrt_register_game_hwaoc_core()
//   nxrt_game_hwaoc        + the game exe's registration    hwaoc.exe              nxrt_register_game_hwaoc()
// The game exe (the base's generic runtime/apps/game/game_main.cpp, built by nxrt_add_game_exe) calls nxrt_register_game_hwaoc() before
// nxrt::init(). No boot hook, hook sites or play tables yet (M1: the exe links; M2 boots it).
#pragma once

#include "nxrt/game.h"

void nxrt_register_game_hwaoc();
void nxrt_register_game_hwaoc_core();
void nxrt_register_game_hwaoc_data();
void nxrt_register_game_hwaoc_launcher();   // the launcher exe: data + its pages (hwaoc_launcher.cpp)

namespace nxrt::games::hwaoc {
const game::Info& data_info();
const game::Info& core_info();
const game::Info& full_info();
}  // namespace nxrt::games::hwaoc
