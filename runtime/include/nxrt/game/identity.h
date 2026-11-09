// SPDX-License-Identifier: GPL-3.0-or-later
// The identity half of game::Info, filled from the manifest (games/<id>/game.json) through the generated nx_game.h that the game module's
// include path carries (cmake/nx_game.cmake). A game's data module starts from it and sets only the tables it has:
//     game::Info g;
//     game::fill_identity(g);
// and its title_defaults (Info::title_defaults) starts with fill_title_identity(p) on the services::TitleProfile it gets.
#pragma once
#include "nxrt/game.h"
#include "nx_game.h"

namespace nxrt::game {

inline void fill_identity(Info& g) {
    g.id = NXGAME_ID;
    g.title = NXGAME_TITLE;
    g.product = NXGAME_PRODUCT;
    g.short_title = NXGAME_SHORT_TITLE;
    g.program_id = NXGAME_PROGRAM_ID;
    g.version = NXGAME_VERSION;
    g.languages = NXGAME_LANGUAGES;
    g.exe_name = NXGAME_EXE;
    g.setup_exe_name = NXGAME_SETUP;
    g.launcher_ini = NXGAME_LAUNCHER_INI;
    g.game_ini = NXGAME_GAME_INI;
    g.shader_pack = NXGAME_SHADER_PACK;
    g.save_folder = NXGAME_SAVE_FOLDER;
}

template <class Profile>
void fill_title_identity(Profile& p) {
    p.program_id = NXGAME_PROGRAM_ID;
    p.name = NXGAME_TITLE;
    p.display_version = NXGAME_VERSION;
    p.supported_languages = NXGAME_LANGUAGES;
}

}
