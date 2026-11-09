// SPDX-License-Identifier: GPL-3.0-or-later
// Metroid Prime Remastered's play-mode vocabulary (play_types.h GameVocabulary): the settings-file names and factory Switch outputs of every
// action, the context names, the beam set the wheel cycles, and the launcher's Keyboard rows. These are the names controls.ini has always used.
// Pure data: the launcher links it too.
#include "mpr_acts.h"
#include "nxrt/game/play_types.h"

namespace nxrt::play {

namespace {

// One row per Act, in Act order: the base's own actions (renamed) then the game's (mpr_acts.h).
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
    {Act::Secondary, "missile", "R"},
    {Act::Jump, "jump", "B"},
    {Act::Map, "map", "PLUS"},
    {Act::Pause, "pause", "MINUS"},
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
    {mact::Target, "lock_on", "ZL"},
    {mact::LookHold, "free_look", "RSTICK"},
    {mact::AltForm, "morph", "Y"},
    {mact::Beam1, "beam_1", "X+UP", true},          // power
    {mact::Beam2, "beam_2", "X+RIGHT", true},       // wave
    {mact::Beam3, "beam_3", "X+DOWN", true},        // ice
    {mact::Beam4, "beam_4", "X+LEFT", true},        // plasma
    {mact::Visor1, "visor_1", "UP", true},          // combat
    {mact::Visor2, "visor_2", "RIGHT", true},       // scan
    {mact::Visor3, "visor_3", "DOWN", true},        // thermal
    {mact::Visor4, "visor_4", "LEFT", true},        // x-ray
    {mact::BeamNext, "beam_next", "", true},
    {mact::BeamPrev, "beam_prev", "", true},
};
constexpr unsigned kActRows = sizeof kActs / sizeof kActs[0];
static_assert(kActRows == static_cast<unsigned>(Act::kBase) + mact::kGameActs, "one row per Act");

// Play = gameplay, Alt = the morph ball, Inspect = the scan visor.
const char* const kCtxNames[] = {"all", "gameplay", "map", "menu", "morph", "scan"};
constexpr unsigned kCtxRows = sizeof kCtxNames / sizeof kCtxNames[0];
static_assert(kCtxRows == static_cast<unsigned>(Ctx::kBase) + mctx::kGameCtxs, "one name per Ctx");

const GameCycle kBeams = {mact::Beam1, 4, mact::BeamNext, mact::BeamPrev};

const GameActLabel kLabels[] = {
    {Act::MoveForward, "Move forward", "Movement"},
    {Act::MoveBack, "Move back", "Movement"},
    {Act::MoveLeft, "Move left", "Movement"},
    {Act::MoveRight, "Move right", "Movement"},

    {Act::Primary, "Fire", "Combat"},
    {Act::Secondary, "Missile", "Combat"},
    {mact::Target, "Lock on", "Combat"},
    {mact::LookHold, "Free look", "Combat"},
    {Act::Jump, "Jump", "Combat"},
    {mact::AltForm, "Morph ball", "Combat"},

    {mact::Beam1, "Power beam", "Beams"},
    {mact::Beam2, "Wave beam", "Beams"},
    {mact::Beam3, "Ice beam", "Beams"},
    {mact::Beam4, "Plasma beam", "Beams"},
    {mact::BeamNext, "Next beam", "Beams"},
    {mact::BeamPrev, "Previous beam", "Beams"},

    {mact::Visor1, "Combat visor", "Visors"},
    {mact::Visor2, "Scan visor", "Visors"},
    {mact::Visor3, "Thermal visor", "Visors"},
    {mact::Visor4, "X-Ray visor", "Visors"},

    {Act::Map, "Map", "Map and menus"},
    {Act::Pause, "Pause", "Map and menus"},
    {Act::DpadUp, "D-pad up", "Map and menus"},
    {Act::DpadDown, "D-pad down", "Map and menus"},
    {Act::DpadLeft, "D-pad left", "Map and menus"},
    {Act::DpadRight, "D-pad right", "Map and menus"},
};

}

const GameVocabulary& mpr_vocabulary() {
    static const GameVocabulary v = [] {
        GameVocabulary g;
        g.acts = kActs;
        g.act_rows = kActRows;
        g.ctx_names = kCtxNames;
        g.ctx_rows = kCtxRows;
        g.play_ctx_mask = (1u << static_cast<unsigned>(Ctx::Play)) | (1u << static_cast<unsigned>(mctx::Alt)) | (1u << static_cast<unsigned>(mctx::Inspect));
        g.cycle = &kBeams;
        g.labels = kLabels;
        g.label_count = static_cast<unsigned>(sizeof kLabels / sizeof kLabels[0]);
        g.key_wheel_cycles = "wheel_beams";
        g.label_wheel_cycles = "Mouse wheel changes the beam";
        g.keys_note = "The menu, map, gameplay, morph ball and scan keys are not touched.";
        return g;
    }();
    return v;
}

}
