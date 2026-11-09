// SPDX-License-Identifier: GPL-3.0-or-later
// Metroid Prime Remastered, the core part of the game module (mpr_game.h): the data part (mpr_data.cpp) plus the title facts, the frame-step
// sites and the test-tooling probes. Needs the runtime library, not the graphics one (the service and kernel tests register this part).
#include "mpr_game.h"

#include "mpr_sites.h"
#include "mpr_timestep.h"
#include "mpr_tt_probe.h"
#include "nxrt/services.h"
#include "nxrt/timestep.h"
#include "nxrt/tt.h"

namespace nxrt::games::mpr {

namespace {

// main.npdm and control.nacp of the 1.0.0 dump.
void title_defaults(services::TitleProfile& p) {
    using services::Language;
    p.program_id = data_info().program_id;
    p.name = data_info().title;
    p.display_version = data_info().version;
    p.supported_languages = data_info().languages;   // control.nacp SupportedLanguage (mpr_data.cpp)
    p.account_save_size = 4ull << 20;          // UserAccountSaveDataSize 4 MiB
    p.account_save_journal = 1ull << 20;       // UserAccountSaveDataJournalSize 1 MiB
    p.main_thread_stack = 0x100000;            // main.npdm
    p.main_thread_priority = 44;
    p.main_thread_core = 0;
    p.user_nickname = "Samus";
    p.user_uid_hi = 0x0000000000000001ull;
    p.user_uid_lo = 0x4d50525265636f6dull;     // "MPRRecom"
}

// The real-time frame step (nxrt/timestep.h): the places that hold the retail 1/60 as an immediate. CGameArchitectureSupport::Update is Info::frame_update.
using timestep::Load;
using timestep::MidSite;
const MidSite kMids[] = {
    {site::kDtClamp, Load::Step, 8},
    {site::kDtLoop, Load::Step, 27},
    {site::kDtFloor, Load::NegStep, 8},
};

const timestep::Sites kTimestep = [] {
    timestep::Sites s;
    s.retail_step = site::kRetailStep;                    // 0x3C888889
    s.mids = kMids;
    s.mid_count = static_cast<uint32_t>(sizeof kMids / sizeof kMids[0]);
    s.movie_update = site::kMovieUpdate;                  // CMovieList::Update(float)
    s.install = &timestep_install;                        // friction, render timings, streaming, budget, stuck tracker (mpr_timestep.h)
    s.remove = &timestep_remove;
    return s;
}();

}

const game::Info& core_info() {
    static const game::Info info = [] {
        game::Info g = data_info();
        g.title_defaults = &title_defaults;
        g.timestep = &kTimestep;
        g.probes = &tt::mpr::mpr_game_probes();
        return g;
    }();
    return info;
}

}

void nxrt_register_game_mpr_core() { nxrt::game::set_current(&nxrt::games::mpr::core_info()); }
