// SPDX-License-Identifier: GPL-3.0-or-later
#include "hwaoc_game.h"
#include "nxrt/game/identity.h"

namespace nxrt::games::hwaoc {

const game::Info& data_info() {
    static const game::Info info = [] {
        game::Info g;
        game::fill_identity(g);
        return g;
    }();
    return info;
}

}  // namespace nxrt::games::hwaoc

void nxrt_register_game_hwaoc_data() { nxrt::game::set_current(&nxrt::games::hwaoc::data_info()); }
