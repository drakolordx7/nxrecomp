// SPDX-License-Identifier: GPL-3.0-or-later
// Button prompts (stream S17): which icon of the prompt pack stands in for each of the game's own button icons. Pure functions, no
// game memory and no graphics; prompts.cpp does the texture side.
//
// The game draws its prompts ("PRESS (A) TO START", the footer of the menus, tutorial texts) with 17 small BC7 textures,
// TXTR_ButtonA, TXTR_ButtonB ... TXTR_DPadIdle, in GuiSysMP1.pak and PreloadFrontEndMPT.pak. Each of them is one Icon here.
#pragma once
#include <string>

#include "play_types.h"

namespace nxrt::play {

// The physical button an icon shows.
enum class Btn : uint8_t { A, B, X, Y, L, R, ZL, ZR, Plus, Minus, DPad, kCount };

// The game's icon textures. The "Pressed" ones are the gold highlighted twins the game draws while the button is held.
enum class Icon : uint8_t {
    A, APressed, B, BPressed, X, Y, L, R, RPressed, ZL, ZLPressed, ZR, ZRPressed, Plus, PlusPressed, Minus, DPad,
    kCount
};
constexpr int kIconCount = static_cast<int>(Icon::kCount);

Btn icon_button(Icon i);
bool icon_pressed(Icon i);
const char* icon_asset_name(Icon i);    // "TXTR_ButtonA" (the name in the game's pak, for logs)
const char* icon_asset_uuid(Icon i);    // the asset id in its pak, canonical lower-case text
const char* icon_pak_file(Icon i);      // pak path under the RomFS folder, "Preload/MPR1/GuiSysMP1.pak"
const char* icon_front_uuid(Icon i);    // the id of the front end's own copy of the icon in Preload/MPRT/PreloadFrontEndMPT.pak (title, menus), or nullptr

struct ResolveIn {
    PromptStyle style = PromptStyle::Switch;   // never Auto: the caller resolves Auto first
    bool label_abxy = true;                    // [pad] label_abxy: Xbox A is Switch A (true) or by position (false)
    Ctx ctx = Ctx::Menu;                       // where the player is
    const Settings* settings = nullptr;        // bindings (keyboard style) and the pad remap (pad styles); null: the built-in mapping
};

// The name of the prompt pack entry that replaces the game's icon: "xbox/a", "ps/cross", "kbm/space", with "_pressed" appended for a
// pressed icon. Pad styles show the physical button that presses the Switch button, following settings->pad.remap. Empty: keep the
// game's own icon (Switch style, a keyboard style button no input is bound to, or a pad style where nothing presses the button or the
// pad source is a stick click or the d-pad).
std::string resolve_icon(const ResolveIn& in, Icon icon);

// Keyboard and mouse: the input that presses this Switch button in the given context, following the bindings of the settings.
// Looks at actions whose [switch] output is exactly that button (confirm and back before the raw btn_ actions), in the bindings that
// apply in ctx (a binding of the context beats the `all` layer for the same input), then in the other contexts. False: nothing is bound.
bool input_for_button(const Settings& s, Ctx ctx, Btn b, InputId* out);

// Pack entry name of an input: "kbm/w", "kbm/left ctrl", "kbm/mouse1", "kbm/wheelup".
std::string kbm_pack_name(const InputId& id);

// Which style "auto" shows: keyboard and mouse once the last input came from them, else the kind of the pad used last.
// Pad kinds are input::PadKind values: 0 other, 1 Xbox, 2 PlayStation, 3 Switch. connected_pad_kind is -1 without a pad. Before any
// input: the first pad if one is connected, else the keyboard.
enum class LastDevice : uint8_t { None, KeyboardMouse, Pad };
PromptStyle auto_style(LastDevice last, int last_pad_kind, int connected_pad_kind);

}  // namespace nxrt::play
