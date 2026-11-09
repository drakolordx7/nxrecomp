// SPDX-License-Identifier: GPL-3.0-or-later
// Game-state probes of Metroid Prime Remastered (S5; tt::GameProbes, nxrt/tt.h): read the player transform, world/area ids, health and play time out of guest memory, install the two
// entry hooks the tooling needs, and run the practice mod's warp sequence. The script steps built on them are in mpr_tt_steps.cpp. Guest pointers are host pointers (memory.h), so a probe is a
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
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "mpr_sites.h"
#include "mpr_tt_probe.h"
#include "nxrt/game/tt_probe.h"
#include "nxrt/memory.h"
#include "nxrt/tt.h"
#include "nxrt/tt_script.h"
#include "mpr_addrs.h"

// The probes live in their own namespace and reach the test tooling through tt::GameProbes (nxrt/tt.h, mpr_game_probes() below). The memory
// helpers they use are the base's (src/testtools/tt_game.cpp).
namespace nxrt::tt::mpr {
namespace detail {
using nxrt::tt::detail::read_mem;
using nxrt::tt::detail::write_mem;
}

namespace {

using namespace nxrt::games::mpr::addr;
constexpr uint64_t kWorldStateSize = 0x50;

constexpr uint64_t kFnUpdate = games::mpr::site::kUpdate;   // CGameArchitectureSupport::Update(float*): the frame boundary (mpr_sites.h)
constexpr int kQuitReasonWarp = 7;
// The game's resource loader (S42), for the warp gate: TOneStatic<CGameGlobalObjects> lives at main+0x4578710 (CGameArchitecture ctor 0x3c635c);
// CGameGlobalObjects +0x00 = CResourceFactory* (its ctor 0x3c9174); CResourceFactory +0x10 = CResourceLoaderManager* (strong_ptr {control, object}
// at +0x08, ctor 0x331558). CResourceLoaderManager::IsThreadWorkDone (0xc7458) is: ints at +0xe8 (load list), +0xa4 (request queue), +0x138 and
// +0x110 all 0, and the byte at +0x144 is 0.
constexpr uint64_t kFactoryLoader = 0x10;
constexpr uint64_t kLoaderCounts[] = {0xe8, 0xa4, 0x138, 0x110};
constexpr uint64_t kLoaderBusyByte = 0x144;

std::atomic<uint64_t> g_player{0}, g_mgr{0}, g_pi_stamp{0};
ProbeLayout g_layout;

// Base of the main module: the probe layout's, else [test] game_base, else where the console maps main.
uint64_t game_base() {
    return g_layout.game_base ? g_layout.game_base : static_cast<uint64_t>(config_get_int("test", "game_base", static_cast<int64_t>(mem::kMainBase)));
}

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

}

void set_probe_layout(const ProbeLayout& l) { g_layout = l; }
void set_player_pointers(uint64_t player, uint64_t mgr) { g_player.store(player); g_mgr.store(mgr); }

namespace detail {
LoaderState loader_state(uint32_t* pending) {
    const uint64_t globals = g_layout.game_globals ? g_layout.game_globals : game_base() + kGameGlobals;
    const uint64_t factory = rd64(globals);
    const uint64_t mgr = factory ? rd64(factory + kFactoryLoader) : 0;
    if (!mgr) return LoaderState::Unknown;
    uint32_t sum = 0;
    for (uint64_t off : kLoaderCounts) {
        int32_t n = 0;
        if (!rd(mgr + off, &n)) return LoaderState::Unknown;
        if (n > 0) sum += static_cast<uint32_t>(n);
    }
    uint8_t busy = 0;
    if (!rd(mgr + kLoaderBusyByte, &busy)) return LoaderState::Unknown;
    if (pending) *pending = sum;
    return (sum == 0 && busy == 0) ? LoaderState::Idle : LoaderState::Busy;
}
}

bool read_state(GameState* out, ::Ctx* c) {
    struct {                        // what the engine's memory says, before it becomes the tooling's fields
        bool have_world = false, have_player = false, have_igt = false, have_hp = false, have_vel = false;
        uint8_t world[16] = {}, area[16] = {};
        float pos[3] = {}, vel[3] = {}, yaw = 0, hp = 0;
        double igt = 0;
    } g;
    const GameState* previous = out->previous;
    uint32_t move_state = 0;
    bool have_phys = false, have_aim = false, have_cam = false, have_ps = false;
    float speed = 0, pitch = 0, cam_yaw = 0, cam_pitch = 0;
    int32_t cam_state = -1, morph_state = -1, orbit_state = -1, beam = -1, visor = -1, missiles = -1;
    const uint64_t base = game_base();
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
        int32_t b = -1, v = -1, m = -1;
        if (ps && rd(ps + 0x8, &b) && rd(ps + 0x18, &v) && rd(ps + 0x2c + 8 * 4, &m)) { beam = b; visor = v; missiles = m; have_ps = true; }
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
            g.yaw = std::atan2(m[2], m[0]);        // heading about Y (the world is Y up; forward is +Z at 0; + = turn left)
            rd(player + 0x3f0, &move_state);
            g.have_player = std::isfinite(g.pos[0]) && std::isfinite(g.pos[1]) && std::isfinite(g.pos[2]);
        }
        if (g.have_player && mgr) {            // the first-person camera: [[[mgr + 0x4e0150] + 0x30] + 0x80]
            const uint64_t logic = rd64(mgr + 0x4e0150);
            const uint64_t cm = logic ? rd64(logic + 0x30) : 0;
            const uint64_t fp = cm ? rd64(cm + 0x80) : 0;
            float cx[12];
            const uint64_t xfp = fp ? rd64(rd64(rd64(fp + 8) + 0x20) + 0x40) : 0;
            if (xfp && rd(xfp, &cx) && std::isfinite(cx[2]) && std::isfinite(cx[0])) {
                cam_yaw = std::atan2(cx[2], cx[0]);
                cam_pitch = std::asin(std::max(-1.0f, std::min(1.0f, cx[6])));
                have_cam = true;
            }
        }
        if (g.have_player) {
            float pt = 0;
            int32_t cs = 0, ms = 0, os = 0;
            if (rd(player + 0x5cc, &pt) && rd(player + 0x4d0, &cs) && rd(player + 0x4d4, &ms) && rd(player + 0x4e0, &os)) {
                pitch = pt; cam_state = cs; morph_state = ms; orbit_state = os; have_aim = true;
            }
            // The physics velocity needs a guest call, so only from inside the frame hook and only while the game is calling ProcessInput
            // (the player pointer is then live; during loads it is stale).
            const uint64_t mgr_now = g_layout.state_manager ? g_layout.state_manager : g_mgr.load();
            if (c && mgr_now && frame() - g_pi_stamp.load() <= 2 && fn_lookup(game_base() + kFnGetVelocityWR)) {
                const uint64_t vp = guest_call(c, game_base() + kFnGetVelocityWR, player, mgr_now);
                float v[3];
                if (vp && rd(vp, &v) && std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2])) {
                    for (int i = 0; i < 3; i++) g.vel[i] = v[i];
                    speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                    have_phys = true;
                    g.have_vel = true;
                }
            }
        }
    }
    if (!g.have_vel && previous && g.have_player) {       // the game is not ticking the player: the position difference, units per second at 60 frames/s
        const StateField* pp = previous->field("pos");
        if (pp && pp->shown) for (int i = 0; i < 3; i++) g.vel[i] = (g.pos[i] - pp->vec[i]) * 60.0f;
    }
    // The fields, in the order the hash folds them (the hash is the one the tooling computed before the state went generic: ids; position and velocity
    // words alternating; heading, health, play time; state; which groups were readable; pitch; cam|morph; orbit|beam; visor|missiles). The trace prints
    // them in this order too.
    using H = StateField::Hash;
    GameState& o = *out;
    o = GameState();
    o.hash_flags = (g.have_world ? 1u : 0u) | (g.have_player ? 2u : 0u) | (g.have_igt ? 4u : 0u) | (g.have_hp ? 8u : 0u);
    o.add_id("world", g.world, g.have_world);
    o.add_id("area", g.area, g.have_world);
    for (int i = 0; i < 3; i++) { o.add_float("", g.pos[i]); o.add_float("", g.vel[i]); }          // hash only
    o.add_vec3("pos", g.pos, g.have_player);
    o.add_vec3("vel", g.vel, g.have_player);
    o.add_float("yaw", g.yaw, g.have_player);
    o.add_float("hp", g.hp, g.have_hp);
    o.fields.back().alias = "health";
    o.add_float("igt", g.igt, g.have_igt, H::Wide);
    static const char* const axis[] = {"px", "py", "pz", "vx", "vy", "vz"};                         // what @assert can name; not in the trace
    for (int i = 0; i < 6; i++) {
        o.add_float(axis[i], i < 3 ? g.pos[i] : g.vel[i - 3], g.have_player, H::None);
        o.fields.back().quiet = true;
    }
    o.add_float("speed", std::sqrt(double(g.vel[0]) * g.vel[0] + double(g.vel[1]) * g.vel[1] + double(g.vel[2]) * g.vel[2]), g.have_player, H::None);
    o.fields.back().quiet = true;
    const bool pl = g.have_player;
    o.add_int("state", move_state, pl);                                              // EPlayerMovementState
    o.add_int("", (have_phys ? 1 : 0) | (have_aim ? 2 : 0) | (have_ps ? 4 : 0));     // hash only
    o.add_int("phys", 1, pl && have_phys, H::None);                                  // vel is the physics object's velocity, not a position difference
    o.add_float("spd", speed, pl && have_phys, H::None);
    o.add_float("pitch", pitch, have_aim);                                           // free-look pitch, radians, + = up
    o.add_int("cam", cam_state, have_aim);                                           // EPlayerCameraState (0 first person)
    o.add_int("morph", morph_state, have_aim, H::High);                              // morph transition state word (0 = unmorphed)
    o.add_int("orbit", orbit_state, have_aim);                                       // EPlayerOrbitState (0 = not locked on)
    o.add_float("camyaw", cam_yaw, have_cam, H::None);                               // the first-person camera's heading and pitch
    o.add_float("campitch", cam_pitch, have_cam, H::None);
    o.add_int("beam", beam, have_ps, H::High);                                       // 0 power 1 ice 2 wave 3 plasma
    o.add_int("visor", visor, have_ps);                                              // 0 combat 1 x-ray 2 scan 3 thermal
    o.add_int("msl", missiles, have_ps, H::High, "missiles");                        // item 4 amount
    return g.have_world || g.have_player || g.have_igt || g.have_hp;
}

