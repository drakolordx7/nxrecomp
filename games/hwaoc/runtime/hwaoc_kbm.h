// SPDX-License-Identifier: GPL-3.0-or-later
// The keyboard and mouse half of Age of Calamity's play mode table (hwaoc_play.cpp routes to these): the mouse camera, the main-thread window service
// and the battle / menu context.
#pragma once
#include <cstddef>
#include <cstdint>

#include "nxrt/game/play_game.h"

namespace nxrt::play {

void kbm_install(uint64_t base, GameHost* host, const Settings& s);
void kbm_remove();
void kbm_frame(const GameFrameIn& in, const GameHost& host, GameFrame* out);
void kbm_status(GameStatus* out);
void kbm_status_text(char* buf, size_t n);

}  // namespace nxrt::play
