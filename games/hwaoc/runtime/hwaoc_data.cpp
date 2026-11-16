// SPDX-License-Identifier: GPL-3.0-or-later
#include "hwaoc_game.h"
#include "hwaoc_addrs.h"
#include "nxrt/game/identity.h"
#include "nxrt/game/play_types.h"

namespace nxrt::play {
const GameDefaults& hwaoc_game_defaults();     // hwaoc_defaults.cpp
}

namespace nxrt::games::hwaoc {

const game::Info& data_info() {
    static const game::Info info = [] {
        game::Info g;
        game::fill_identity(g);
        g.frame_update = addr::kFrameTask;                  // play mode's frame boundary (M7)
        g.play_defaults = &play::hwaoc_game_defaults();     // factory controls.ini, action names, launcher Keyboard rows
        return g;
    }();
    return info;
}

}  // namespace nxrt::games::hwaoc

void nxrt_register_game_hwaoc_data() { nxrt::game::set_current(&nxrt::games::hwaoc::data_info()); }