static void install_game_hooks() {
    const uint64_t base = game_base();
    hook_add_entry(base + kFnUpdate, frame_hook, nullptr);
    hook_add_entry(base + kFnProcessInput, input_hook, nullptr);
    NXLOG_I("tt", "game hooks registered: frame boundary at 0x%llx, state capture at 0x%llx (they fire only if the lifted game was built with recomp/main.sites)",
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
    return write_mem(ps + 0x2c + 8 * static_cast<uint64_t>(item), v, sizeof v);       // CPlayerStateMP1: {amount, capacity} per item from +0x2c (CPlayerStateMP1::SetPowerUp)
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

bool grant(int64_t item, int32_t amount, int32_t capacity) {
    const uint64_t gs = rd64(g_layout.game_state_global ? g_layout.game_state_global : game_base() + kGpGameState);
    const uint64_t ps = gs ? rd64(gs + 0x20) : 0;
    if (!ps) return false;
    auto put = [&](int64_t id, int32_t a, int32_t cap) {
        const int32_t v[2] = {a, cap};
        return write_mem(ps + 0x2c + static_cast<uint64_t>(id) * 8, v, sizeof v);
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


// ---- @watch: hardware write watch on the player's velocity.y / position.y (S25) ------------------------------------------------------------
// Debug registers DR0 (velocity.y) and DR1 (position.y) are set on every thread; the write traps into a vectored handler that records the writing
// instruction pointer, the value written and the module-range return addresses on the stack (guest functions are native frames of the lifted code).
// Hits are logged once per frame; resolve the addresses with llvm-symbolizer against mpr.exe / mpr.pdb.
namespace {
struct WatchHit { uint64_t rip; uint64_t addr; float value; int slot; int nstack; uint64_t stack[10]; DWORD tid; };
constexpr int kMaxHits = 4096;
WatchHit g_hits[kMaxHits];
std::atomic<int> g_hit_count{0};
std::atomic<int> g_hit_logged{0};
std::atomic<bool> g_watch_on{false};
uint64_t g_watch_addr[2] = {0, 0};
void* g_veh = nullptr;
uint64_t g_mod_lo = 0, g_mod_hi = 0;

LONG CALLBACK watch_veh(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_watch_on.load()) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* cx = ep->ContextRecord;
    const DWORD64 dr6 = cx->Dr6;
    if (!(dr6 & 0x3)) return EXCEPTION_CONTINUE_SEARCH;
    for (int slot = 0; slot < 2; slot++) {
        if (!(dr6 & (1u << slot))) continue;
        const int i = g_hit_count.fetch_add(1);
        if (i >= kMaxHits) continue;
        WatchHit& h = g_hits[i];
        h.rip = cx->Rip; h.slot = slot; h.addr = g_watch_addr[slot]; h.tid = GetCurrentThreadId();
        h.value = *reinterpret_cast<volatile float*>(g_watch_addr[slot]);
        h.nstack = 0;
        const uint64_t* sp = reinterpret_cast<const uint64_t*>(cx->Rsp);
        for (int k = 0; k < 512 && h.nstack < 10; k++) {
            uint64_t v = 0;
            if (IsBadReadPtr(sp + k, 8)) break;
            v = sp[k];
            if (v >= g_mod_lo && v < g_mod_hi) h.stack[h.nstack++] = v;
        }
    }
    cx->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void set_debug_regs(bool on) {
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
        if (!th) continue;
        const bool self = te.th32ThreadID == GetCurrentThreadId();
        if (!self) SuspendThread(th);
        CONTEXT cx{};
        cx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(th, &cx)) {
            cx.Dr0 = on ? g_watch_addr[0] : 0;
            cx.Dr1 = on ? g_watch_addr[1] : 0;
            DWORD64 d7 = 0;
            if (on) {
                if (g_watch_addr[0]) d7 |= (1ull << 0) | (1ull << 16) | (3ull << 18);      // slot 0: local enable, write, 4 bytes
                if (g_watch_addr[1]) d7 |= (1ull << 2) | (1ull << 20) | (3ull << 22);      // slot 1
            }
            cx.Dr7 = d7;
            cx.Dr6 = 0;
            SetThreadContext(th, &cx);
        }
        if (!self) ResumeThread(th);
        CloseHandle(th);
    }
    CloseHandle(snap);
}
}

void watch_stop();

bool watch_start(::Ctx* c, int what) {
    uint64_t player = g_layout.player ? g_layout.player : g_player.load();
    const uint64_t mgr = g_layout.state_manager ? g_layout.state_manager : g_mgr.load();
    if (!c || !player || !mgr) return false;
    if (g_watch_on.load()) watch_stop();
    g_watch_addr[0] = g_watch_addr[1] = 0;
    if (what & 1) {
        const uint64_t vp = guest_call(c, game_base() + kFnGetVelocityWR, player, mgr);
        if (!vp) return false;
        g_watch_addr[0] = vp + 4;
    }
    if (what & 2) {
        const uint64_t xf = rd64(rd64(rd64(player + 8) + 0x20) + 0x40);
        if (!xf) return false;
        g_watch_addr[1] = xf + 28;
    }
    HMODULE m = GetModuleHandleA(nullptr);
    MODULEINFO mi{};
    GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof mi);
    g_mod_lo = reinterpret_cast<uint64_t>(mi.lpBaseOfDll);
    g_mod_hi = g_mod_lo + mi.SizeOfImage;
    g_hit_count = 0; g_hit_logged = 0;
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, watch_veh);
    g_watch_on = true;
    set_debug_regs(true);
    NXLOG_I("tt", "watch: on (vel.y at 0x%llx, pos.y at 0x%llx), module base 0x%llx (frame %llu)", static_cast<unsigned long long>(g_watch_addr[0]),
            static_cast<unsigned long long>(g_watch_addr[1]), static_cast<unsigned long long>(g_mod_lo), static_cast<unsigned long long>(frame()));
    return true;
}

