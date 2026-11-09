// SPDX-License-Identifier: GPL-3.0-or-later
// Metroid Prime Remastered: the whole game module as one nxrt::game::Info (mpr_game.h): the core part (mpr_core.cpp) plus the play-mode hooks, the
// button prompts, which need the graphics library.
#include "mpr_game.h"

#include "nxrt/game_render.h"
#include "nxrt/game/play_game.h"
#include "nxrt/game/prompt_resolve.h"
#include "mpr_settings.h"

namespace nxrt::play {
const GamePlay& mpr_game_play();
const PromptGame& mpr_prompt_game();
}

namespace nxrt::games::mpr {

const game::Info& full_info() {
    static const game::Info info = [] {
        game::Info g = core_info();
        g.play = &play::mpr_game_play();
        g.settings = &play::mpr_settings_section();
        g.prompts = &play::mpr_prompt_game();
        return g;
    }();
    return info;
}

}

void nxrt_register_game_mpr() { nxrt::game::set_current(&nxrt::games::mpr::full_info()); }
