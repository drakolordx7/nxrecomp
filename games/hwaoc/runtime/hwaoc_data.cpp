// SPDX-License-Identifier: GPL-3.0-or-later
#include "hwaoc_game.h"
#include "nxrt/game/identity.h"
#include "hwaoc_addrs.h"
#include "hwaoc_settings.h"

namespace nxrt::games::hwaoc {

const game::Info& data_info() {
    static const game::Info info = [] {
        game::Info g;
        game::fill_identity(g);
        g.frame_update = addr::kFrameTask;     // the KIDS frame task: once per update (docs/FRAMESTEP.md)
        g.settings = &settings_section();       // [display] game_step
        return g;
    }();
    return info;
}

}  // namespace nxrt::games::hwaoc

void nxrt_register_game_hwaoc_data() { nxrt::game::set_current(&nxrt::games::hwaoc::data_info()); }
