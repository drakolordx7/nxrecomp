// SPDX-License-Identifier: GPL-3.0-or-later
// Metroid Prime Remastered: who the game is and what its files are called (nxrt/game.h Info), and its factory play-mode settings. Pure data, no
// runtime: the launcher links this part alone.
#include "mpr_game.h"
#include "play_types.h"

namespace nxrt::play {
const GameDefaults& mpr_game_defaults();      // mpr_defaults.cpp
}  // namespace nxrt::play

namespace nxrt::games::mpr {

const game::Info& data_info() {
    static const game::Info info = [] {
        game::Info g;
        g.id = "mpr";
        g.title = "Metroid Prime Remastered";
        g.product = "Metroid Prime Remastered PC";
        g.short_title = "Metroid Prime";
        g.program_id = 0x010012101468C000ull;
        g.version = "1.0.0";
        g.main_base = 0x7100000000ull;
        g.exe_name = "mpr.exe";
        g.setup_exe_name = "mpr_setup.exe";
        g.launcher_ini = "mpr_launcher.ini";
        g.game_ini = "mpr.ini";
        g.shader_pack = "shader_cache.mprshc";
        g.save_folder = "MPRRecomp";
        g.play_defaults = &play::mpr_game_defaults();
        return g;
    }();
    return info;
}

}  // namespace nxrt::games::mpr

void nxrt_register_game_mpr_data() { nxrt::game::set_current(&nxrt::games::mpr::data_info()); }
