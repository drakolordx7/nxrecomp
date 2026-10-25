// SPDX-License-Identifier: GPL-3.0-or-later
// Metroid Prime Remastered, the core part of the game module (mpr_game.h): the data part (mpr_data.cpp) plus the title facts, the frame-step
// sites and the test-tooling probes. Needs the runtime library, not the graphics one (the service and kernel tests register this part).
#include "mpr_game.h"

#include "mpr_sites.h"
#include "nxrt/services.h"
#include "nxrt/timestep.h"
#include "nxrt/tt.h"


namespace nxrt::tt::mpr {
const GameProbes& mpr_game_probes();      // mpr_tt_probe.cpp
}  // namespace nxrt::tt::mpr

namespace nxrt::games::mpr {

namespace {

// main.npdm and control.nacp of the 1.0.0 dump.
void title_defaults(services::TitleProfile& p) {
    using services::Language;
    p.program_id = data_info().program_id;
    p.name = data_info().title;
    p.display_version = data_info().version;
    // control.nacp SupportedLanguage: 13 languages.
    uint32_t m = 0;
    for (Language l : {Language::AmericanEnglish, Language::BritishEnglish, Language::Japanese, Language::French, Language::German,
                       Language::LatinAmericanSpanish, Language::Spanish, Language::Italian, Language::Dutch, Language::CanadianFrench,
                       Language::Korean, Language::TraditionalChinese, Language::SimplifiedChinese})
        m |= 1u << static_cast<uint32_t>(l);
    p.supported_languages = m;
    p.account_save_size = 4ull << 20;          // UserAccountSaveDataSize 4 MiB
    p.account_save_journal = 1ull << 20;       // UserAccountSaveDataJournalSize 1 MiB
    p.main_thread_stack = 0x100000;            // main.npdm
    p.main_thread_priority = 44;
    p.main_thread_core = 0;
    p.user_nickname = "Samus";
    p.user_uid_hi = 0x0000000000000001ull;
    p.user_uid_lo = 0x4d50525265636f6dull;     // "MPRRecom"
}

// The real-time frame step (nxrt/timestep.h): CGameArchitectureSupport::Update and the places that hold 1/60, in the order the base registers them.
using timestep::Load;
using timestep::MidSite;
const MidSite kMids[] = {
    {site::kDtClamp, Load::Step, 8},
    {site::kDtLoop, Load::Step, 27},
    {site::kDtFloor, Load::NegStep, 8},
    {site::kBudget0, Load::Budget, 8, site::kExtraBudget},
    {site::kBudget1, Load::Budget, 8, site::kExtraBudget},
    {site::kBudget2, Load::Budget, 8, site::kExtraBudget},
    {site::kBudget3, Load::Budget, 8, site::kExtraBudget},
    {site::kFriction, Load::ScaledFloat, 1, site::kFrictionPerTick},   // CPlayerMP1::GetDampedClampedVelocityWR: s1 = 3.5 per 1/60 s
    {site::kStreamA0, Load::Stream1, 8},
    {site::kStreamA1, Load::Stream2, 8},
    {site::kStreamB0, Load::Stream1, 9},
    {site::kStreamB1, Load::Stream2, 9},
};

const timestep::Sites kTimestep = [] {
    timestep::Sites s;
    s.frame_update = site::kUpdate;
    s.mids = kMids;
    s.mid_count = static_cast<uint32_t>(sizeof kMids / sizeof kMids[0]);
    s.tick_render_timings = site::kTickRenderTimings;     // NRenderUtilMPT::TickRenderTimings
    s.render_timing_counter = site::kRenderTimingCounter;
    s.render_counter_wrap = site::kRenderCounterWrap;
    s.movie_update = site::kMovieUpdate;                  // CMovieList::Update(float)
    s.per_tick_sampler = site::kStuckAddState;            // CPlayerStuckTrackerMP1::AddState
    return s;
}();

}  // namespace

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

}  // namespace nxrt::games::mpr

void nxrt_register_game_mpr_core() { nxrt::game::set_current(&nxrt::games::mpr::core_info()); }
