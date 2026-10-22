// SPDX-License-Identifier: GPL-3.0-or-later
// The factory bindings of play mode, as the text of a settings file (play_types.h describes the format). default_settings() in
// play_config.cpp parses this text on top of the built-in tables, so there is one source for the defaults.
//
// How the layers work: a binding in [bindings.<context>] beats a binding for the same input in [bindings.all]. `none` as the action
// switches an input off in that context. The Switch side (what "fire" or "beam_2" press on the pad) is the [switch] table; the defaults
// there are the Remastered "dual stick" layout: ZR fire, ZL lock on, R missile, B jump, Y morph ball, + map, - pause, X held with the
// d-pad picks a beam, the d-pad alone picks a visor.
#include "play_types.h"

namespace nxrt::play {

const char* default_settings_text() {
    return R"INI(# Metroid Prime Remastered, play mode: controls and display.
# F1 in game opens the settings page, which writes this file.

[mouse]
# Radians per mouse count (raw, unaccelerated). The settings page shows the same value on the Source engine scale
# (yaw_source = 2.0 is 2.0 * 0.022 degrees per count).
yaw = 0.000767945
pitch = 0.000767945
invert_y = 0
invert_x = 0
menu_touch = 1
wheel_beams = 1

[pad]
label_abxy = 1
deadzone = 0.10
trigger_threshold = 0.35
gyro = 1
gyro_scale = 1.0
rumble = 1
rumble_scale = 1.0

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

}  // namespace nxrt::play
