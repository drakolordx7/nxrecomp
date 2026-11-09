// SPDX-License-Identifier: GPL-3.0-or-later
// Frame pacing: the one owner of the frame cap, of where the frame limiter waits, and of the wait itself. The frame limiter (frame_limiter.h),
// low latency (lowlat.h), play mode and the frame-time module (timestep.h) ask this module and do not read the settings or wait on their own.
//
// Configuration, section [latency] (env NXRT_LATENCY_<KEY>), read once:
//   pace = main | present   Where the frame limiter waits (S18). main (default): at the game's frame boundary, before the input is sampled, so the
//                           picture shown was simulated one render time earlier and not one queue depth earlier. present: inside the present call.
//                           A present that no boundary paced (loading screens, movies) waits in the present call in both modes.
//   vsync_cap = 1           With vsync on and no cap below the refresh rate, hold the frame 0.05 % under the refresh rate (S21): the queue behind the
//                           display stays empty. 0 = no cap, the display alone paces.
//   reflex_limiter = 0      Keep the limiter's wait at the boundary while Reflex's own sleep paces the frame (two pacers accumulate their sleeps).
#pragma once
#include <cstdint>

namespace nxrt::pacing {

uint64_t now_ns();                       // monotonic (performance counter), nanoseconds

struct Config {
    bool in_main = true;
    bool vsync_cap = true;
    bool reflex_limiter = false;
};
const Config& config();
inline bool in_main() { return config().in_main; }

// The cap that applies for these display settings: fps_cap (0 = none), with vsync on the display paces, so a cap at or above the refresh rate (or none)
// becomes just under the refresh rate ([latency] vsync_cap) or none.
double frame_cap(double fps_cap, bool vsync, float refresh_hz);

// The frame cap in force (0 = none). Every pacer follows it through a listener; a listener added later is not called for the value already set,
// it reads cap().
void set_cap(double fps);
double cap();
using CapListener = void (*)(void* user, double fps);
void add_cap_listener(CapListener fn, void* user);
void remove_cap_listener(CapListener fn, void* user);

// The frame-boundary handshake: the boundary hook (main thread) notes that it waited for the frame it starts; the present of that frame (presenting
// thread) consumes the note and does not wait again.
void boundary_done();
bool boundary_consume();

// One wait until a time: a high-resolution waitable timer for the bulk, a spin on the performance counter for the last stretch whose length follows
// how late the timer woke before. One instance per waiting thread.
class Waiter {
public:
    void until_ns(uint64_t target);
private:
    double overshoot_ema_ns_ = 0.0;
};

}  // namespace nxrt::pacing
