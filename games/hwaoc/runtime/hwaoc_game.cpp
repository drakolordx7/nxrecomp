// SPDX-License-Identifier: GPL-3.0-or-later
#include "hwaoc_game.h"

#include "nxrt/game/play_game.h"

namespace nxrt::play {
const GamePlay& hwaoc_game_play();   // hwaoc_play.cpp
}

namespace nxrt::games::hwaoc {

const game::Info& full_info() {
    static const game::Info info = [] {
        game::Info g = core_info();
        g.play = &play::hwaoc_game_play();
        return g;
    }();
    return info;
}

}  // namespace nxrt::games::hwaoc

void nxrt_register_game_hwaoc() { nxrt::game::set_current(&nxrt::games::hwaoc::full_info()); }
