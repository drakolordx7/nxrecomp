// SPDX-License-Identifier: GPL-3.0-or-later
// The last frame of a jump's rise with a variable game step (S50, docs/S50.md; found in S49).
//
// How the game does it (main 1.0.0, read in IDA):
//   * CPlayerMP1::JumpInput (0xcf0060) returns the vertical force of the frame. While Samus rises (jump state +0x3f0 == 1) it is the jump force.
//     In the frame where the jump timer (+0x428) runs out partway, timer < dt, it blends: f * jump + (1 - f) * gravity, f = timer / dt
//     (0xcf034c..0xcf0754; at 0xcf0750 s8 = f * jump force, s9 = (1 - f) * gravity acceleration, s0 = the mass).
//   * The rising speed is capped at CPlayerMP1::GetMaximumPlayerPositiveVerticalVelocity (11.667, 14.0 with item 15). The trace of S49 fits one
//     rule to 1e-4: each frame the speed entering the step is capped, the player moves with it, and the frame's force is added on top of it
//     (v_next = min(v, vmax) + F / m * dt).
//   * So in the blend frame of a jump that is already at the cap, the jump part is added on top of the capped speed and is taken away again by the
//     next frame's cap, together with the gravity of the (1 - f) part. A standing jump then goes up to about 6.9 * dt higher (+3.3 % with
//     16 ms frames). With steps of exactly 1/60 the 0.25 s timer ends on a step boundary (the float count-down leaves 1.1e-8 s, f = 7e-7: a
//     frame of pure gravity), which is the console's behaviour: apex 3.500.
//
// The fix: in that frame only, the jump part may raise the speed at most to the cap, as a cap that acts all through the step would:
//     v_end = min(v + a_jump * f * dt, vmax) + a_gravity * (1 - f) * dt
// The mid hook at 0xcf0750 lowers s8 (the jump part) to mass * (vmax - min(v, vmax)) / dt when it is larger; the game's own next two instructions
// add the gravity part. Every other frame, and the blend frame of a jump still below the cap, is left exactly as it was.
//
// The cap scales the whole velocity, not only its vertical part (CPhysicsSteppedMoverCollider::MoveObject: v *= vmax / v.y), so a running jump also
// loses horizontal speed while it is held at the cap (S50 trace: 11.733 -> 10.960 = 11.667 / 12.5 at 1/60). The jump part the fix takes away would
// have caused that loss in the next frame; the hook applies it in place: the horizontal velocity is scaled by vmax / (v + jump part / mass * dt),
// what the cap does to the speed the jump part alone would reach. At f = 1 that is the next frame's own factor.
//
//   [physics] jump_fix = 1     default on; 0 runs the game's code unchanged
//
// Hook sites (recomp games/mpr/main.sites): entryhook 0x7100cf0060 (records dt), midhook 0x7100cf0750 (the rule). Without them this does nothing.
#pragma once
#include <cstdint>

namespace nxrt::play {

// The rule in the form the hook uses: the jump part of the blended force (f * the jump force, mass units) as the game computed it, lowered so that it
// raises the vertical speed at most to the cap within the step. v = the vertical speed entering the frame (above the cap counts as the cap),
// vmax = the cap, dt = the step (s), mass > 0. Returns jump_part itself (bit for bit) when it stays below the cap or an input is not usable.
float jump_part_capped(float jump_part, float v, float vmax, float dt, float mass);

// The horizontal scale that goes with a lowered jump part: vmax / (min(v, vmax) + jump_part / mass * dt) when that is above vmax, else 1.
float jump_cap_scale(float jump_part, float v, float vmax, float dt, float mass);

// The whole rule: the force JumpInput should return in the frame where the jump timer (timer < dt) ends inside the step. a_jump and a_gravity are
// accelerations (units/s^2, gravity negative); f = timer / dt as the game computes it.
float jump_end_force(float v, float vmax, float a_jump, float a_gravity, float timer, float dt, float mass);

void jump_install(uint64_t game_base);    // reads [physics] jump_fix; idempotent
void jump_remove();
uint64_t jump_blend_frames();             // blend frames seen (the timer ended inside a step)
uint64_t jump_capped_frames();            // ... of which the jump part was lowered

}  // namespace nxrt::play
