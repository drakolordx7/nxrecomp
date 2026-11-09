// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher's registration of Metroid Prime Remastered: the data module plus its launcher pages (mpr_launcher.h).
#include "mpr_game.h"
#include "mpr_launcher.h"

namespace nxl {

namespace {
const PageFactory kTabs[] = {&make_tweaks_page};
}

const GameLauncher& mpr_launcher_pages() {
    static const GameLauncher g = [] {
        GameLauncher l;
        l.tabs = kTabs;
        l.tab_count = static_cast<unsigned>(sizeof kTabs / sizeof kTabs[0]);
        l.extras_rows = &add_fastboot_rows;
        return l;
    }();
    return g;
}

}  // namespace nxl

void nxrt_register_game_mpr_launcher() {
    static const nxrt::game::Info info = [] {
        nxrt::game::Info g = nxrt::games::mpr::data_info();
        g.launcher = &nxl::mpr_launcher_pages();
        return g;
    }();
    nxrt::game::set_current(&info);
}
