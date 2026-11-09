// SPDX-License-Identifier: GPL-3.0-or-later
// Metroid Prime Remastered: the whole game module as one nxrt::game::Info (mpr_game.h): the core part (mpr_core.cpp) plus the play-mode hooks, the
// button prompts and the boot hook, which need the graphics library.
#include "mpr_game.h"

#include "play_game.h"
#include "prompt_resolve.h"

namespace nxrt::play {
const GamePlay& mpr_game_play();          // mpr_play.cpp
const PromptGame& mpr_prompt_game();      // mpr_prompts.cpp
}  // namespace nxrt::play

namespace nxrt::games::mpr {
bool boot(std::string& error);            // mpr_boot.cpp
}  // namespace nxrt::games::mpr

namespace nxrt::games::mpr {

const game::Info& full_info() {
    static const game::Info info = [] {
        game::Info g = core_info();
        g.play = &play::mpr_game_play();
        g.prompts = &play::mpr_prompt_game();
        g.boot = &boot;
        return g;
    }();
    return info;
}

}  // namespace nxrt::games::mpr

void nxrt_register_game_mpr() { nxrt::game::set_current(&nxrt::games::mpr::full_info()); }
