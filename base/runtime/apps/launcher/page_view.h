// SPDX-License-Identifier: GPL-3.0-or-later
// Draws a launcher page (nxrt/game/launcher_ui.h) with Dear ImGui: the rows in order, in the grid the Form gave them.
#pragma once
#include "nxrt/game/launcher_ui.h"

namespace nxui {
class App;
}

namespace nxl {

// Draws `page` into a child window that fills the rest of the current window (less `reserve_bottom` pixels) (scrolls when the rows do not fit). A control the player
// changes runs the row's after() and host.changed(); a button calls page.command(id).
void draw_page(Page& page, nxui::App& app, Host& host, float reserve_bottom = 0.f);

}  // namespace nxl
