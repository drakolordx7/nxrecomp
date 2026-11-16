// SPDX-License-Identifier: GPL-3.0-or-later
#include "hwaoc_game.h"

namespace nxrt::games::hwaoc {

const game::Info& full_info() { return core_info(); }   // M1: no boot hook, hooks or play tables yet

}  // namespace nxrt::games::hwaoc

void nxrt_register_game_hwaoc() { nxrt::game::set_current(&nxrt::games::hwaoc::full_info()); }
