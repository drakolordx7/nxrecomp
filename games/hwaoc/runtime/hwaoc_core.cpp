// SPDX-License-Identifier: GPL-3.0-or-later
#include "hwaoc_game.h"

#include "nxrt/game/identity.h"
#include "nxrt/services.h"

namespace nxrt::games::hwaoc {

namespace {

// main.npdm (main thread priority 44, core 0, stack 1 MiB: the TitleProfile defaults) and control.nacp of the 1.4.1 update.
void title_defaults(services::TitleProfile& p) {
    game::fill_title_identity(p);
    // One account save per user, no device, cache or temporary storage: UserAccountSaveDataSize 2 MiB, journal 2 MiB.
    p.account_save_size = 0x200000;
    p.account_save_journal = 0x200000;
    p.device_save_size = 0;
    p.device_save_journal = 0;
    p.cache_save_size = 0;
    p.cache_save_journal = 0;
    p.firmware = "9.2.0";                      // the 1.4.1 update is built with SDK 9.6.0
    p.user_nickname = "Player";
}

}  // namespace

const game::Info& core_info() {
    static const game::Info info = [] {
        game::Info g = data_info();
        g.title_defaults = &title_defaults;
        return g;
    }();
    return info;
}

}  // namespace nxrt::games::hwaoc

void nxrt_register_game_hwaoc_core() { nxrt::game::set_current(&nxrt::games::hwaoc::core_info()); }
