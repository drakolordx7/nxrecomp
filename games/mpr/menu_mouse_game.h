// SPDX-License-Identifier: GPL-3.0-or-later
// Mouse pointer in the game's menus (S28), the Metroid Prime Remastered side.
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
// The pure parts (window pixel to UI plane, the pointer state) are the base's (src/gfx/play/menu_mouse.h); the game side is menu_mouse_game.cpp.
#pragma once
#include <cstdint>

#include "menu_mouse.h"

namespace nxrt::play::mm {

// ---- the game side (menu_mouse_game.cpp) -----------------------------------------------------------------------------------------------
struct GameStats { uint64_t hovers = 0, clicks = 0, widget_calls = 0; };
void game_install(uint64_t module_base);            // adds the hooks (idempotent)
void game_remove();
Act game_frame(bool menu_up, const Geometry& g);    // once per rendered frame: takes the pointer's snapshot for the hooks; the result is a button the click stands for (Act::None = nothing)
GameStats game_stats();
bool game_over_value();                             // the pointer is over a slider's track: the wheel steps its value
const Snapshot& game_snapshot();                    // what the hooks see this frame (tests, status log)
uint64_t game_hook_calls();                         // calls into the two hooks since install

}  // namespace nxrt::play::mm
