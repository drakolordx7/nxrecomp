// SPDX-License-Identifier: GPL-3.0-or-later
// Age of Calamity's factory play-mode settings (play_types.h GameDefaults, GameVocabulary): the controls.ini text, the action names and their
// Switch outputs, the context names and the launcher's Keyboard rows. Pure data: the launcher links it too.
//
// The Switch layout the outputs follow (the game's own): left stick moves, right stick turns the camera, Y attack, X strong attack, A jump,
// B dodge, ZL lock on, ZR the character's unique action, R held with a face button a Sheikah Slate rune, L the special attack, + the pause menu,
// - the map, the d-pad picks and uses items. The mouse turns the camera through the camera's own code (hwaoc_play.cpp), not through the stick.
#include "hwaoc_acts.h"
#include "nxrt/game/play_types.h"

namespace nxrt::play {

namespace {

const char* settings_text() {
    return R"INI(# Hyrule Warriors: Age of Calamity, play mode: controls and display.
# F1 in game opens the settings page, which writes this file. The launcher's Keyboard page rebinds the keys too.

[meta]
# The factory-defaults revision this file was written with. Do not change it by hand.
defaults_rev = 1

[mouse]
# Radians per mouse count (raw, unaccelerated), added straight to the camera's turn each camera update.
# The settings page shows the same value on the Source engine scale (yaw_source = 2.0 is 2.0 * 0.022 degrees per count).
yaw = 0.000767945
pitch = 0.000767945
invert_y = 0
invert_x = 0
menu_cursor = 1
wheel_cycles = 0

[prompts]
# Button icons in the game's prompts: switch (the game's own), xbox, playstation, keyboard, or auto (follows the last device used).
style = auto

[bindings.all]
W = move_forward
S = move_back
A = move_left
D = move_right
Mouse1 = attack
Mouse2 = strong_attack
Space = jump
Left Shift = dodge
Mouse3 = lock_on
Q = lock_on
E = unique_action
Mouse5 = unique_action
Left Ctrl = sheikah_slate
Mouse4 = sheikah_slate
F = special_attack
C = camera_reset
Up = dpad_up
Down = dpad_down
Left = dpad_left
Right = dpad_right
1 = dpad_up
2 = dpad_right
3 = dpad_down
4 = dpad_left
WheelUp = dpad_left
WheelDown = dpad_right
Escape = pause
Tab = map
M = map

[bindings.battle]
# Everything comes from [bindings.all]. Add lines here to change a key in battle only.

[bindings.map]

[bindings.menu]
Return = confirm
Keypad Enter = confirm
Space = confirm
Mouse1 = confirm
Escape = back
Backspace = back
Mouse2 = back
Q = btn_l
E = btn_r
PageUp = btn_l
PageDown = btn_r
Z = btn_zl
C = btn_zr
R = btn_y
F = btn_x
Tab = btn_minus
WheelUp = dpad_up
WheelDown = dpad_down
Left Shift = none
Left Ctrl = none
Mouse3 = none
Mouse4 = none
Mouse5 = none
1 = none
2 = none
3 = none
4 = none
)INI";
}

constexpr int kDefaultsRev = 1;

// One row per Act, in Act order: the base's actions (renamed) then the game's (hwaoc_acts.h).
const ActInfo kActs[] = {
    {Act::None, "none", ""},
    {Act::MoveForward, "move_forward", "LS_UP"},
    {Act::MoveBack, "move_back", "LS_DOWN"},
    {Act::MoveLeft, "move_left", "LS_LEFT"},
    {Act::MoveRight, "move_right", "LS_RIGHT"},
    {Act::LookUp, "look_up", "RS_UP"},
    {Act::LookDown, "look_down", "RS_DOWN"},
    {Act::LookLeft, "look_left", "RS_LEFT"},
    {Act::LookRight, "look_right", "RS_RIGHT"},
    {Act::Primary, "attack", "Y"},
    {Act::Secondary, "strong_attack", "X"},
    {Act::Jump, "jump", "A"},
    {Act::Map, "map", "MINUS"},
    {Act::Pause, "pause", "PLUS"},
    {Act::Confirm, "confirm", "A"},
    {Act::Back, "back", "B"},
    {Act::DpadUp, "dpad_up", "UP"},
    {Act::DpadDown, "dpad_down", "DOWN"},
    {Act::DpadLeft, "dpad_left", "LEFT"},
    {Act::DpadRight, "dpad_right", "RIGHT"},
    {Act::BtnA, "btn_a", "A"},
    {Act::BtnB, "btn_b", "B"},
    {Act::BtnX, "btn_x", "X"},
    {Act::BtnY, "btn_y", "Y"},
    {Act::BtnL, "btn_l", "L"},
    {Act::BtnR, "btn_r", "R"},
    {Act::BtnZL, "btn_zl", "ZL"},
    {Act::BtnZR, "btn_zr", "ZR"},
    {Act::BtnPlus, "btn_plus", "PLUS"},
    {Act::BtnMinus, "btn_minus", "MINUS"},
    {Act::BtnLStick, "btn_lstick", "LSTICK"},
    {Act::BtnRStick, "btn_rstick", "RSTICK"},
    {hact::Dodge, "dodge", "B"},
    {hact::LockOn, "lock_on", "ZL"},
    {hact::Unique, "unique_action", "ZR"},
    {hact::Slate, "sheikah_slate", "R"},
    {hact::Special, "special_attack", "L"},
    {hact::CamReset, "camera_reset", "RSTICK"},
};
constexpr unsigned kActRows = sizeof kActs / sizeof kActs[0];
static_assert(kActRows == static_cast<unsigned>(Act::kBase) + hact::kGameActs, "one row per Act");

// Play = battle; the base's map context is kept (the game has no map context of its own yet: the full map counts as a menu).
const char* const kCtxNames[] = {"all", "battle", "map", "menu"};
constexpr unsigned kCtxRows = sizeof kCtxNames / sizeof kCtxNames[0];
static_assert(kCtxRows == static_cast<unsigned>(Ctx::kBase), "one name per Ctx");

const GameActLabel kLabels[] = {
    {Act::MoveForward, "Move forward", "Movement"},
    {Act::MoveBack, "Move back", "Movement"},
    {Act::MoveLeft, "Move left", "Movement"},
    {Act::MoveRight, "Move right", "Movement"},
    {Act::Jump, "Jump", "Movement"},
    {hact::Dodge, "Dodge", "Movement"},

    {Act::Primary, "Attack", "Combat"},
    {Act::Secondary, "Strong attack", "Combat"},
    {hact::Special, "Special attack (L)", "Combat"},
    {hact::Unique, "Unique action (ZR)", "Combat"},
    {hact::Slate, "Sheikah Slate runes (hold R)", "Combat"},
    {hact::LockOn, "Lock on (ZL)", "Combat"},
    {hact::CamReset, "Camera reset (right stick click)", "Combat"},

    {Act::LookUp, "Camera up (keys)", "Camera"},
    {Act::LookDown, "Camera down (keys)", "Camera"},
    {Act::LookLeft, "Camera left (keys)", "Camera"},
    {Act::LookRight, "Camera right (keys)", "Camera"},

    {Act::DpadUp, "Item / d-pad up", "Items and menus"},
    {Act::DpadDown, "Item / d-pad down", "Items and menus"},
    {Act::DpadLeft, "Previous item / d-pad left", "Items and menus"},
    {Act::DpadRight, "Next item / d-pad right", "Items and menus"},
    {Act::Pause, "Pause menu", "Items and menus"},
    {Act::Map, "Map", "Items and menus"},
};

const GameVocabulary& vocabulary() {
    static const GameVocabulary v = [] {
        GameVocabulary g;
        g.acts = kActs;
        g.act_rows = kActRows;
        g.ctx_names = kCtxNames;
        g.ctx_rows = kCtxRows;
        g.play_ctx_mask = 1u << static_cast<unsigned>(Ctx::Play);
        g.labels = kLabels;
        g.label_count = static_cast<unsigned>(sizeof kLabels / sizeof kLabels[0]);
        g.keys_note = "The menu keys are not touched.";
        return g;
    }();
    return v;
}

}  // namespace

const GameDefaults& hwaoc_game_defaults() {
    static const GameDefaults d{settings_text(), kDefaultsRev, nullptr, 0, &vocabulary(), nullptr, 0};
    return d;
}

}  // namespace nxrt::play
