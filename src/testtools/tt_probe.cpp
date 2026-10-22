// SPDX-License-Identifier: GPL-3.0-or-later
// Game-state probes (stream S5): read the player transform, world/area ids, health and play time out of guest memory, install the two
// entry hooks the tooling needs, and run the practice mod's warp sequence. Guest pointers are host pointers (memory.h), so a probe is a
// chain of reads; every read goes through ReadProcessMemory on this process so that a stale pointer gives "not available", not a fault.
//
// Layout, all main-module offsets of the v1.0.0 binary, each read off the disassembly of the named function (work/elf/main.elf):
//   gpGameState                   main+0x459cc70      pointer to CGameStateMP1 (symbol, verified)
//   CGameStateMP1                 +0x00 current world id (CObjectId, 16 bytes)   CGameStateMP1::SetCurrentWorldId stores it with stp [x0]
//                                 +0x10 int32 count of world states, +0x18 pointer to the array   CGameStateMP1::StateForWorld (0xc8424c)
//                                 +0x20 pointer to CPlayerStateMP1                CGameStateMP1::PlayerState (0xc84488) = this+0x20 then ldr
//   CWorldStateMP1 (0x50 bytes)   +0x00 world id, +0x10 current area id, +0x40 desired area id   (SetDesiredAreaAssetId 0xc8275c: stp [x0,#0x40])
//   CPlayerStateMP1               +0x0c health (float)                            CPlayerMP1::HealthInfo (0xc740fc) = PlayerState()+0xc
//   CStateManager                 +0x08 -> +0x08 = CGameState*                    CStateManager::GameState (0xb0275c)
//                                 +0x4e0150 = CStateManagerGameLogicMP1*, its +0x18 = CPlayerMP1*   (CPlayerMP1::HealthInfo; GameLogic::PlayerActor 0xc652e8)
//   CGameState                    +0xffb08 double play time                       (practice mod, GetPlayTime; unverified by disassembly)
//   CActorMP1                     +0x08 -> +0x20 -> +0x40 = CTransform4f* (12 floats, rows of 4, origin in column 3)   CActorMP1::GetTransform (0xc4ba10)
//   CPlayerMP1                    +0x3f0 EPlayerMovementState (u32)
// The warp: SetCurrentWorldId(gpGameState, world); CurrentWorldState()->SetDesiredAreaAssetId(area); CGameState::ContinueWithNewInitialState(
// CStateManager::GameState(), SInitialState{area, -1}); CGameInstanceState::QuitGame(7) with the instance state from CStateManager::GameInstanceState().
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "tt_internal.h"

namespace nxrt::tt {
namespace detail {
namespace {
std::atomic<uint64_t> g_base{kDefaultGameBase};
}
uint64_t game_base() { return g_base.load(); }
void set_game_base(uint64_t b) { g_base.store(b); }

bool read_mem(uint64_t addr, void* out, size_t n) {
    if (addr < 0x10000) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(addr), out, n, &got) && got == n;
}
bool write_mem(uint64_t addr, const void* in, size_t n) {
    if (addr < 0x10000) return false;
    SIZE_T got = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(addr), in, n, &got) && got == n;
}
}  // namespace detail

namespace {

constexpr uint64_t kGpGameState = 0x459cc70;
constexpr uint64_t kWorldStateSize = 0x50;

constexpr uint64_t kFnSetCurrentWorldId = 0xc838e4;     // (CGameStateMP1*, CObjectId)
constexpr uint64_t kFnCurrentWorldState = 0xc84478;     // (CGameStateMP1*) -> CWorldStateMP1*
constexpr uint64_t kFnSetDesiredArea = 0xc8275c;        // (CWorldStateMP1*, CObjectId)
constexpr uint64_t kFnContinueWithNewInitialState = 0x40a820;   // (CGameState&, SInitialState const&)
constexpr uint64_t kFnGameInstanceState = 0xb01cc8;     // (CStateManager*) -> CGameInstanceState*
constexpr uint64_t kFnQuitGame = 0x410544;              // (CGameInstanceState*, int reason)
constexpr uint64_t kFnUpdate = 0x3c81c8;                // CGameArchitectureSupport::Update(float*): the frame boundary
constexpr uint64_t kFnProcessInput = 0xc70334;          // CPlayerMP1::ProcessInput(CFinalInput const&, CStateManager&): x0 player, x2 state manager
constexpr int kQuitReasonWarp = 7;

std::atomic<uint64_t> g_player{0}, g_mgr{0}, g_pi_stamp{0};
std::atomic<::Ctx*> g_call_ctx{nullptr};
constexpr uint64_t kFnGetVelocityWR = 0xc577c4;        // CPhysicsActorMP1::GetVelocityWR(player, CStateManager const&) -> CVector3f*
ProbeLayout g_layout;

template <class T>
bool rd(uint64_t a, T* v) { return detail::read_mem(a, v, sizeof(T)); }

uint64_t rd64(uint64_t a) { uint64_t v = 0; rd(a, &v); return v; }

uint64_t guest_call(::Ctx* c, uint64_t addr, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0) {
    ::Ctx saved = *c;
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2;
    dispatch_call(c, addr);
    const uint64_t r = c->x[0];
    *c = saved;
    return r;
}

int frame_hook(::Ctx* c, uint64_t, void*) { frame_boundary(c); return 0; }
int input_hook(::Ctx* c, uint64_t, void*) { set_player_pointers(c->x[0], c->x[2]); g_pi_stamp.store(frame()); return 0; }

}  // namespace

