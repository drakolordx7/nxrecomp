// SPDX-License-Identifier: GPL-3.0-or-later
// Game-state probes (S5): read the player transform, world/area ids, health and play time out of guest memory, install the two
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

#include <cctype>
#include <cmath>
#include <cstdlib>
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

std::atomic<uint64_t> g_player{0}, g_mgr{0};
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
int input_hook(::Ctx* c, uint64_t, void*) { set_player_pointers(c->x[0], c->x[2]); return 0; }

}  // namespace

void set_probe_layout(const ProbeLayout& l) { g_layout = l; }
void set_player_pointers(uint64_t player, uint64_t mgr) { g_player.store(player); g_mgr.store(mgr); }

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
            g.yaw = std::atan2(-m[1], m[5]);
            rd(player + 0x3f0, &g.move_state);
            g.have_player = std::isfinite(g.pos[0]) && std::isfinite(g.pos[1]) && std::isfinite(g.pos[2]);
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

bool grant(int64_t item, int32_t amount, int32_t capacity) {
    const uint64_t gs = rd64(g_layout.game_state_global ? g_layout.game_state_global : game_base() + kGpGameState);
    const uint64_t ps = gs ? rd64(gs + 0x20) : 0;
    if (!ps) return false;
    auto put = [&](int64_t id, int32_t a, int32_t cap) {
        const int32_t v[2] = {a, cap};
        return write_mem(ps + 0x44 + static_cast<uint64_t>(id) * 8, v, sizeof v);
    };
    if (item >= 0) return put(item, amount, capacity);
    bool ok = true;
    for (int id : {0, 1, 2, 3, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 0x1c}) ok &= put(id, 1, 1);
    ok &= put(4, 250, 250);       // missiles
    ok &= put(7, 8, 8);           // power bombs
    ok &= put(0x18, 14, 14);      // energy tanks
    return ok;
}

bool call_guest(::Ctx* c, uint64_t offset, const std::string& args, uint64_t* result, std::string* err) {
    if (!c) { *err = "no live context (the frame source is not the game hook)"; return false; }
    const uint64_t base = game_base();
    const uint64_t gs = rd64(g_layout.game_state_global ? g_layout.game_state_global : base + kGpGameState);
    const uint64_t mgr = g_layout.state_manager ? g_layout.state_manager : g_mgr.load();
    uint64_t a[6] = {0, 0, 0, 0, 0, 0};
    size_t n = 0;
    size_t pos = 0;
    while (pos < args.size()) {
        while (pos < args.size() && std::isspace(static_cast<unsigned char>(args[pos]))) pos++;
        if (pos >= args.size()) break;
        size_t e = pos;
        while (e < args.size() && !std::isspace(static_cast<unsigned char>(args[e]))) e++;
        const std::string tok = args.substr(pos, e - pos);
        pos = e;
        if (n >= 6) { *err = "more than 6 arguments"; return false; }
        uint64_t v = 0;
        if (tok == "$gs") v = gs;
        else if (tok == "$ps") v = gs ? rd64(gs + 0x20) : 0;
        else if (tok == "$mgr") v = mgr;
        else if (tok == "$player") { const uint64_t logic = mgr ? rd64(mgr + 0x4e0150) : 0; v = g_layout.player ? g_layout.player : (logic ? rd64(logic + 0x18) : g_player.load()); }
        else {
            char* end = nullptr;
            v = std::strtoull(tok.c_str(), &end, 0);
            if (!end || *end != 0) { *err = "bad argument '" + tok + "'"; return false; }
        }
        if ((tok[0] == '$') && v == 0) { *err = "argument " + tok + " is not available yet"; return false; }
        a[n++] = v;
    }
    ::Ctx saved = *c;
    for (int i = 0; i < 6; i++) c->x[i] = a[i];
    dispatch_call(c, base + offset);
    *result = c->x[0];
    *c = saved;
    return true;
}

}  // namespace detail
}  // namespace nxrt::tt
