// SPDX-License-Identifier: GPL-3.0-or-later
// The last frame of a jump's rise with a variable game step (S50). See play_jump.h.
#include "play_jump.h"

#include <atomic>
#include <cmath>

#include "mpr_sites.h"
#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "play_aim.h"

namespace nxrt::play {
namespace site = games::mpr::site;

namespace {
constexpr const char* kTag = "jump";

struct State {
    uint32_t ids[2] = {0, 0};
    uint64_t base = 0;
    std::atomic<uint64_t> blends{0}, capped{0};
};
State g_s;

// What the entry hook saw: the player and the step of the JumpInput call that is running on this thread.
struct Call { uint64_t player = 0; float dt = 0; };
thread_local Call t_call;

// CPlayerMP1::JumpInput(this, float dt, CFinalInput const&, CStateManager&): x0 = the player, s0 = dt. Only records; the game's function runs.
int entry_hook(::Ctx* c, uint64_t, void*) {
    t_call.player = c->x[0];
    t_call.dt = c->v[0].f[0];
    return 0;
}

// A guest call from inside the hook; the registers are put back afterwards.
::Ctx call(::Ctx* c, uint64_t fn, uint64_t a0, uint64_t a1) {
    const ::Ctx saved = *c;
    c->x[0] = a0;
    c->x[1] = a1;
    dispatch_call(c, g_s.base + fn);
    const ::Ctx out = *c;
    *c = saved;
    return out;
}

// 0xcf0750 (FMUL S0, S9, S0), only reached in the blend frame: s8 = f * jump force, s9 = (1 - f) * gravity, s0 = the mass; x19 = the player,
// x20 = the state manager. The next two instructions make s9 = s8 + s9 * s0, the result.
int blend_hook(::Ctx* c, uint64_t, void*) {
    g_s.blends.fetch_add(1, std::memory_order_relaxed);
    const uint64_t player = c->x[19], mgr = c->x[20];
    if (player == 0 || player != t_call.player) return 0;
    const float dt = t_call.dt, mass = c->v[0].f[0], jump_part = c->v[8].f[0];
    const float vmax = call(c, site::kFnMaxRiseSpeed, player, mgr).v[0].f[0];
    const uint64_t vel = call(c, site::kFnGetVelocityWR, player, mgr).x[0];
    float v = 0;
    if (!vel || !guest_read(vel + 4, &v, sizeof v)) return 0;
    const float part = jump_part_capped(jump_part, v, vmax, dt, mass);
    if (part != jump_part) {
        c->v[8].f[0] = part;
        // the horizontal speed the cap would have taken in the next frame (ComputeMovement reads the velocity again after JumpInput)
        const float s = jump_cap_scale(jump_part, v, vmax, dt, mass);
        float h[3];
        if (s < 1.f && guest_read(vel, h, sizeof h)) {
            h[0] *= s;
            h[2] *= s;
            guest_write(vel, h, sizeof h);
        }
        const uint64_t n = g_s.capped.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((n & (n - 1)) == 0)        // 1, 2, 4, 8, ...
            NXLOG_I(kTag, "jump timer ended inside a %.2f ms step at the rise cap: jump part %.3f -> %.3f (v %.3f, cap %.3f, %llu so far)",
                    static_cast<double>(dt) * 1000.0, static_cast<double>(jump_part), static_cast<double>(part), static_cast<double>(v),
                    static_cast<double>(vmax), static_cast<unsigned long long>(n));
    }
    return 0;
}
}  // namespace

float jump_part_capped(float jump_part, float v, float vmax, float dt, float mass) {
    if (!(dt > 0.f) || !(mass > 0.f) || !std::isfinite(jump_part) || !std::isfinite(v) || !std::isfinite(vmax) || !std::isfinite(dt)) return jump_part;
    const double vc = std::fmin(static_cast<double>(v), static_cast<double>(vmax));      // the cap acts on the speed entering the step
    const double room = static_cast<double>(mass) * (static_cast<double>(vmax) - vc) / static_cast<double>(dt);
    if (static_cast<double>(jump_part) <= room) return jump_part;
    return static_cast<float>(room);
}

float jump_cap_scale(float jump_part, float v, float vmax, float dt, float mass) {
    if (!(dt > 0.f) || !(mass > 0.f) || !(vmax > 0.f) || !std::isfinite(jump_part) || !std::isfinite(v) || !std::isfinite(vmax) || !std::isfinite(dt)) return 1.f;
    const double reach = std::fmin(static_cast<double>(v), static_cast<double>(vmax)) + static_cast<double>(jump_part) / mass * dt;
    return reach > vmax ? static_cast<float>(vmax / reach) : 1.f;
}

float jump_end_force(float v, float vmax, float a_jump, float a_gravity, float timer, float dt, float mass) {
    const float f = timer / dt;                                  // as the game: FDIV S0, timer, dt
    const float jump_part = f * (a_jump * mass);                 // FMUL S8, f, jump force
    const float gravity_part = ((1.f - f) * a_gravity) * mass;  // FSUB, FMUL S9, then FMUL S0, S9, mass
    return jump_part_capped(jump_part, v, vmax, dt, mass) + gravity_part;
}

void jump_install(uint64_t game_base) {
    if (g_s.ids[0] || g_s.ids[1]) return;
    if (!config_get_bool("physics", "jump_fix", true)) {
        NXLOG_I(kTag, "jump end fix off ([physics] jump_fix = 0)");
        return;
    }
    g_s.base = game_base;
    g_s.ids[0] = hook_add_entry(game_base + site::kJumpInput, &entry_hook);
    g_s.ids[1] = hook_add_mid(game_base + site::kJumpInputBlend, &blend_hook);
}

void jump_remove() {
    for (uint32_t& id : g_s.ids) {
        if (id) hook_remove(id);
        id = 0;
    }
}

uint64_t jump_blend_frames() { return g_s.blends.load(); }
uint64_t jump_capped_frames() { return g_s.capped.load(); }

}  // namespace nxrt::play
