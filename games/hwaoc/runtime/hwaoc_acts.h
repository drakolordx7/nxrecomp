// SPDX-License-Identifier: GPL-3.0-or-later
// Age of Calamity's own play-mode actions, on the ids the base leaves to the game (nxrt/game/play_types.h game_act). The order is the order of
// the rows of hwaoc_defaults.cpp. No contexts of its own: battle is the base's Play context, everything else is Menu.
#pragma once
#include "nxrt/game/play_types.h"

namespace nxrt::play::hact {

constexpr Act Dodge = game_act(0);         // B
constexpr Act LockOn = game_act(1);        // ZL
constexpr Act Unique = game_act(2);        // ZR: the character's unique action
constexpr Act Slate = game_act(3);         // R held: the Sheikah Slate runes (with Y X A B)
constexpr Act Special = game_act(4);       // L
constexpr Act CamReset = game_act(5);      // right stick click
constexpr int kGameActs = 6;

}  // namespace nxrt::play::hact
