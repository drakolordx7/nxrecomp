// SPDX-License-Identifier: GPL-3.0-or-later
// Metroid Prime Remastered's factory settings for play mode (play_types.h GameDefaults): the text of a settings file (play_types.h describes
// the format) and the bindings later revisions added. default_settings() in play_config.cpp parses this text on top of the built-in tables,
// so there is one source for the defaults. Pure data: the launcher links it too.
//
// How the layers work: a binding in [bindings.<context>] beats a binding for the same input in [bindings.all]. `none` as the action
// switches an input off in that context. The Switch side (what "fire" or "beam_2" press on the pad) is the [switch] table; the defaults
// there are the Remastered "dual stick" layout: ZR fire, ZL lock on, R missile, B jump, Y morph ball, + map, - pause, X held with the
// d-pad picks a beam, the d-pad alone picks a visor.
#include "play_types.h"

namespace nxrt::play {

namespace {

const char* settings_text() {
    return R"INI(# Metroid Prime Remastered, play mode: controls and display.
# F1 in game opens the settings page, which writes this file.

[meta]
# The factory-defaults revision this file was written with. A file with bindings and an older revision gets the newer factory bindings
# added where the key is still unbound (docs/CONFIG.md). Do not change it by hand.
defaults_rev = 2

[mouse]
# Radians per mouse count (raw, unaccelerated). The settings page shows the same value on the Source engine scale
# (yaw_source = 2.0 is 2.0 * 0.022 degrees per count).
yaw = 0.000767945
pitch = 0.000767945
# The morph ball camera (the mouse orbits the camera around the ball) has its own sensitivity, same unit.
morph_yaw = 0.000767945
morph_pitch = 0.000767945
invert_y = 0
invert_x = 0
# In menus the mouse is a pointer: visible cursor, hover highlights, left click activates, right click goes back, the wheel scrolls.
menu_cursor = 1
wheel_beams = 1

[aim]
# Lock-on (hold the lock button): classic is the game's own, the view turns to face the target. free keeps the lock, the homing shots and the strafing, but the view stays under your control (mouse and right stick). Scanning always uses classic.
lock_mode = classic
# With free, the view can move once the lock is established, but the locked target stays on screen. This is the part of the screen kept free along every edge, 0 to 0.4 (0.15 = the target stays in the middle 70 percent).
free_lock_margin = 0.15

[camera]
# 1: the arm cannon is fixed to the view, so its idle drift, walking bob and landing dip move the whole view (the game's own).
# 0: the view follows only the mouse and the stick; those movements move the gun on screen and the view stays still.
gun_drives_view = 1

[pad]
label_abxy = 1
deadzone = 0.10
trigger_threshold = 0.35
gyro = 1
gyro_scale = 1.0
rumble = 1
rumble_scale = 1.0
ball_orbit = 2.0

[timing]
tap_ms = 200
hold_ms = 250
double_ms = 300
pulse_ms = 80

[display]
window_mode = windowed
vsync = 0
fps_cap = 60
width = 1280
height = 720
keep_aspect = 1
# Internal render size as a percent of the window size, 25..400; applies at the next start. Ignored while a fixed size is set.
render_scale = 100
# Fixed internal width, 0 = follow the window, otherwise 320..16384; applies at the next start.
render_width = 0
# Fixed internal height, 0 = follow the window, otherwise 320..16384; applies at the next start.
render_height = 0
# Forced aspect ratio of the 3D view and HUD: 0 = auto, otherwise 1.0..5.0 (16:9 is 1.7778); applies at the next start.
aspect = 0
# Degrees added to the vertical field of view of every game camera, -30..40. 0 = the game's own.
fov_offset = 0
# The game's own dynamic resolution: it lowers the render size when the GPU is late.
dynamic_resolution = 0
# Texture anisotropy: 0 = the game's own, otherwise 2, 4, 8 or 16 at least on every filtered texture.
anisotropy = 0
# Texture LOD bias added to every sampler, -2.0..2.0. Negative is sharper.
lod_bias = 0
# Bloom post effect.
bloom = 1
# Chromatic aberration post effect.
chromatic_aberration = 1

[prompts]
# Button icons in the game's prompts: switch (the game's own), xbox, playstation, keyboard, or auto (follows the last device used; keyboard icons
# until a controller is used).
style = auto

[fps]
# Frame rate counter in a corner of the screen. show = 1 starts with it on; key flips it in the game.
show = 0
key = F2
# top_left, top_right, bottom_left or bottom_right.
corner = top_left
# 0 = frame rate only, 1 = frame rate and frame time, 2 = also the 1% low and the worst frame.
detail = 1
# 1 = the counter also appears in screenshots taken by the test tooling.
in_screenshots = 0

[padmap]
# What each controller button presses on the Switch pad: default (the built-in mapping), none, or Switch buttons joined by +, like A or ZL+L.
# Buttons are named by position: south is the bottom face button (A on an Xbox pad), east the right one, west the left one, north the top one.
south = default
east = default
west = default
north = default
left_shoulder = default
right_shoulder = default
left_trigger = default
right_trigger = default
start = default
back = default
left_stick = default
right_stick = default
dpad_up = default
dpad_down = default
dpad_left = default
dpad_right = default

[bindings.all]
W = move_forward
S = move_back
A = move_left
D = move_right
Up = dpad_up
Down = dpad_down
Left = dpad_left
Right = dpad_right
Mouse1 = fire
Mouse2 = missile
Mouse3 = lock_on
F = missile
Q = lock_on
Space = jump
Left Ctrl = morph
C = morph
Left Alt = free_look
Tab = map
M = map
Escape = pause
1 = beam_1
2 = beam_2
3 = beam_3
4 = beam_4
5 = visor_1
6 = visor_2
7 = visor_3
8 = visor_4
WheelUp = beam_prev
WheelDown = beam_next

[bindings.gameplay]
# Everything comes from [bindings.all]. Add lines here to change a key in the first-person view only.

[bindings.morph]
# Everything comes from [bindings.all]. Add lines here to change a key in the morph ball only.

[bindings.scan]
# Everything comes from [bindings.all]. Add lines here to change a key while the scan visor is up.

[bindings.menu]
Return = confirm
Keypad Enter = confirm
Space = confirm
Mouse1 = confirm
Escape = back
Backspace = back
Mouse3 = back
Q = btn_l
E = btn_r
PageUp = btn_l
PageDown = btn_r
WheelUp = dpad_up
WheelDown = dpad_down
Left Ctrl = none
C = none
F = none
Left Alt = none
1 = none
2 = none
3 = none
4 = none
5 = none
6 = none
7 = none
8 = none
Mouse2 = none

[bindings.map]
# X is the map's Key / Legend button (the footer of the map shows it)
X = btn_x
Return = confirm
Keypad Enter = confirm
Space = confirm
Mouse1 = confirm
Escape = back
Backspace = back
Mouse3 = back
Q = btn_l
E = btn_r
PageUp = btn_l
PageDown = btn_r
WheelUp = look_up
WheelDown = look_down
Left Ctrl = none
C = none
F = none
Left Alt = none
1 = none
2 = none
3 = none
4 = none
5 = none
6 = none
7 = none
8 = none
Mouse2 = none
)INI";
}

// Factory bindings that an older controls.ini does not have (append new ones with the next revision and bump kDefaultsRev).
constexpr int kDefaultsRev = 2;
const GameDefaultAddition kAdditions[] = {
    {2, Ctx::Map, "X", Act::BtnX},   // the map's Key / Legend button (its footer shows X)
};

}  // namespace

const GameDefaults& mpr_game_defaults() {
    static const GameDefaults d{settings_text(), kDefaultsRev, kAdditions, static_cast<unsigned>(sizeof kAdditions / sizeof kAdditions[0])};
    return d;
}

}  // namespace nxrt::play