void set_probe_layout(const ProbeLayout& l) { g_layout = l; }
void set_player_pointers(uint64_t player, uint64_t mgr) { g_player.store(player); g_mgr.store(mgr); }
void set_call_context(::Ctx* c) { g_call_ctx.store(c); }

namespace detail {
bool have_state_manager() { return (g_layout.state_manager ? g_layout.state_manager : g_mgr.load()) != 0; }
}

bool read_state(GameState* out) {
    GameState g;
    const uint64_t base = detail::game_base();
    const uint64_t gp = g_layout.game_state_global ? g_layout.game_state_global : base + kGpGameState;
    const uint64_t gs = rd64(gp);
    if (gs) {
        uint8_t world[16];
        int32_t count = 0;
        uint64_t arr = 0;
        if (rd(gs, &world) && rd(gs + 0x10, &count) && rd(gs + 0x18, &arr) && count > 0 && count < 64 && arr) {
            std::memcpy(g.world, world, 16);
            for (int32_t i = 0; i < count; i++) {
                uint8_t e[16];
                if (rd(arr + i * kWorldStateSize, &e) && std::memcmp(e, world, 16) == 0) {
                    if (rd(arr + i * kWorldStateSize + 0x10, &g.area)) g.have_world = true;
                    break;
                }
            }
        }
        const uint64_t ps = rd64(gs + 0x20);
        float hp;
        if (ps && rd(ps + 0xc, &hp)) { g.hp = hp; g.have_hp = true; }
        int32_t beam = -1, visor = -1, missiles = -1;
        if (ps && rd(ps + 0x8, &beam) && rd(ps + 0x18, &visor) && rd(ps + 0x30 + 8 * 4, &missiles)) {
            g.beam = beam; g.visor = visor; g.missiles = missiles; g.have_ps = true;
        }
    }
    const uint64_t mgr = g_layout.state_manager ? g_layout.state_manager : g_mgr.load();
    uint64_t player = g_layout.player ? g_layout.player : 0;
    if (mgr) {
        const uint64_t cgs = rd64(rd64(mgr + 8) + 8);
        double igt;
        if (cgs && rd(cgs + 0xffb08, &igt) && std::isfinite(igt)) { g.igt = igt; g.have_igt = true; }
        if (!player) {
            const uint64_t logic = rd64(mgr + 0x4e0150);
            if (logic) player = rd64(logic + 0x18);
        }
    }
    if (!player) player = g_player.load();
    if (player) {
        const uint64_t xf = rd64(rd64(rd64(player + 8) + 0x20) + 0x40);
        float m[12];
        if (xf && rd(xf, &m)) {
            g.pos[0] = m[3]; g.pos[1] = m[7]; g.pos[2] = m[11];
            g.yaw = std::atan2(m[2], m[0]);        // heading about Y (the world is Y up; forward is -Z at 0; + = turn left)
            rd(player + 0x3f0, &g.move_state);
            g.have_player = std::isfinite(g.pos[0]) && std::isfinite(g.pos[1]) && std::isfinite(g.pos[2]);
        }
        if (g.have_player && mgr) {            // the first-person camera: [[[mgr + 0x4e0150] + 0x30] + 0x80]
            const uint64_t logic = rd64(mgr + 0x4e0150);
            const uint64_t cm = logic ? rd64(logic + 0x30) : 0;
            const uint64_t fp = cm ? rd64(cm + 0x80) : 0;
            float cx[12];
            const uint64_t xfp = fp ? rd64(rd64(rd64(fp + 8) + 0x20) + 0x40) : 0;
            if (xfp && rd(xfp, &cx) && std::isfinite(cx[2]) && std::isfinite(cx[0])) {
                g.cam_yaw = std::atan2(cx[2], cx[0]);
                g.cam_pitch = std::asin(std::max(-1.0f, std::min(1.0f, -cx[6])));
                g.have_cam = true;
            }
        }
        if (g.have_player) {
            float pitch = 0;
            int32_t cs = 0, ms = 0, os = 0;
            if (rd(player + 0x5cc, &pitch) && rd(player + 0x4d0, &cs) && rd(player + 0x4d4, &ms) && rd(player + 0x4e0, &os)) {
                g.pitch = pitch; g.cam_state = cs; g.morph_state = ms; g.orbit_state = os; g.have_aim = true;
            }
            // The physics velocity needs a guest call, so only from inside the frame hook and only while the game is calling ProcessInput
            // (the player pointer is then live; during loads it is stale).
            ::Ctx* c = g_call_ctx.load();
            const uint64_t mgr_now = g_layout.state_manager ? g_layout.state_manager : g_mgr.load();
            if (c && mgr_now && frame() - g_pi_stamp.load() <= 2 && fn_lookup(detail::game_base() + kFnGetVelocityWR)) {
                const uint64_t vp = guest_call(c, detail::game_base() + kFnGetVelocityWR, player, mgr_now);
                float v[3];
                if (vp && rd(vp, &v) && std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2])) {
                    for (int i = 0; i < 3; i++) g.vel[i] = v[i];
                    g.speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                    g.have_phys = true;
                }
            }
        }
    }
    *out = g;
    return g.have_world || g.have_player || g.have_igt || g.have_hp;
}

