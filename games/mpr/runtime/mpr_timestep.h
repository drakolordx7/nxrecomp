// SPDX-License-Identifier: GPL-3.0-or-later
// Metroid Prime Remastered's own frame-step hooks (nxrt/timestep.h Sites::install): what the base's step does not reach. The step itself is the
// base's (current_dt); these correct the per-frame counters and constants the game keeps in units of its 1/60 s tick.
#pragma once
#include <cstdint>

namespace nxrt::games::mpr {

// Registers the hooks relative to the main module's base; remove() takes them out. Config, section [time] (read at install):
//   fix_scaled_float = 1   the in-air walk friction (a per-tick float constant loaded into a vector register) is scaled by the step (old name fix_friction)
//   fix_frame_counter = 1  the per-frame render-timing counter (texture scroll, UV animation, water) advances by the step (old name fix_render_timings)
//   fix_stream_step = 1    texture streaming ages its entries by the step instead of 1/60 (old name fix_streaming)
//   fix_scaled_reg = 1     the extra-processing budget (a per-tick constant loaded into a general register) scales with the step (old name fix_budget)
//   fix_tick_sampler = 1   the player stuck tracker takes one sample per 1/60 s of game time instead of one per tick (old name fix_stuck_tracker)
void timestep_install(uint64_t base);
void timestep_remove();

// Tests: put the render-timing counter (a u32 and the float seconds after it) at `counter` instead of the game's address.
void timestep_set_render_counter_for_tests(void* counter);

}
