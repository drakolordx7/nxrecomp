// Frame time for the game's step (games/mpr/docs/work/S11.md): a retail game that runs one tick of a hard-coded step (1/60 s) for every call of its frame
// update speeds up when it renders faster. This module measures the real time of each frame and feeds it to the game instead of the constant:
// the entry hook on the frame update (nxrt/game.h Info::frame_update) computes the step, and mid hooks load it into the registers that held the
// immediate. The game lists those places in Sites (nxrt/game.h Info::timestep) and registers the hooks of its own mechanisms (counters,
// samplers, budgets that are not driven by that step) through Sites::install, reading current_dt().
//
// Configuration, section [time] (env NXRT_TIME_<KEY>):
//   mode = real | fixed | off     real (default): the step is the measured frame time, clamped and smoothed.
//                                 fixed: the step is 1/rate and the frame is held until that time has passed (the 60 Hz lock).
//                                 off: the game's own step per call (retail behaviour, nothing is touched).
//   rate = 60                     steps per second in fixed mode
//   off_rate = 0                  off mode only: hold the frame to this rate but keep the retail step (a diagnostic: the game then runs faster)
//   max_rate = 480                real mode: the step is never shorter than 1/max_rate; with pace=1 a frame is held until then
//   min_rate = 30                 real mode: the step is never longer than 1/min_rate (a slower frame makes the game run slow)
//   smooth = 4                    real mode: the step is the mean of the last N frame times (1 = raw); a jump of more than 50 % restarts it
//   pace = 1                      hold the frame until the minimum step has passed (stops an uncapped limiter from running the game faster)
//   movie_real = 1                the movie list's clock is real elapsed time (audio plays in real time), not the step
//   movie_max = 0.5               longest delta handed to the movie player in one call (seconds)
//   log_every = 0                 log one line of frame statistics every N frames (0 = off)
//   csv = <file>                  write one line per frame: frame, host time (s), measured elapsed, step used, sum of steps, movie frames delivered
// The game's own mechanisms have keys of their own in this section, read by the game.
// Deterministic test mode ([test] deterministic=1): the step is [test] clock_step_ticks / 19.2 MHz and nothing waits, so a scripted run at
// 60, 120, 144 or 240 steps per second is the same run with another clock_step_ticks (160000 = 120 Hz).
#pragma once
#include <cstdint>
#include <string>

#include "nxrt/abi.h"

namespace nxrt::timestep {

enum class Mode : uint8_t { Off = 0, Real = 1, Fixed = 2 };

struct Config {
    Mode mode = Mode::Real;
    double rate = 60.0;
    double off_rate = 0.0;           // Mode::Off only: pace the frame to this rate without touching the step (shows what the retail step does at N Hz)
    double max_rate = 480.0;
    double min_rate = 30.0;
    int smooth = 4;
    bool pace = true;
    bool movie_real = true;
    double movie_max = 0.5;          // longest delta handed to the movie player in one call (seconds)
    int log_every = 0;
    std::string csv;
    float retail_step = 0.f;         // the game's own step (Sites::retail_step); load_config() takes it from the registered game
};

Config load_config();
const char* mode_name(Mode m);

// The game's own step (Sites::retail_step) while the module is installed, else the registered game's. 0 when the game lists none.
float retail_step();
float step_scale(float dt);                 // dt / retail_step(): exactly 1.0f when dt is the game's own step

// ---- where the step goes: the game's sites (offsets in main, nxrt/game.h Info::timestep) ------------------------------------------------
// What one mid hook loads into its register just before the instruction that reads the immediate.
enum class Load : uint8_t {
    Step = 1,       // w<reg> = step (float bits)
    NegStep,        // w<reg> = -step
};
struct MidSite {
    uint64_t offset;
    Load load;
    uint8_t reg;
};
struct Sites {
    float retail_step = 0.f;            // the step the game's code loads as an immediate (seconds)
    const MidSite* mids = nullptr;      // mid hooks, registered in this order
    uint32_t mid_count = 0;
    uint64_t movie_update = 0;          // entry: s0 = the movie clock's delta seconds (0 = none)
    // The game's own hooks that depend on the step. Called once the base's hooks are in (only when the step is in use: not in off mode);
    // `base` is the main module's load address. The hooks read current_dt() and active(). remove undoes install.
    void (*install)(uint64_t base) = nullptr;
    void (*remove)() = nullptr;
};

// ---- the frame-time policy (pure, no clock) -----------------------------------------------------------------------------------
class Stepper {
public:
    explicit Stepper(const Config& c = {});
    // elapsed: seconds since the previous frame started (negative = no previous frame). Returns the step for this frame.
    double step(double elapsed);
    double dt() const { return dt_; }
    uint64_t frames() const { return frames_; }
    uint64_t restarts() const { return restarts_; }
    uint64_t clamped() const { return clamped_; }     // frames whose time was outside [1/max_rate, 1/min_rate]
    double min_period() const;                        // shortest frame the mode allows (0 in off mode)
private:
    Config c_;
    double dt_ = 1.0 / 60.0;
    double ring_[64] = {};
    int n_ = 0, head_ = 0;
    uint64_t frames_ = 0, restarts_ = 0, clamped_ = 0;
};

// ---- the game hooks --------------------------------------------------------------------------------------------------------------
// Registers the hooks on the lifted game (a site the module was not lifted with is simply never called). Idempotent.
void install(const Config& c);
void remove();
bool installed();
void init();
void shutdown();

// The step of the frame that is running (what the mid hooks load), seconds. The retail step until the first frame and in off mode.
float current_dt();
// What the game was last given: false in off mode.
bool active();

struct Stats {
    uint64_t frames = 0;
    double last_dt = 0, last_elapsed = 0;
    double sim_seconds = 0;
    double min_elapsed = 0, max_elapsed = 0, mean_elapsed = 0;
    uint64_t waits = 0;
    uint64_t movie_calls = 0;
    double movie_total = 0;
};
Stats stats();
void reset_stats();

// The frame update (Info::frame_update) is hooked once, here: the step is computed first, then the listener runs with the step the game will
// use in this frame (current_dt() while the module is in use, else the retail step). One listener; nullptr removes it.
using FrameListener = void (*)(double step_seconds);
void set_frame_listener(FrameListener fn);

// Tests: replace the clock (seconds, monotonic) and the sleep; nullptr restores the host clock. A test clock also disables the pacing sleep.
using ClockFn = double (*)();
void set_clock_for_tests(ClockFn fn);
// Tests: treat the run as deterministic with this step (seconds); 0 = follow [test] deterministic.
void set_deterministic_for_tests(double step_seconds);
// The script step `@rate <hz>` (test tooling): from the next frame on the game step is exactly 1/hz, nothing waits (hz > 0), or the configured mode again (0).
void set_run_rate(double hz);

}
