// SPDX-License-Identifier: GPL-3.0-or-later
// Metroid Prime Remastered: who the game is and what its files are called (nxrt/game.h Info), and its factory play-mode settings. Pure data, no
// runtime: the launcher links this part alone.
#include "mpr_game.h"
#include "nx_game.h"     // generated from games/mpr/game.json (NXGAME_*)
#include "play_types.h"

namespace nxrt::play {
const GameDefaults& mpr_game_defaults();      // mpr_defaults.cpp
}  // namespace nxrt::play

namespace nxrt::games::mpr {

const game::Info& data_info() {
    static const game::Info info = [] {
        game::Info g;
        g.id = NXGAME_ID;
        g.title = NXGAME_TITLE;
        g.product = NXGAME_PRODUCT;
        g.short_title = NXGAME_SHORT_TITLE;
        g.program_id = NXGAME_PROGRAM_ID;
        g.version = NXGAME_VERSION;
        // control.nacp SupportedLanguage, 13 languages: Japanese 0, AmericanEnglish 1, French 2, German 3, Italian 4, Spanish 5, Korean 7,
        // Dutch 8, BritishEnglish 12, CanadianFrench 13, LatinAmericanSpanish 14, SimplifiedChinese 15, TraditionalChinese 16
        g.languages = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 7) | (1u << 8) | (1u << 12) | (1u << 13) |
                      (1u << 14) | (1u << 15) | (1u << 16);
        g.exe_name = NXGAME_EXE;
        g.setup_exe_name = NXGAME_SETUP;
        g.launcher_ini = NXGAME_LAUNCHER_INI;
        g.game_ini = NXGAME_GAME_INI;
        g.shader_pack = NXGAME_SHADER_PACK;
        g.save_folder = NXGAME_SAVE_FOLDER;
        g.play_defaults = &play::mpr_game_defaults();
        return g;
    }();
    return info;
}

}  // namespace nxrt::games::mpr

void nxrt_register_game_mpr_data() { nxrt::game::set_current(&nxrt::games::mpr::data_info()); }
