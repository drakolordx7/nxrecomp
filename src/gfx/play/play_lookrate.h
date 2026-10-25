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

// Adds dt to *acc (seconds not yet given to the pitch code) and returns how many calls to make now. A call is due once the clock holds one step, give or take
// 3 percent of a step, so a step length of 1/60 s that float arithmetic makes a hair short still gives a call every time. At most max_calls are made; what a long
// hitch leaves over beyond that is dropped.
int look_steps(double* acc, double dt, int max_calls = 8);

// Weight of one step of the gyro's rate: dt * 60, kept to a range that stays sane when a frame takes very long or very short.
double look_rate_scale(double dt);

}  // namespace nxrt::play
