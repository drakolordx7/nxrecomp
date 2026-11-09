// SPDX-License-Identifier: GPL-3.0-or-later
// Metroid Prime Remastered's pages of the launcher (the base's nxrt/game/launcher_ui.h, GameLauncher): the Tweaks tab (glow and paint colours)
// and the language and start-up rows at the end of the Extras tab. Compiled into the launcher exe only.
#pragma once
#include <memory>

#include "nxrt/game/launcher_ui.h"

namespace nxl {

std::unique_ptr<Page> make_tweaks_page();    // mpr_page_tweaks.cpp
void add_fastboot_rows(Form& f);             // mpr_page_fastboot.cpp

}  // namespace nxl