void watch_flush(uint64_t fr) {
    if (!g_watch_on.load()) return;
    const int n = std::min(g_hit_count.load(), kMaxHits);
    for (int i = g_hit_logged.load(); i < n; i++) {
        const WatchHit& h = g_hits[i];
        char b[512];
        int o = std::snprintf(b, sizeof b, "watch: frame %llu %s=%g rip=+0x%llx tid=%lu stack:", static_cast<unsigned long long>(fr), h.slot == 0 ? "vel.y" : "pos.y", h.value,
                              static_cast<unsigned long long>(h.rip - g_mod_lo), h.tid);
        for (int k = 0; k < h.nstack && o < 480; k++) o += std::snprintf(b + o, sizeof b - o, " +0x%llx", static_cast<unsigned long long>(h.stack[k] - g_mod_lo));
        NXLOG_I("tt", "%s", b);
    }
    g_hit_logged = n;
}

void watch_stop() {
    if (!g_watch_on.load()) return;
    watch_flush(frame());
    set_debug_regs(false);
    g_watch_on = false;
    NXLOG_I("tt", "watch: off, %d hits", g_hit_count.load());
}


// ---- @rays: batch of static-collision ray casts through the game's own NCollisionMP1::RayStaticIntersection (S26) -------------------------------
// Used by the route planner (nav.py, a local test script) to read the walkable geometry of the loaded areas without parsing any game file.
// A host fault inside the guest call (a bad pointer) is caught here: the ray counts as a miss and the run goes on.
bool seh_call(::Ctx* c, uint64_t addr) {
    __try { dispatch_call(c, addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
                                                          // +0 t, +4 point xyz, +16 normal xyz, +28 f, +32 hit (byte)
bool cast_rays(::Ctx* c, const std::string& in_path, const std::string& out_path, std::string* err) {
    const uint64_t mgr = g_layout.state_manager ? g_layout.state_manager : g_mgr.load();
    if (!c || !mgr) { *err = "no state manager (not in a level)"; return false; }
    FILE* in = std::fopen(in_path.c_str(), "rb");
    if (!in) { *err = "cannot read " + in_path; return false; }
    FILE* out = std::fopen(out_path.c_str(), "wb");
    if (!out) { std::fclose(in); *err = "cannot write " + out_path; return false; }
    const uint64_t fn = game_base() + kFnRayStaticIntersection;
    // the filter argument (x3): CIncludeExcludeCollisionFilterMP1 built from CMaterialFilterMP1::skPassEverything (every surface passes)
    alignas(16) uint8_t filt[512] = {};
    {
        ::Ctx saved = *c;
        c->x[0] = reinterpret_cast<uint64_t>(filt); c->x[1] = game_base() + kSkPassEverything;
        dispatch_call(c, game_base() + kFnMakeIncludeExcludeFilter);
        *c = saved;
    }
    size_t n = 0, nhit = 0, nfault = 0;
    float r[6], len;
    while (std::fscanf(in, "%f %f %f %f %f %f %f", &r[0], &r[1], &r[2], &r[3], &r[4], &r[5], &len) == 7) {
        alignas(16) float start[4] = {r[0], r[1], r[2], 0}, dir[4] = {r[3], r[4], r[5], 0};
        alignas(16) uint8_t res[64] = {};
        ::Ctx saved = *c;
        c->x[0] = mgr; c->x[1] = reinterpret_cast<uint64_t>(start); c->x[2] = reinterpret_cast<uint64_t>(dir); c->x[3] = reinterpret_cast<uint64_t>(filt);
        c->x[8] = reinterpret_cast<uint64_t>(res);
        c->v[0].u64[0] = 0; c->v[0].u64[1] = 0; c->v[0].f32[0] = len;
        if (!seh_call(c, fn)) nfault++;
        *c = saved;
        float t, p[3], nrm[3];
        std::memcpy(&t, res, 4); std::memcpy(p, res + 4, 12); std::memcpy(nrm, res + 16, 12);
        const int hit = res[32] ? 1 : 0;
        if (hit) { nhit++; std::fprintf(out, "1 %g %g %g %g %g %g %g\n", t, p[0], p[1], p[2], nrm[0], nrm[1], nrm[2]); }
        else std::fprintf(out, "0 0 0 0 0 0 0 0\n");
        n++;
    }
    std::fclose(in);
    std::fclose(out);
    NXLOG_I("tt", "rays: %zu cast, %zu hit, %zu faulted (%s -> %s)", n, nhit, nfault, in_path.c_str(), out_path.c_str());
    return true;
}


// ---- @probe: would the player's own collision box collide with the static world at these positions? (S26) ----------------------------------------
// NCollisionMP1::DetectStaticCollisionBoolean(mgr, rc_ptr<CCollidableObject>, CTransform4f, filter) with the player's box (CPlayerMP1::GetCollidableBox), the
// transform a translation of the position (the player's origin is at its feet). in: lines `x y z`; out: one `0` (free) or `1` (collides) per line.
bool probe_positions(::Ctx* c, const std::string& in_path, const std::string& out_path, std::string* err) {
    const uint64_t mgr = g_layout.state_manager ? g_layout.state_manager : g_mgr.load();
    const uint64_t player = g_layout.player ? g_layout.player : g_player.load();
    if (!c || !mgr || !player) { *err = "no state manager or player (not in a level)"; return false; }
    FILE* in = std::fopen(in_path.c_str(), "rb");
    if (!in) { *err = "cannot read " + in_path; return false; }
    FILE* out = std::fopen(out_path.c_str(), "wb");
    if (!out) { std::fclose(in); *err = "cannot write " + out_path; return false; }
    const uint64_t base = game_base();
    alignas(16) uint8_t filt[512] = {};
    alignas(16) uint64_t rc[2] = {0, 0};
    {
        ::Ctx saved = *c;
        c->x[0] = reinterpret_cast<uint64_t>(filt); c->x[1] = base + kSkPassEverything;
        dispatch_call(c, base + kFnMakeIncludeExcludeFilter);
        *c = saved;
        c->x[0] = player; c->x[8] = reinterpret_cast<uint64_t>(rc);
        const bool ok = seh_call(c, base + kFnGetCollidableBox);
        *c = saved;
        if (!ok || !rc[0]) {            // not fatal: every position reports `1` (collides) and the log says why, so a script can retry after the next frame
            size_t m = 0; float a, b, d;
            while (std::fscanf(in, "%f %f %f", &a, &b, &d) == 3) { std::fputs("1\n", out); m++; }
            std::fclose(in); std::fclose(out);
            NXLOG_W("tt", "probe: no collision box on the player (player 0x%llx), %zu positions answered `collides`", static_cast<unsigned long long>(player), m);
            return true;
        }
    }
    size_t n = 0, hit = 0;
    float x, y, z;
    while (std::fscanf(in, "%f %f %f", &x, &y, &z) == 3) {
        alignas(16) float xf[16] = {1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z, 0, 0, 0, 1};
        ::Ctx saved = *c;
        c->x[0] = mgr; c->x[1] = reinterpret_cast<uint64_t>(rc); c->x[2] = reinterpret_cast<uint64_t>(xf); c->x[3] = reinterpret_cast<uint64_t>(filt);
        const bool ok = seh_call(c, base + kFnDetectStaticCollisionBoolean);
        const int r = ok ? static_cast<int>(c->x[0] & 1) : 1;
        *c = saved;
        std::fprintf(out, "%d\n", r);
        n++; hit += r;
    }
    std::fclose(in);
    std::fclose(out);
    NXLOG_I("tt", "probe: %zu positions, %zu collide (%s -> %s)", n, hit, in_path.c_str(), out_path.c_str());
    return true;
}

// ---- @objs: list the live objects of a class by scanning memory for its vtable pointer (S25) ---------------------------------------------------
// Every guest object starts with its vtable pointer (main + vtable symbol + 16). The actor transform is read through the same chain the player probe uses:
// [[[obj + 8] + 0x20] + 0x40] = 3x4 matrix. Used to find doors, docks, pickups and save stations so a script knows where to walk.
bool dump_objects(const std::string& classes, const std::string& out_path, std::string* err) {
    struct Cls { const char* name; uint64_t vtable; };
    static const Cls kCls[] = {{"door", 0x20e9fc0}, {"dock", 0x20e9df8}, {"pickup", 0x20f8300}, {"special", 0x20efdb0}, {"actor", 0x20ed470},
                               {"platform", 0x20edda8}, {"trigger", 0x20ea188}, {"relay", 0x2105700}};
    const uint64_t base = game_base();
    std::vector<std::pair<std::string, uint64_t>> want;
    // "*" = every class listed in the file NXRT_TEST_VTABLES (lines `name offset`, offset = vtable symbol offset in the main module); S26
    if (classes.find('*') != std::string::npos) {
        const char* vf = std::getenv("NXRT_TEST_VTABLES");
        FILE* in = vf ? std::fopen(vf, "rb") : nullptr;
        if (!in) { *err = "'*' needs NXRT_TEST_VTABLES (file of `name offset` lines)"; return false; }
        char nm[256]; unsigned long long off;
        while (std::fscanf(in, "%255s %llx", nm, &off) == 2) want.push_back({nm, base + off + 16});
        std::fclose(in);
    }
    size_t pos = 0;
    if (!want.empty()) pos = classes.size();
    while (pos < classes.size()) {
        while (pos < classes.size() && std::isspace(static_cast<unsigned char>(classes[pos]))) pos++;
        size_t e = pos;
        while (e < classes.size() && !std::isspace(static_cast<unsigned char>(classes[e]))) e++;
        if (e > pos) {
            const std::string tok = classes.substr(pos, e - pos);
            bool found = false;
            for (const Cls& c : kCls) if (tok == c.name) { want.push_back({tok, base + c.vtable + 16}); found = true; }
            if (!found && tok == "player") {
                const uint64_t pl = g_layout.player ? g_layout.player : g_player.load();
                if (pl) { want.push_back({tok, rd64(pl)}); found = true; NXLOG_I("tt", "objs: player object 0x%llx vptr 0x%llx (+0x%llx)", static_cast<unsigned long long>(pl), static_cast<unsigned long long>(rd64(pl)), static_cast<unsigned long long>(rd64(pl) - base)); }
            }
            if (!found) {
                char* end = nullptr;
                const uint64_t off = std::strtoull(tok.c_str(), &end, 0);
                if (end && !*end) { want.push_back({tok, base + off + 16}); found = true; }
            }
            if (!found) { *err = "unknown class '" + tok + "'"; return false; }
        }
        pos = e;
    }
    if (want.empty()) { *err = "no class given"; return false; }
    FILE* f = std::fopen(out_path.c_str(), "wb");
    if (!f) { *err = "cannot write " + out_path; return false; }
    std::unordered_set<uint64_t> want_set;
    std::unordered_map<uint64_t, std::string> want_name;
    if (want.size() > 8) for (const auto& w : want) { want_set.insert(w.second); want_name[w.second] = w.first; }
    uint64_t want_lo = ~0ull, want_hi = 0;           // every wanted pointer lies in a narrow range: rejecting the rest first makes the scan an order of magnitude faster
    for (const auto& w : want) { want_lo = std::min(want_lo, w.second); want_hi = std::max(want_hi, w.second); }
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uint8_t* a = static_cast<uint8_t*>(si.lpMinimumApplicationAddress);
    size_t hits = 0;
    uint64_t scanned = 0;
    std::vector<uint8_t> buf;
    while (a < static_cast<uint8_t*>(si.lpMaximumApplicationAddress)) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(a, &mbi, sizeof mbi)) break;
        uint8_t* next = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY));
        if (readable && mbi.RegionSize >= 64) {
            scanned += mbi.RegionSize;
            for (size_t off = 0; off < mbi.RegionSize; off += (64u << 20)) {
                const size_t n = std::min<size_t>(64u << 20, mbi.RegionSize - off);
                buf.resize(n);
                SIZE_T got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), static_cast<uint8_t*>(mbi.BaseAddress) + off, buf.data(), n, &got) || got < 8) continue;
                for (size_t i = 0; i + 8 <= got; i += 8) {
                    uint64_t v;
                    std::memcpy(&v, &buf[i], 8);
                    if (v < want_lo || v > want_hi) continue;
                    if (!want_set.empty() && !want_set.count(v)) continue;
                    for (size_t wi = 0; wi < (want_set.empty() ? want.size() : 1); wi++) {
                        const std::pair<std::string, uint64_t> w = want_set.empty() ? want[wi] : std::pair<std::string, uint64_t>{want_name[v], v};
                        if (v != w.second) continue;
                        const uint64_t obj = reinterpret_cast<uint64_t>(mbi.BaseAddress) + off + i;
                        const uint64_t xf = rd64(rd64(rd64(obj + 8) + 0x20) + 0x40);
                        float m[12];
                        if (xf && rd(xf, &m) && std::isfinite(m[3]) && std::isfinite(m[7]) && std::isfinite(m[11])) {
                            std::fprintf(f, "%s %.2f %.2f %.2f 0x%llx", w.first.c_str(), m[3], m[7], m[11], static_cast<unsigned long long>(obj));
                            if (w.first == "dock" || w.first == "CScriptDockMP1") {       // S26: the connected area (obj + 980) and the dock plane (three points at obj + 1000)
                                uint8_t gd[16]; float pl[9];
                                if (rd(obj + 980, &gd) && rd(obj + 1000, &pl))
                                    std::fprintf(f, " area=%s plane=%.2f,%.2f,%.2f;%.2f,%.2f,%.2f;%.2f,%.2f,%.2f", tt_guid_to_text(gd).c_str(), pl[0], pl[1], pl[2], pl[3], pl[4], pl[5], pl[6], pl[7], pl[8]);
                            }
                            std::fprintf(f, "\n");
                        } else
                            std::fprintf(f, "%s - - - 0x%llx\n", w.first.c_str(), static_cast<unsigned long long>(obj));
                        hits++;
                    }
                }
            }
        }
        if (next <= a) break;
        a = next;
    }
    std::fclose(f);
    NXLOG_I("tt", "objs: %zu objects of '%s' written to %s (scanned %llu MB, game base 0x%llx, wanted vptrs +0x%llx...)", hits, classes.c_str(), out_path.c_str(), static_cast<unsigned long long>(scanned >> 20), static_cast<unsigned long long>(base), static_cast<unsigned long long>(want[0].second - base));
    return true;
}
}

namespace {
void on_frame(uint64_t frame) { detail::watch_flush(frame); }
void reset() { set_probe_layout(ProbeLayout{}); }
// The keys of the fields read_state adds that @assert and @mashuntil may name.
const char* const kStateKeys[] = {"igt", "health", "px", "py", "pz", "vx", "vy", "vz", "speed", "state", "spd", "pitch", "cam", "morph", "orbit", "camyaw", "campitch", "beam", "visor", "msl", nullptr};
const IdKeyDef kIdKeys[] = {{"world", false}, {"area", true}, {nullptr, false}};
}

const GameProbes& mpr_game_probes() {
    static const GameProbes p = [] {
        GameProbes t;
        t.install_hooks = &install_game_hooks;
        t.read_state = &read_state;
        t.on_frame = &on_frame;
        t.reset = &reset;
        t.state_keys = kStateKeys;
        t.id_keys = kIdKeys;
        t.steps = mpr_game_steps();
        t.step_count = mpr_game_step_count();
        return t;
    }();
    return p;
}

}
