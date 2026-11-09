// SPDX-License-Identifier: GPL-3.0-or-later
// Hook sites of Metroid Prime Remastered 1.0.0 that the game module hands to the base as data: the real-time frame step (nxrt/timestep.h Sites,
// docs/S11.md has what each does). Offsets in main; the lifter gets the same addresses from recomp/games/mpr/main.sites.
#pragma once
#include <cstdint>

namespace nxrt::games::mpr::site {

constexpr uint64_t kUpdate = 0x3c81c8;
constexpr uint64_t kDtClamp = 0x3c8238;          // mid: w8 = step (elapsed clamp)
constexpr uint64_t kDtLoop = 0x3c830c;           // mid: w27 = step
constexpr uint64_t kDtFloor = 0x3c8354;          // mid: w8 = -step
constexpr uint64_t kBudget0 = 0x3c8634, kBudget1 = 0x3c8648, kBudget2 = 0x3c86f4, kBudget3 = 0x3c8708;   // mid: w8 = budget
constexpr uint64_t kFriction = 0xcee3ec;         // mid: s1 = 3.5 * scale60(step)
constexpr uint64_t kTickRenderTimings = 0xc32890;   // entry, handled
constexpr uint64_t kStreamA0 = 0xfe268, kStreamA1 = 0xfe274;   // mid: w8 = step, 2 * step
constexpr uint64_t kStreamB0 = 0xff2fc, kStreamB1 = 0xff308;   // mid: w9 = step, 2 * step
constexpr uint64_t kMovieUpdate = 0x3669ec;      // entry: s0 = elapsed
constexpr uint64_t kStuckAddState = 0xd801d8;     // entry, handled: CPlayerStuckTrackerMP1::AddState(this, state, pos, vel, input); x0 = this stays as the result
// S50 (play_jump.h): the jump timer's last frame
constexpr uint64_t kJumpInput = 0xcf0060;        // entry: CPlayerMP1::JumpInput(this, float dt, CFinalInput const&, CStateManager&)
constexpr uint64_t kJumpInputBlend = 0xcf0750;   // mid: the blend frame, s8 = f * jump force, s9 = (1 - f) * gravity, s0 = mass
constexpr uint64_t kFnMaxRiseSpeed = 0xc72354;   // CPlayerMP1::GetMaximumPlayerPositiveVerticalVelocity(CStateManager const&) -> s0
constexpr uint64_t kFnGetVelocityWR = 0xc577c4;  // CPhysicsActorMP1::GetVelocityWR(CStateManager const&) -> CVector3f* (y = up)
constexpr uint64_t kRenderTimingCounter = 0x459cc10;   // u32 frame counter (data); the float seconds follow at +4
constexpr float kFrictionPerTick = 3.5f;         // the in-air walk friction the friction site loads, per 1/60 s
constexpr float kExtraBudget = 0.014f;           // the extra-processing budget the budget sites load, seconds (0x3C656042)
constexpr double kRenderCounterWrap = 53000.0;   // TickRenderTimings keeps frame % 0xD2F0

}  // namespace nxrt::games::mpr::site
