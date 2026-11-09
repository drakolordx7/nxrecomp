// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode's half of the game boundary (docs/GAME_BOUNDARY.md, nxrt/game.h Info::play).
//
// Play mode itself is the game-independent PC front end: the window, the SDL event pump, the frame limiter, the binding engine that turns
// keys, mouse buttons and pads into Switch pad input, the menu pointer state, the F1 overlay and its settings file. What only makes sense for
// one game goes through GamePlay: the hooks on that game's functions (raw mouse aim, camera look, display options, menu widgets), what the
// game's state says about the context (nxrt/game/play_types.h: the game's own contexts) and its options and F1 entries (settings.h). A game
// without a GamePlay table still gets a window, pad input, the frame limiter and the overlay; the context is then always the menu.
//
// Order at start (play.cpp start()): install_render, the listener on the frame update (nxrt/timestep.h set_frame_listener; the site is
// nxrt/game.h Info::frame_update), install.
// Order at every frame boundary (main thread, after the step is known): settings_changed (when the page applied new settings), events, pads,
// frame_begin, the engine's tick, frame (context, hints, the pointer's sticks), then the menu pointer (menu_frame).
#pragma once
#include <cstddef>
#include <cstdint>

#include "nxrt/game/menu_mouse.h"
#include "nxrt/game/play_display.h"
#include "nxrt/game/play_types.h"

namespace nxrt::play {

// What play mode offers the game module. Main thread (the thread the game's frame update and input hooks run on) unless noted.
class GameHost {
public:
    virtual ~GameHost() = default;
    virtual const Settings& settings() const = 0;
    virtual bool action_active(Act a, uint64_t now_ns) const = 0;
    virtual uint64_t activations(Act a) const = 0;     // activation edges of the action's bindings so far
    // The right stick as the game gets it (dead zone, invert and sensitivity applied), -1..1, + = right and up.
    virtual void right_stick(float* x, float* y) const = 0;
    // A pad is looking up or down right now (the right stick's vertical axis off centre, or the gyro turning about the pad's pitch axis).
    virtual bool pad_look(bool gyro_enabled) const = 0;
    virtual bool has_input() const = 0;               // play mode's input source is up (pad_look and the game's own look checks apply only then)
    // The game's per-tick player-input function was entered (the game is in play: the context detection and Stats count it). Returns the mouse
    // counts gathered since the last frame boundary on the first call after it (pumping the window's events once more first), zero after that.
    virtual void input_tick(double* dx, double* dy) = 0;
    virtual void aim_applied(double dx, double dy) = 0;
    virtual double pitch_limit_deg() const = 0;                   // [play] pitch_limit_deg
    virtual bool log_status() const = 0;                          // [play] log_status
};

// What play mode knows at one frame boundary.
struct GameFrameIn {
    bool in_game = false;            // the game's per-tick player-input function was called during one of the last few frame boundaries
    double step_seconds = 0;         // the step this frame's game tick will use
    uint64_t now_ns = 0;
    bool pointer_live = false;       // the menu pointer is usable: a focused window, [mouse] menu_cursor on, no overlay
    int win_w = 1, win_h = 1;        // the window's pixel size
};
// The game's answer at one frame boundary.
struct GameFrame {
    float look_scale = 1.f;          // the gyro's pitch rate is multiplied by this before the game reads it
    bool hints_valid = false;        // the cycle fields are meaningful
    int cycle_index = -1;            // the selected member of the cycled set (GameCycle::first = 0), -1 = unknown
    unsigned cycle_mask = 0;         // bit i = member i selectable
    Ctx ctx = Ctx::Menu;             // the context now: the game's own variants of play while in game, the map or a menu outside it (preset: Play / Menu)
    float aux[4] = {};               // left stick x y, right stick x y added to the pad's: the menu pointer driving a screen with sticks (the map)
};

struct GameStatus {
    const char* aim = "";            // why the mouse aim does or does not apply now (settings page)
    double pitch_rad = 0;            // the view pitch the aim holds
};

struct GamePlay {
    // Before the frame listener is set: hooks that do not need the host (display options, the tone curve). window_w/h: the client size.
    void (*install_render)(uint64_t base, const Settings& s, int window_w, int window_h) = nullptr;
    // After it: input, camera, menu widgets.
    void (*install)(uint64_t base, GameHost* host, const Settings& s) = nullptr;
    void (*remove)() = nullptr;
    void (*settings_changed)(const Settings& s) = nullptr;       // new settings are in force
    void (*frame_begin)() = nullptr;                             // after the pads are polled
    void (*frame)(const GameFrameIn& in, const GameHost& host, GameFrame* out) = nullptr;
    DisplayLive (*display_live)() = nullptr;                     // what the display hooks use (the menu pointer's geometry)
    uint32_t (*display_hook_calls)() = nullptr;
    // The menu pointer on the game's own menus (menu_mouse.h, the game side): once per frame, the button the click stands for.
    Act (*menu_frame)(bool menu_up, const mm::Geometry& g) = nullptr;
    bool (*menu_over_value)() = nullptr;                          // the pointer is over a slider's track: the wheel steps its value
    // A wheel notch in context `ctx` that the game takes as a button pulse (the map's zoom). Returns true with *act and the pulse length in ms.
    bool (*menu_wheel)(Ctx ctx, float dy, Act* act, int* ms) = nullptr;
    bool (*menu_active)() = nullptr;                              // the menu hooks saw an active pointer this frame
    void (*status)(GameStatus* out) = nullptr;
    // The game's part of the [play] log_status line, appended after the base fields.
    void (*status_text)(char* buf, size_t n) = nullptr;
};


}  // namespace nxrt::play
