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
        // control.nacp SupportedLanguage, 13 languages: Japanese 0, AmericanEnglish 1, French 2, German 3, Italian 4, Spanish 5, Korean 7,
        // Dutch 8, BritishEnglish 12, CanadianFrench 13, LatinAmericanSpanish 14, SimplifiedChinese 15, TraditionalChinese 16
        g.languages = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 7) | (1u << 8) | (1u << 12) | (1u << 13) |
                      (1u << 14) | (1u << 15) | (1u << 16);
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
