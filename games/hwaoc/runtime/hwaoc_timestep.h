// SPDX-License-Identifier: GPL-3.0-or-later
// Age of Calamity's real-time frame step (docs/FRAMESTEP.md). The base (nxrt/timestep.h) measures each update's real time at the KIDS frame
// task (Info::frame_update) and paces it; these hooks hand that time to the game's own step mechanisms, which are all whole 60 Hz vsyncs or
// 1/30 s per update on the Switch:
//   * the task's own 1/fps wait is skipped (the base paces);
//   * the frame-rate object's step (game clock) becomes timescale x the real time;
//   * the frame-rate block {fps, interval, fps, float fps}: the integer interval is the whole 60 Hz ticks of this update (an accumulator, so
//     integer timers keep real time on average), the float fps is 1 / step, and every place that reads the interval as a float multiplier
//     gets the fractional ticks (step x 60);
//   * the KTGL device frame rate (event scripts) is 59.94 / ticks;
//   * an event cutscene's frame and frames left advance by the whole 30 fps frames of real time (cutscenes stay on 30 fps frames).
// Keys, section [time] of the game ini (with the base's mode, max_rate, ...): none of its own; mode = off restores the retail 30 fps step.
#pragma once
#include <cstdint>

namespace nxrt::timestep { struct Sites; }

namespace nxrt::games::hwaoc {

const timestep::Sites& timestep_sites();

}  // namespace nxrt::games::hwaoc
