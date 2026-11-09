// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: the look code of the game at the rate it was written for (S36).
//
// The game's pitch code (CPlayerMP1::ComputeFreeLook, called once per ProcessInput) has no time step. The right stick adds to the pitch target by a fixed amount
// per call (dual stick) or sets the target and lets a smoothing filter follow (the other schemes); the filter (CIIRFilter, y = 0.1 x + 0.4 y[-2] + 0.5 y[-1]) is
// written per call as well. The console calls it 60 times a second. Run at another step rate the same code pitches that many times faster (or slower), and the
// filter settles in a different time.
//
// The mechanism here: the hook on ComputeFreeLook (play_lockcam.cpp) keeps a clock and lets the game's own call run once per 1/60 s of game time, with the frame
// time it sees set to 1/60 for the call. At 60 steps per second that is every call, as before; at 120 every second call, at 30 two calls per step. The
// accumulation, the filter and the timers inside run exactly as on the console, whatever the step rate.
//
// The gyro does not go through this: its value is an angular rate that the game integrates per call too, but without a filter, so play.cpp hands the game the
// rate times (step * 60) and the integral is right at every step rate while the view still moves on every frame.
#pragma once

namespace nxrt::play {

constexpr double kLookStep = 1.0 / 60.0;     // seconds of game time per call of the pitch code

// A call is due once the clock holds one step, give or take this fraction of a step. The slack makes a steady 60 Hz give exactly one call per frame: a step
// of 1/60 s that float arithmetic makes a hair short, and frame times that jitter by up to 10 percent (Real timestep, smoothing off), would otherwise give no
// call in one frame and two in the next. The time a call was due early is taken off the clock again, so the long-run rate is exact.
constexpr double kLookTolerance = 0.10;

// Adds dt to *acc (seconds not yet given to the pitch code) and returns how many calls to make now (see kLookTolerance). At most max_calls are made; what a long
// hitch leaves over beyond that is dropped.
int look_steps(double* acc, double dt, int max_calls = 8);

// The carried pitch target (play_lockcam.cpp): between two calls of the game's pitch code the target is carried on at the speed of the last call. pprev, prev and
// cur are the target after the call before the one before the last, after the one before, and after the last call; phase (0..1) is how far into the next
// 1/60 s the frame is. The target is only carried when the target is accumulating: the last two call-to-call speeds have the same sign and are within
// kCarryRatioMin..kCarryRatioMax of each other. Anything else (a stick let go, a clamp, a target the game sets outright, a jump from the mouse) holds cur.
// The carried part never goes beyond +-limit (radians; limit <= 0 means no limit) and never pulls cur itself back.
constexpr double kCarryRatioMin = 0.25, kCarryRatioMax = 4.0;
double look_carry_target(double pprev, double prev, double cur, double phase, double limit);

// Weight of one step of the gyro's rate: dt * 60, kept to a range that stays sane when a frame takes very long or very short.
double look_rate_scale(double dt);

}  // namespace nxrt::play
