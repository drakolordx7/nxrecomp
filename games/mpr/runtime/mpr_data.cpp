// SPDX-License-Identifier: GPL-3.0-or-later
// Metroid Prime Remastered: who the game is and what its files are called (nxrt/game.h Info), and its factory play-mode settings. Pure data, no
// runtime: the launcher links this part alone.
#include "mpr_game.h"
#include "nxrt/game/identity.h"
#include "nxrt/game/play_types.h"
#include "mpr_settings.h"

namespace nxrt::play {
const GameDefaults& mpr_game_defaults();      // mpr_defaults.cpp
}

namespace nxrt::games::mpr {

const game::Info& data_info() {
    static const game::Info info = [] {
        game::Info g;
        game::fill_identity(g);
        g.play_defaults = &play::mpr_game_defaults();
        g.settings = &play::mpr_settings_section_data();
        return g;
    }();
    return info;
}

}

void nxrt_register_game_mpr_data() { nxrt::game::set_current(&nxrt::games::mpr::data_info()); }
