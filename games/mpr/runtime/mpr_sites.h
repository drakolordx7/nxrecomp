// SPDX-License-Identifier: GPL-3.0-or-later
// Hook sites of Metroid Prime Remastered 1.0.0 that the game module hands to the base as data: the real-time frame step (nxrt/timestep.h Sites,
// docs/S11.md has what each does). Offsets in main; the lifter gets the same addresses from recomp/main.sites.
#pragma once
#include <cstdint>
#include "mpr_addrs.h"

namespace nxrt::games::mpr::site {

using namespace nxrt::games::mpr::addr;
// S50 (play_jump.h): the jump timer's last frame
constexpr float kFrictionPerTick = 3.5f;         // the in-air walk friction the friction site loads, per 1/60 s
constexpr float kExtraBudget = 0.014f;           // the extra-processing budget the budget sites load, seconds (0x3C656042)
constexpr double kRenderCounterWrap = 53000.0;   // TickRenderTimings keeps frame % 0xD2F0

}
