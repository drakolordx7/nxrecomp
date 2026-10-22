// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode (stream S8): the data model shared by the settings file, the binding engine and the settings page.
//
// Vocabulary:
//   input    a physical thing on the keyboard or mouse: a key (SDL scancode), a mouse button or a wheel notch.
//   action   what the player wants the game to do ("fire", "jump", "beam_2"). The game only knows the Switch pad, so every action is
//            tied to a Switch output (buttons and stick directions) by the [switch] table of the settings file.
//   context  where the player is: gameplay, morph ball, map, menu, scan. `all` is a layer under every context.
//   trigger  how an input turns into an action: press (while held), tap (a short pulse after a quick release), hold (after a delay,
//            while held), toggle (each press flips it), doubletap (a second quick press, while held).
// A binding says "in this context, this input drives this action with this trigger".
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nxrt::play {

// ---- contexts ----------------------------------------------------------------------------------------------------------------
enum class Ctx : uint8_t { All = 0, Gameplay, Morph, Map, Menu, Scan, kCount };
constexpr int kCtxCount = static_cast<int>(Ctx::kCount);
const char* ctx_name(Ctx c);                           // "all" "gameplay" "morph" "map" "menu" "scan"
bool parse_ctx(const std::string& s, Ctx* out);        // case-insensitive

// ---- triggers ----------------------------------------------------------------------------------------------------------------
enum class Trig : uint8_t { Press = 0, Tap, Hold, Toggle, DoubleTap };
const char* trig_name(Trig t);                         // "press" "tap" "hold" "toggle" "doubletap"
bool parse_trig(const std::string& s, Trig* out);      // case-insensitive; also accepts "double_tap"

// ---- actions -----------------------------------------------------------------------------------------------------------------
enum class Act : uint8_t {
    None = 0,
    // left stick
    MoveForward, MoveBack, MoveLeft, MoveRight,
    // right stick (keyboard look; the mouse does not use these)
    LookUp, LookDown, LookLeft, LookRight,
    // game actions
    Fire, LockOn, Missile, Jump, Morph, FreeLook, Map, Pause, Confirm, Back,
    DpadUp, DpadDown, DpadLeft, DpadRight,
    Beam1, Beam2, Beam3, Beam4,            // power, wave, ice, plasma: select the beam
    Visor1, Visor2, Visor3, Visor4,        // combat, scan, thermal, x-ray: select the visor
    BeamNext, BeamPrev,                    // cycle the beam (the mouse wheel)
    // raw Switch buttons, for anything the table above does not cover
    BtnA, BtnB, BtnX, BtnY, BtnL, BtnR, BtnZL, BtnZR, BtnPlus, BtnMinus, BtnLStick, BtnRStick,
    kCount
};
constexpr int kActCount = static_cast<int>(Act::kCount);

struct ActInfo {
    Act act;
    const char* name;           // the name used in the settings file: "move_forward" "fire" "beam_1" "btn_zl" ...
    const char* default_out;    // the default [switch] entry in output syntax (see parse_output): "ZR", "X+UP", "LS_UP" ... "" = none
};
const ActInfo& act_info(Act a);
bool parse_act(const std::string& name, Act* out);     // case-insensitive, '-' and '_' equal

// ---- Switch outputs ----------------------------------------------------------------------------------------------------------
// What an action does on the Switch pad: buttons pressed together plus stick deflection (each axis -1..1, summed over all active
// actions and clamped). BeamNext / BeamPrev have no fixed output: the engine turns them into one of Beam1..4 (see engine).
struct Output {
    uint64_t buttons = 0;       // nn::hid::NpadButton bits (nxrt/input_source.h: btn(kBtnA) ...)
    float lx = 0, ly = 0;       // left stick, y up = +
    float rx = 0, ry = 0;       // right stick, y up = +
    bool empty() const { return buttons == 0 && lx == 0 && ly == 0 && rx == 0 && ry == 0; }
    bool operator==(const Output& o) const { return buttons == o.buttons && lx == o.lx && ly == o.ly && rx == o.rx && ry == o.ry; }
};
// Syntax: terms joined by '+', case-insensitive: A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT (d-pad) LSTICK RSTICK (clicks)
// and stick directions LS_UP LS_DOWN LS_LEFT LS_RIGHT RS_UP RS_DOWN RS_LEFT RS_RIGHT. Example "X+UP" = X and d-pad up together.
// "" or "none" parses to the empty output. Unknown terms: returns false and *err names the term.
bool parse_output(const std::string& text, Output* out, std::string* err = nullptr);
std::string output_text(const Output& o);              // canonical text, parse_output(output_text(o)) == o

// ---- inputs ------------------------------------------------------------------------------------------------------------------
enum class InKind : uint8_t { None = 0, Key, Mouse, Wheel };
struct InputId {
    InKind kind = InKind::None;
    int32_t code = 0;           // Key: SDL_Scancode. Mouse: 1 left, 2 middle, 3 right, 4 x1, 5 x2 (the numbering of input.h). Wheel: +1 up, -1 down.
    bool operator==(const InputId& o) const { return kind == o.kind && code == o.code; }
    bool operator<(const InputId& o) const { return kind != o.kind ? kind < o.kind : code < o.code; }
};
// Names: SDL's scancode names for keys ("W", "Space", "Left Ctrl", "Tab", "1", "Keypad 5", "F"), "Mouse1".."Mouse5", "WheelUp", "WheelDown".
std::string input_name(const InputId& id);
bool parse_input(const std::string& name, InputId* out);   // case-insensitive; false for an unknown name

struct Binding {
    InputId in;
    Ctx ctx = Ctx::Gameplay;    // All = every context that has no binding of its own for the same input
    Act act = Act::None;
    Trig trig = Trig::Press;
    int ms = 0;                 // Tap/Hold/DoubleTap window in ms; 0 = the default of [timing]
};

// ---- settings ----------------------------------------------------------------------------------------------------------------
constexpr double kPi = 3.14159265358979323846;
// Source engine yaw scale: one mouse count at sensitivity 1.0 turns 0.022 degrees. The settings file stores radians per count; the
// settings page shows (and edits) the Source-scale value, source = rad_per_count / kSourceRadPerCount.
constexpr double kSourceRadPerCount = 0.022 * kPi / 180.0;
inline double source_sens(double rad_per_count) { return rad_per_count / kSourceRadPerCount; }
inline double rad_per_count(double source) { return source * kSourceRadPerCount; }

struct MouseSettings {
    double yaw_rad = 0.000767945;   // radians per count, horizontal
    double pitch_rad = 0.000767945;   // radians per count, vertical (separate)
    bool invert_y = false;                         // false: mouse up looks up
    bool invert_x = false;
    bool menu_touch = true;                        // in menus the left click is a touch on the Switch touch screen
    bool wheel_beams = true;                       // the wheel cycles beams (the BeamNext / BeamPrev bindings stay either way)
};
struct PadSettings {
    bool label_abxy = true;        // true: buttons by the label printed on them (Xbox A = Switch A); false: by position (bottom = Switch B)
    double stick_deadzone = 0.10;  // radial, 0..0.5, applied before the game's own dead zone
    double trigger_threshold = 0.35;   // analog trigger travel (0..1) that counts as ZL / ZR pressed
    bool gyro = true;              // pass the gyro to the six-axis sensor
    double gyro_scale = 1.0;
    bool rumble = true;
    double rumble_scale = 1.0;
};
struct TimingSettings {
    int tap_ms = 200;              // a press shorter than this is a tap
    int hold_ms = 250;             // a hold starts after this
    int double_ms = 300;           // the second press of a double tap must come within this of the first release
    int pulse_ms = 80;             // how long a tap / beam select / visor select / wheel notch stays pressed on the Switch side
};
struct DisplaySettings {
    std::string window_mode = "windowed";   // "windowed" "borderless" "exclusive"
    bool vsync = false;            // false: no forced vsync (immediate / mailbox, VRR friendly) with the frame cap below
    int fps_cap = 60;              // 0 = unlimited; with vsync on, the cap only applies when it is below the refresh rate
    int width = 1280, height = 720;    // windowed client size, remembered
    bool keep_aspect = true;       // letterbox instead of stretching
    // ---- S10 display options (play_display.h). The first four only apply at the next start: the game builds its render targets once.
    int render_scale = 100;        // percent of the window's client size used as the internal resolution (25..400); ignored with a fixed size
    int render_width = 0, render_height = 0;   // fixed internal resolution (both > 0 and both within 320..16384), 0 = follow the window
    double aspect = 0;             // 0 = the ratio of the internal resolution; otherwise the forced ratio of the 3D view and the HUD (1.0 .. 5.0)
    // live:
    double fov_offset = 0;         // degrees added to the vertical field of view of every game camera (-30 .. 40), 0 = the game's own
    bool dynamic_resolution = false;   // the game's own dynamic resolution (it lowers the render size when the GPU is late)
    int anisotropy = 0;            // 0 = what the game's samplers ask for; 2, 4, 8 or 16 = at least that on every filtering sampler
    double lod_bias = 0;           // texture LOD bias added to every sampler (-2.0 .. 2.0; negative is sharper)
    bool bloom = true;
    bool chromatic_aberration = true;
};

// ---- button prompts ----------------------------------------------------------------------------------------------------------------
// Which button icons the game's prompts show: the game's own Switch icons, or those of another pad or the keyboard. Auto follows the
// last input device used.
enum class PromptStyle : uint8_t { Switch = 0, Xbox, PlayStation, Keyboard, Auto };
constexpr int kPromptStyleCount = 5;
const char* prompt_style_name(PromptStyle s);          // "switch" "xbox" "playstation" "keyboard" "auto"
bool parse_prompt_style(const std::string& s, PromptStyle* out);   // case-insensitive; also accepts ps, ps4, ps5, dualsense, kbm, keyboard_mouse, mouse
struct PromptSettings {
    PromptStyle style = PromptStyle::Switch;   // the game's own icons by default
};

struct Settings {
    MouseSettings mouse;
    PadSettings pad;
    TimingSettings timing;
    DisplaySettings display;
    PromptSettings prompts;
    Output out[kActCount];         // the [switch] table; defaulted from ActInfo::default_out
    std::vector<Binding> bindings;
};

// The factory settings: the table above and the default bindings (default_settings_text() in play_defaults.cpp, which is the same text
// the settings file would contain).
Settings default_settings();
const char* default_settings_text();

// ---- the settings file ---------------------------------------------------------------------------------------------------------
// Plain ini, '#' or ';' starts a comment. Sections and keys are case-insensitive, whitespace around names and values is ignored.
//   [mouse]    yaw = 0.000768   pitch = 0.000768   invert_y = 0   invert_x = 0   menu_touch = 1   wheel_beams = 1     (radians per count)
//              yaw_source / pitch_source = 2.0 are accepted as well (Source scale); when both forms appear the last one wins
//   [pad]      label_abxy deadzone trigger_threshold gyro gyro_scale rumble rumble_scale
//   [timing]   tap_ms hold_ms double_ms pulse_ms
//   [display]  window_mode vsync fps_cap width height keep_aspect render_scale render_width render_height aspect fov_offset
//              dynamic_resolution anisotropy lod_bias bloom chromatic_aberration
//   [prompts]  style = switch|xbox|playstation|keyboard|auto   (which button icons the game's prompts show; auto follows the last used input device)
//   [switch]   <action> = <output>      e.g.  fire = ZR    beam_1 = X+UP    (missing actions keep the default)
//   [bindings.<ctx>]   <input> = <action> [trigger [ms]]     e.g.   Mouse1 = fire    Left Ctrl = morph hold 250    Space = jump toggle
//                      ctx is one of all gameplay morph map menu scan. The same input may appear several times in a section.
// Unknown sections/keys are ignored with a warning in *warnings (not an error); a malformed value of a known key is an error.
// parse_settings starts from default_settings() and applies the text on top. If the text has any [bindings.*] section, the default
// bindings are dropped first (the file is then the whole truth); a file without bindings sections keeps the default bindings.
bool parse_settings(const std::string& text, Settings* out, std::string* err, std::vector<std::string>* warnings = nullptr);
std::string format_settings(const Settings& s);        // canonical text, parse_settings(format_settings(s)) reproduces s
bool load_settings(const std::string& path, Settings* out, std::string* err);   // false + defaults in *out when the file is missing (err empty) or invalid (err set)
bool save_settings(const std::string& path, const Settings& s, std::string* err);   // writes atomically (temp file + rename)

}  // namespace nxrt::play
