// SPDX-License-Identifier: GPL-3.0-or-later
// Metroid Prime Remastered's frame-step hooks (mpr_timestep.h). The base computes the step and loads it into the registers that held the
// immediate (nxrt/timestep.h MidSite); the places below hold a value in units of the game's 1/60 s tick that the step does not reach.
#include "mpr_timestep.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "mpr_sites.h"
#include "nxrt/config.h"
#include "nxrt/hooks.h"
#include "nxrt/timestep.h"

namespace nxrt::games::mpr {

namespace {

// The values the mid sites below load, per 1/60 s.
enum class Load : uint8_t {
    ScaledReg,      // w<reg> = value * step / retail step (float bits)
    ScaledFloat,    // s<reg> = value * step / retail step, the rest of v<reg> zeroed
    Stream1,        // w<reg> = step
    Stream2,        // w<reg> = 2 * step
};
struct MidSite {
    uint64_t offset;
    Load load;
    uint8_t reg;
    float value = 0.f;
};
const MidSite kMids[] = {
    {site::kBudget0, Load::ScaledReg, 8, site::kExtraBudget},
    {site::kBudget1, Load::ScaledReg, 8, site::kExtraBudget},
    {site::kBudget2, Load::ScaledReg, 8, site::kExtraBudget},
    {site::kBudget3, Load::ScaledReg, 8, site::kExtraBudget},
    {site::kFriction, Load::ScaledFloat, 1, site::kFrictionPerTick},   // CPlayerMP1::GetDampedClampedVelocityWR: s1 = 3.5 per 1/60 s
    {site::kStreamA0, Load::Stream1, 8},
    {site::kStreamA1, Load::Stream2, 8},
    {site::kStreamB0, Load::Stream1, 9},
    {site::kStreamB1, Load::Stream2, 9},
};

struct S {
    std::vector<uint32_t> hooks;
    bool fix_scaled_float = true, fix_frame_counter = true, fix_stream_step = true, fix_scaled_reg = true, fix_tick_sampler = true;

    uint64_t render_addr = 0;
    bool render_init = false;
    double render_frames = 0;
    double sampler_acc = 0.0;
    std::mutex mu;
};

S& s() {
    static S* inst = new S();               // never destroyed: hooks may still run during process teardown
    return *inst;
}

uint32_t f2u(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

bool fix_flag(const char* key, const char* alias) { return config_get_bool("time", key, config_get_bool("time", alias, true)); }

// Mid hooks run on any guest thread (streaming is on the render thread): they read the published step only.
int hook_mid(::Ctx* c, uint64_t, void* user) {
    S& st = s();
    if (!timestep::active()) return 0;
    const MidSite& site = *static_cast<const MidSite*>(user);
    const int reg = site.reg;
    const float dt = timestep::current_dt();
    switch (site.load) {
        case Load::ScaledReg:
            if (st.fix_scaled_reg) c->x[reg] = f2u(site.value * timestep::step_scale(dt));
            break;
        case Load::ScaledFloat:
            if (st.fix_scaled_float) {
                c->v[reg].u64[1] = 0;
                c->v[reg].u64[0] = 0;
                c->v[reg].f32[0] = site.value * timestep::step_scale(dt);
            }
            break;
        case Load::Stream1:
            if (st.fix_stream_step) c->x[reg] = f2u(dt);
            break;
        case Load::Stream2:
            if (st.fix_stream_step) c->x[reg] = f2u(2.0f * dt);
            break;
    }
    return 0;
}

// ---- NRenderUtilMPT::TickRenderTimings: retail counts one frame per call and stores counter / 60 seconds ---------------------------
// The counter is a u32 at kRenderTimingCounter, the float seconds follow at +4.
int hook_render_timings(::Ctx* c, uint64_t, void*) {
    S& st = s();
    if (!timestep::active() || !st.fix_frame_counter || !st.render_addr) return 0;
    const float dt = timestep::current_dt();
    uint32_t* counter = reinterpret_cast<uint32_t*>(st.render_addr);
    float* secs = reinterpret_cast<float*>(st.render_addr + 4);
    if (!st.render_init) {
        st.render_frames = *counter;
        st.render_init = true;
    }
    st.render_frames += static_cast<double>(timestep::step_scale(dt));
    while (st.render_frames >= site::kRenderCounterWrap) st.render_frames -= site::kRenderCounterWrap;
    const uint32_t whole = static_cast<uint32_t>(st.render_frames);
    const float seconds = static_cast<float>(st.render_frames) / 60.0f;
    *counter = whole;
    *secs = seconds;
    c->v[0].u64[0] = 0;
    c->v[0].u64[1] = 0;
    c->v[0].f32[0] = seconds;
    return 1;
}

// ---- CPlayerStuckTrackerMP1::AddState: one sample per 1/60 s of game time ----------------------------------------------------------
// The tracker keeps the last 20 samples (positions, velocities, states, one added per player tick) and IsPlayerStuck judges the path length and the
// bounding box of those 20. Written for 60 ticks per second the window is 1/3 s; at 240 ticks per second it is 1/12 s, the top of a jump (speed
// about 1.4 units/s) covers less than the 1/30 unit threshold, the test says "stuck" and ProcessInput calls CPhysicsActorMP1::Stop on the player:
// vertical speed 0 for one frame, a jump 4 % longer. Taking a sample only when 1/60 s of game time has passed keeps the window as it was made.
int hook_tick_sampler(::Ctx*, uint64_t, void*) {
    S& st = s();
    if (!timestep::active() || !st.fix_tick_sampler) return 0;
    const double tick = static_cast<double>(timestep::retail_step());
    const double dt = timestep::current_dt();
    if (dt >= tick * 0.999) {
        st.sampler_acc = 0.0;
        return 0;
    }
    st.sampler_acc += dt;
    if (st.sampler_acc < tick - 1e-5) return 1;
    st.sampler_acc -= tick;
    if (st.sampler_acc > tick) st.sampler_acc = 0.0;
    return 0;
}

}

void timestep_install(uint64_t base) {
    S& st = s();
    st.fix_scaled_float = fix_flag("fix_scaled_float", "fix_friction");
    st.fix_frame_counter = fix_flag("fix_frame_counter", "fix_render_timings");
    st.fix_stream_step = fix_flag("fix_stream_step", "fix_streaming");
    st.fix_scaled_reg = fix_flag("fix_scaled_reg", "fix_budget");
    st.fix_tick_sampler = fix_flag("fix_tick_sampler", "fix_stuck_tracker");
    st.render_init = false;
    st.render_frames = 0;
    st.sampler_acc = 0.0;
    st.render_addr = base + site::kRenderTimingCounter;
    for (const MidSite& m : kMids) st.hooks.push_back(hook_add_mid(base + m.offset, &hook_mid, const_cast<MidSite*>(&m)));
    st.hooks.push_back(hook_add_entry(base + site::kTickRenderTimings, &hook_render_timings, nullptr));
    st.hooks.push_back(hook_add_entry(base + site::kStuckAddState, &hook_tick_sampler, nullptr));
}

void timestep_remove() {
    S& st = s();
    for (uint32_t id : st.hooks) hook_remove(id);
    st.hooks.clear();
}

void timestep_set_render_counter_for_tests(void* counter) { s().render_addr = reinterpret_cast<uint64_t>(counter); }

}
