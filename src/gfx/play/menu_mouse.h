// SPDX-License-Identifier: GPL-3.0-or-later
// Mouse pointer in the game's menus (S28).
//
// The front end (title, main menu, file select, options, extras) is built from script-driven "UI" components (CUIMenuGOC, CUIWidgetGOC, CUIButtonGOC,
// CUIListViewGOC). Each widget keeps a bounding box in UI units (CUIButtonGOC +176) and the Switch version routed its touch screen through them, but
// the PC build of the game never fires that path (the "touch pressed" command is the invalid command). This module feeds the same code from the
// mouse: a hook on CUIWidgetGOC::ProcessInput hit-tests the cursor against the widget's own box and, on hover, queues a selection exactly the way a
// touch does ("deferred selection"), on a click runs the game's own CUIWidgetGOC::ProcessWidgetTouchScreenInput with the cursor position.
//
// Hooks (entry hooks in recomp/games/mpr/main.sites, the "S28" block):
//   0x88cb74  CUIWidgetGOC::ProcessInput(CStateManager&, float)       x0 = widget, x1 = state manager
//   0x35322c  CFinalInput::GetTouchScreenPosition(CTransform4f const*) returns the position in UI units in s0, s1
//
// The pure parts (window pixel to UI plane, the pointer state) are here and covered by unit tests; the game side is in menu_mouse.cpp.
#pragma once
#include <cstdint>
#include <string>

#include "play_types.h"

namespace nxrt::play::mm {

// ---- arithmetic -----------------------------------------------------------------------------------------------------------------
constexpr double kUiAspect = 16.0 / 9.0;      // the aspect the front end's UI camera is authored for; on a wider display it stays centred

struct Geometry {
    int win_w = 1280, win_h = 720;            // client size in pixels
    int render_w = 1280, render_h = 720;      // internal size: the picture the present pass draws
    double display_aspect = 0;                // the aspect the 3D view and UI use (0 = render_w / render_h)
    bool keep_aspect = true;                  // the present pass letterboxes the picture instead of stretching it
};

// The picture rectangle in window pixels.
struct Rect { double x = 0, y = 0, w = 0, h = 0; };
Rect picture_rect(const Geometry& g);

// A window pixel to the UI plane the touch screen maps to: nx, ny in -1..1 across the centred 16:9 region (nx right +, ny up +). `inside` is
// false when the pixel is outside the picture or beside the 16:9 region (the cursor still moves there, it just hits nothing).
void window_to_ui(const Geometry& g, float px, float py, float* nx, float* ny, bool* inside);

// A window pixel to the picture: u, v in 0..1 (v down); `inside` false outside the picture. The GUI frames (pause screen, options, map) are projected over the
// whole picture, so their normalised screen coordinates are 2u - 1 and 1 - 2v.
void window_to_picture(const Geometry& g, float px, float py, float* u, float* v, bool* inside);

// out = M * (x, y, z, 1) for a row-major 3x4 transform (the game's CTransform4f).
void transform_point(const float m[12], float x, float y, float z, float out[3]);

// ---- the pointer ------------------------------------------------------------------------------------------------------------------
// What the hooks see in one rendered frame. `hover`: the pointer moved since the last frame; `click`: the left button went down.
struct Snapshot {
    bool active = false;       // menu context, window focused, feature on
    bool hover = false;
    bool click = false;
    bool held = false;         // the left button is down
    bool click_unclaimed = false;   // a click ended this frame and no widget took it
    float nx = 0, ny = 0;      // pointer on the UI plane, -1..1
    bool inside = false;
    bool shown = false;        // the cursor is up (the mouse was the last device used)
    float gx = 0, gy = 0;      // the pointer in the GUI frames' screen space, -1..1, y up
    bool in_picture = false;
};

class Pointer {
public:
    void set_enabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }
    // Events (main thread).
    void on_motion(float x, float y);
    void on_button(int button, bool down);
    void on_other_input();                          // a key or a pad button: the cursor hides until the mouse moves
    void on_focus_lost();
    bool held() const { return held_; }
    bool mid_held() const { return mid_held_; }
    // One call per rendered frame, after the context is known. `active`: a menu is up and the window has focus.
    Snapshot begin_frame(bool active, const Geometry& g);
    // The hooks tell the pointer what they did with the snapshot.
    void click_used() { click_used_ = true; }
    // The OS cursor: shown while a menu is up, the feature is on and the mouse was the last device used.
    bool cursor_visible(bool menu_up) const { return enabled_ && menu_up && cursor_shown_; }
    bool has_moved() const { return moved_; }        // a motion event came since the last frame (set by on_motion, which runs just before)
    float x() const { return x_; }
    float y() const { return y_; }
private:
    bool enabled_ = true;
    float x_ = 0, y_ = 0;
    bool moved_ = false, pressed_ = false, held_ = false, mid_held_ = false;
    bool cursor_shown_ = true;
    int click_age_ = 0;
    bool click_used_ = false;
    bool have_pos_ = false;
};

// ---- the game side (menu_mouse.cpp) -----------------------------------------------------------------------------------------------
struct GameStats { uint64_t hovers = 0, clicks = 0, widget_calls = 0; };
Pointer& pointer();                                 // the one pointer of the process
void game_install(uint64_t module_base);            // adds the hooks (idempotent)
void game_remove();
Act game_frame(bool menu_up, const Geometry& g);    // once per rendered frame: takes the pointer's snapshot for the hooks; the result is a button the click stands for (Act::None = nothing)
GameStats game_stats();
bool game_over_value();                             // the pointer is over a slider's track: the wheel steps its value
const Snapshot& game_snapshot();                    // what the hooks see this frame (tests, status log)
uint64_t game_hook_calls();                         // calls into the two hooks since install

}  // namespace nxrt::play::mm
