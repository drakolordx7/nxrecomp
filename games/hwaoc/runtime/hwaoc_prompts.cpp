// SPDX-License-Identifier: GPL-3.0-or-later
// Button prompts, the Age of Calamity side (prompt_resolve.h PromptGame): the game draws its Switch button icons from one 1024 x 1024 BC3 atlas (the
// alpha is the shape, the game tints it). The base recognises the atlas in the texture uploads by its size and a hash of its bytes and redraws the
// cells below with the keyboard, mouse or pad icon the prompt style asks for. The cells were measured on the 1.4.1 atlas (the Switch icon sheet: face
// buttons, shoulders, triggers, plus and minus); the d-pad and stick icons stay the game's own.
#include "nxrt/game/prompt_resolve.h"

namespace nxrt::play {

namespace {

// The resolver needs the Switch button of every icon (Btn); the asset names are for the log only. Order of Icon.
#define HW_ICON(btn, pressed, name) {Btn::btn, pressed, name, "", "", nullptr}
const PromptIcon kIcons[kIconCount] = {
    HW_ICON(A, false, "atlas A"),        HW_ICON(A, true, "atlas A pressed"),   HW_ICON(B, false, "atlas B"),       HW_ICON(B, true, "atlas B pressed"),
    HW_ICON(X, false, "atlas X"),        HW_ICON(Y, false, "atlas Y"),          HW_ICON(L, false, "atlas L"),       HW_ICON(R, false, "atlas R"),
    HW_ICON(R, true, "atlas R pressed"), HW_ICON(ZL, false, "atlas ZL"),        HW_ICON(ZL, true, "atlas ZL pressed"), HW_ICON(ZR, false, "atlas ZR"),
    HW_ICON(ZR, true, "atlas ZR pressed"), HW_ICON(Plus, false, "atlas Plus"),  HW_ICON(Plus, true, "atlas Plus pressed"), HW_ICON(Minus, false, "atlas Minus"),
    HW_ICON(DPad, false, "atlas DPad"),  HW_ICON(None, false, "atlas LStick"),  HW_ICON(None, false, "atlas RStick"),
};
#undef HW_ICON

// Cells in texels (multiples of 4), around the glyphs of the atlas (each glyph's alpha bounds rounded out to whole blocks). Every button has two copies in
// the sheet side by side (normal and pressed): both are redrawn with the same icon. The thin plus and minus bars used inside text are left alone.
const PromptAtlasCell kCells[] = {
    {Icon::A, 784, 408, 48, 44},     {Icon::A, 840, 408, 44, 44},     {Icon::B, 892, 408, 44, 44},      {Icon::B, 944, 408, 44, 44},
    {Icon::X, 692, 672, 44, 44},     {Icon::X, 744, 672, 48, 44},     {Icon::Y, 800, 672, 44, 44},      {Icon::Y, 852, 672, 44, 44},
    {Icon::L, 376, 568, 44, 32},     {Icon::L, 428, 568, 44, 32},     {Icon::R, 532, 624, 44, 36},      {Icon::R, 588, 624, 40, 36},
    {Icon::ZL, 588, 568, 40, 40},    {Icon::ZL, 640, 568, 44, 40},    {Icon::ZR, 640, 624, 44, 36},     {Icon::ZR, 696, 624, 40, 36},
    {Icon::Plus, 8, 564, 36, 40},    {Icon::Plus, 168, 620, 36, 36},  {Icon::Plus, 220, 620, 36, 36},
    {Icon::Minus, 220, 672, 36, 36}, {Icon::Minus, 272, 672, 36, 36},
};

const PromptAtlas kAtlas = [] {
    PromptAtlas a;
    a.width = 1024;
    a.height = 1024;
    a.bc = 3;
    a.storage_size = 1024u * 1024u;
    a.hash = 0x56c7b239c5615d76ull;      // prompt_hash() of the atlas storage in 1.4.1
    a.cells = kCells;
    a.cell_count = sizeof kCells / sizeof kCells[0];
    return a;
}();

}  // namespace

const PromptGame& hwaoc_prompt_game() {
    static const PromptGame g = [] {
        PromptGame p;
        p.icons = kIcons;
        p.atlas = &kAtlas;
        return p;
    }();
    return g;
}

}  // namespace nxrt::play
