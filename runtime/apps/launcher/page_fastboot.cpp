// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher's start-up and menu tweaks (S46, src/gfx/play/play_fastboot.h): rows at the end of the Extras tab.
#include <string>

#include "pages.h"

namespace mprl {

void add_fastboot_rows(Form& f) {
    f.heading(L"Start-up and menus");
    f.checkbox(L"Skip the opening cutscene of a new game", [](const Settings& s) { return s.fastboot.skip_intro; },
               [](Settings& s, bool v) { s.fastboot.skip_intro = v; });
    f.checkbox(L"Skip the title intro movie", [](const Settings& s) { return s.fastboot.skip_boot_logos; },
               [](Settings& s, bool v) { s.fastboot.skip_boot_logos = v; });
    f.checkbox(L"Faster menus", [](const Settings& s) { return s.fastboot.fast_menus; }, [](Settings& s, bool v) { s.fastboot.fast_menus = v; });
    f.slider(
        L"Menu speed", 1, 4, [](const Settings& s) { return s.fastboot.menu_speed; }, [](Settings& s, int v) { s.fastboot.menu_speed = v; },
        [](int v) { return std::to_wstring(v) + L"x"; });
    f.hint(L"New Game then starts in the docking hangar, after the game's own cutscene skip. Faster menus speed up the title, main menu and save "
           L"slots (used only with Faster menus on). Everything here is off by default.", 2, true);
}

}  // namespace mprl
