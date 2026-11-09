// SPDX-License-Identifier: GPL-3.0-or-later
// Metroid Prime Remastered's own actions and contexts, on the ids the base leaves to the game (nxrt/game/play_types.h game_act, game_ctx). The
// order is the order of the rows of mpr_vocabulary.cpp.
#pragma once
#include "nxrt/game/play_types.h"

namespace nxrt::play {

namespace mact {
constexpr Act Target = game_act(0);      // lock on
constexpr Act LookHold = game_act(1);    // free look
constexpr Act AltForm = game_act(2);     // morph ball
constexpr Act Beam1 = game_act(3);       // power, wave, ice, plasma
constexpr Act Beam2 = game_act(4);
constexpr Act Beam3 = game_act(5);
constexpr Act Beam4 = game_act(6);
constexpr Act Visor1 = game_act(7);      // combat, scan, thermal, x-ray
constexpr Act Visor2 = game_act(8);
constexpr Act Visor3 = game_act(9);
constexpr Act Visor4 = game_act(10);
constexpr Act BeamNext = game_act(11);
constexpr Act BeamPrev = game_act(12);
constexpr int kGameActs = 13;
}

namespace mctx {
constexpr Ctx Alt = game_ctx(0);         // the morph ball
constexpr Ctx Inspect = game_ctx(1);     // the scan visor
constexpr int kGameCtxs = 2;
}

}
