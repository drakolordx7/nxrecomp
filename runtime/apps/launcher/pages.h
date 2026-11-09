// SPDX-License-Identifier: GPL-3.0-or-later
// The tabs of the launcher. Each factory returns a page the window owns.
#pragma once
#include <memory>

#include "nxrt/game/launcher_ui.h"

namespace nxl {

std::unique_ptr<Page> make_display_page();
std::unique_ptr<Page> make_extras_page();
std::unique_ptr<Page> make_controls_page();
std::unique_ptr<Page> make_keys_page();
std::unique_ptr<Page> make_pad_page();
// The game's own tabs and Extras rows come from the registered game (launcher_ui.h GameLauncher).

}  // namespace nxl
