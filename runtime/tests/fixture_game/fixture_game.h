// SPDX-License-Identifier: GPL-3.0-or-later
// The neutral fixture game of the base's tests (fixture_game.cpp).
#pragma once
#include <cstdint>

#include "nxrt/game.h"

void nxrt_register_fixture_game();

namespace nxtest {
constexpr uint64_t kFixtureProgramId = 0x0100000000F1C000ull;   // no title has it
const nxrt::game::Info& fixture_game();
}  // namespace nxtest
