// SPDX-License-Identifier: GPL-3.0-or-later
// The tabs of the launcher. Each factory returns a page the window owns.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "nxrt/game/launcher_ui.h"
#include "nxrt/game/settings.h"

namespace nxl {

std::unique_ptr<Page> make_display_page();
std::unique_ptr<Page> make_extras_page();
std::unique_ptr<Page> make_controls_page();
std::unique_ptr<Page> make_keys_page();
std::unique_ptr<Page> make_pad_page();
// The game's own tabs and Extras rows come from the registered game (launcher_ui.h GameLauncher).

// The rows of the options the game registered for one tab (nxrt/game/settings.h GameOption), for a page that shows them: build() in the page's
// build(), load() in its load().
class GameOptionRows {
public:
    void build(Form& f, nxrt::play::OptTab tab);
    void load(const Settings& s);

private:
    void toggled(unsigned idx);
    void refresh_enabled();
    std::vector<std::wstring> texts_;       // the rows' text, kept as long as the page
    std::vector<int> orig_;                 // a slider's position as loaded: a slider the player did not move writes nothing
    std::vector<HWND> rows_;                // the row's control, for the rows that can be disabled
    std::vector<char> state_;               // the value of each Bool or Choice option as the player has it (non-zero)
};

}  // namespace nxl
