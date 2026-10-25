// SPDX-License-Identifier: GPL-3.0-or-later
// Metroid Prime Remastered's words for play mode's generic slots (play_types.h GameVocabulary): the settings-file names and factory Switch
// outputs of every action, the context names, the launcher's Keyboard rows and the keys of the morph ball and beam-cycling settings. These are
// the names controls.ini has always used. Pure data: the launcher links it too.
#include "play_types.h"

namespace nxrt::play {

namespace {

// One row per Act, in enum order.
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
    {Act::Primary, "fire", "ZR"},
    {Act::Target, "lock_on", "ZL"},
    {Act::Secondary, "missile", "R"},
    {Act::Jump, "jump", "B"},
    {Act::AltForm, "morph", "Y"},
    {Act::LookHold, "free_look", "RSTICK"},
    {Act::Map, "map", "PLUS"},
    {Act::Pause, "pause", "MINUS"},
    {Act::Confirm, "confirm", "A"},
    {Act::Back, "back", "B"},
    {Act::DpadUp, "dpad_up", "UP"},
    {Act::DpadDown, "dpad_down", "DOWN"},
    {Act::DpadLeft, "dpad_left", "LEFT"},
    {Act::DpadRight, "dpad_right", "RIGHT"},
    {Act::SetA1, "beam_1", "X+UP"},          // power
    {Act::SetA2, "beam_2", "X+RIGHT"},       // wave
    {Act::SetA3, "beam_3", "X+DOWN"},        // ice
    {Act::SetA4, "beam_4", "X+LEFT"},        // plasma
    {Act::SetB1, "visor_1", "UP"},           // combat
    {Act::SetB2, "visor_2", "RIGHT"},        // scan
    {Act::SetB3, "visor_3", "DOWN"},         // thermal
    {Act::SetB4, "visor_4", "LEFT"},         // x-ray
    {Act::SetANext, "beam_next", ""},
    {Act::SetAPrev, "beam_prev", ""},
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
};
static_assert(sizeof(kActs) / sizeof(kActs[0]) == kActCount, "one row per Act");

// Play = gameplay, Alt = the morph ball, Inspect = the scan visor.
const char* const kCtxNames[] = {"all", "gameplay", "morph", "map", "menu", "scan"};
static_assert(sizeof(kCtxNames) / sizeof(kCtxNames[0]) == kCtxCount, "one name per Ctx");

const GameActLabel kLabels[] = {
    {Act::MoveForward, "Move forward", "Movement"},
    {Act::MoveBack, "Move back", "Movement"},
    {Act::MoveLeft, "Move left", "Movement"},
    {Act::MoveRight, "Move right", "Movement"},

    {Act::Primary, "Fire", "Combat"},
    {Act::Secondary, "Missile", "Combat"},
    {Act::Target, "Lock on", "Combat"},
    {Act::LookHold, "Free look", "Combat"},
    {Act::Jump, "Jump", "Combat"},
    {Act::AltForm, "Morph ball", "Combat"},

    {Act::SetA1, "Power beam", "Beams"},
    {Act::SetA2, "Wave beam", "Beams"},
    {Act::SetA3, "Ice beam", "Beams"},
    {Act::SetA4, "Plasma beam", "Beams"},
    {Act::SetANext, "Next beam", "Beams"},
    {Act::SetAPrev, "Previous beam", "Beams"},

    {Act::SetB1, "Combat visor", "Visors"},
    {Act::SetB2, "Scan visor", "Visors"},
    {Act::SetB3, "Thermal visor", "Visors"},
    {Act::SetB4, "X-Ray visor", "Visors"},

    {Act::Map, "Map", "Map and menus"},
    {Act::Pause, "Pause", "Map and menus"},
    {Act::DpadUp, "D-pad up", "Map and menus"},
    {Act::DpadDown, "D-pad down", "Map and menus"},
    {Act::DpadLeft, "D-pad left", "Map and menus"},
    {Act::DpadRight, "D-pad right", "Map and menus"},
};

}  // namespace

const GameVocabulary& mpr_vocabulary() {
    static const GameVocabulary v = [] {
        GameVocabulary g;
        g.acts = kActs;
        g.ctx_names = kCtxNames;
        g.labels = kLabels;
        g.label_count = static_cast<unsigned>(sizeof kLabels / sizeof kLabels[0]);
        g.key_alt_yaw = "morph_yaw";
        g.key_alt_pitch = "morph_pitch";
        g.key_wheel_cycles = "wheel_beams";
        g.key_alt_orbit = "ball_orbit";
        g.label_alt_camera = "Morph ball camera";
        g.label_wheel_cycles = "Mouse wheel changes the beam";
        return g;
    }();
    return v;
}

}  // namespace nxrt::play