void install_game_hooks() {
    const uint64_t base = detail::game_base();
    hook_add_entry(base + kFnUpdate, frame_hook, nullptr);
    hook_add_entry(base + kFnProcessInput, input_hook, nullptr);
    NXLOG_I("tt", "game hooks registered: frame boundary at 0x%llx, state capture at 0x%llx (they fire only if the lifted game was built with tests/sites/mpr_testtools.sites)",
            static_cast<unsigned long long>(base + kFnUpdate), static_cast<unsigned long long>(base + kFnProcessInput));
}

namespace detail {

bool give_item(int item, int amount) {
    if (item < 0 || item > 40) return false;
    const uint64_t gp = g_layout.game_state_global ? g_layout.game_state_global : game_base() + kGpGameState;
    const uint64_t gs = rd64(gp);
    const uint64_t ps = gs ? rd64(gs + 0x20) : 0;
    if (!ps) return false;
    const int32_t v[2] = {amount, amount};
    return write_mem(ps + 0x30 + 8 * static_cast<uint64_t>(item), v, sizeof v);
}

bool warp(::Ctx* c, const uint8_t world[16], const uint8_t area[16]) {
    const uint64_t base = game_base();
    const uint64_t mgr = g_layout.state_manager ? g_layout.state_manager : g_mgr.load();
    const uint64_t gp = g_layout.game_state_global ? g_layout.game_state_global : base + kGpGameState;
    const uint64_t gs = rd64(gp);
    if (!c || !mgr || !gs) return false;
    const uint64_t cgs = rd64(rd64(mgr + 8) + 8);      // CStateManager::GameState()
    if (!cgs) return false;
    uint64_t w[2], a[2];
    std::memcpy(w, world, 16);
    std::memcpy(a, area, 16);
    guest_call(c, base + kFnSetCurrentWorldId, gs, w[0], w[1]);
    const uint64_t ws = guest_call(c, base + kFnCurrentWorldState, gs);
    if (!ws) return false;
    guest_call(c, base + kFnSetDesiredArea, ws, a[0], a[1]);
    alignas(16) static uint8_t initial[32];             // SInitialState { CObjectId id; int slot = -1 }
    std::memset(initial, 0, sizeof initial);
    std::memcpy(initial, a, 16);
    const int32_t slot = -1;
    std::memcpy(initial + 16, &slot, 4);
    guest_call(c, base + kFnContinueWithNewInitialState, cgs, reinterpret_cast<uint64_t>(initial));
    const uint64_t gis = guest_call(c, base + kFnGameInstanceState, mgr);
    if (!gis) return false;
    guest_call(c, base + kFnQuitGame, gis, static_cast<uint64_t>(kQuitReasonWarp));
    return true;
}

}  // namespace detail
}  // namespace nxrt::tt
