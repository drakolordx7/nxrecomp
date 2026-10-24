// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: free lock-on and the decoupled gun, see play_lockcam.h.
#include "play_lockcam.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "nxrt/abi.h"
#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "play_display.h"

namespace nxrt::play {

namespace {

// main-module relative addresses (Metroid Prime Remastered 1.0.0); the hook sites are base + these
constexpr uint64_t kFnUpdateOrbitOrientation = 0xc77854;   // CPlayerMP1::UpdateOrbitOrientation(float, CStateManager&): x0 the player, x1 the manager; turns the body to face the orbit target
constexpr uint64_t kFnComputeFreeLook = 0xc71638;          // CPlayerMP1::ComputeFreeLook(CFinalInput const&, CStateManager const&): x0 the player; called from ProcessInput
constexpr uint64_t kFnTurnInput = 0xcefa54;                // CPlayerMP1::TurnInput(CStateManager const&, CFinalInput const&) const: x0 the player; returns s0
constexpr uint64_t kTurnInputFromComputeMovement = 0xcefba0;   // the return address of its call inside CPlayerMP1::ComputeMovement (the other callers are footsteps and the dash)
constexpr uint64_t kFnValidateCurrentOrbitTarget = 0xc765d0;   // CPlayerMP1::ValidateCurrentOrbitTargetId(CStateManager&): x0 the player; returns 0 = still valid, else the reason the lock breaks (5 = the view turned too far from it)
constexpr uint64_t kFnFpCamUpdateTransform = 0xd710ac;     // CFirstPersonCameraMP1::UpdateTransform(CStateManager&, float): x0 the camera; with a lock it turns its own view to the target
constexpr uint64_t kFnGunUpdate = 0xc93950;                // CPlayerGunMP1::Update(IMP1EntityBaseGOC&, float, float, float, CStateManager&): x0 the gun; builds the gun's world transform (+484, 3x4 floats) from the camera and the cursor, then lets the motion states and the firing use it
constexpr uint64_t kFnSetTransform = 0xc4d588;             // CActorMP1::SetTransform(CTransform4f const&)
constexpr uint64_t kFnUpdateAimTarget = 0xc7ad8c;          // CPlayerMP1::UpdateAimTarget(CStateManager&): x0 the player; picks the object the aiming cursor and the homing shots follow (the locked target when it can be shot)
constexpr uint64_t kFnSetAimTargetId = 0xc687d4;           // CPlayerMP1::SetAimTargetId(CValueVersionId): x0 the player, w1 the id
constexpr uint64_t kFnGetCombatMode = 0xc73564;            // CPlayerMP1::GetCombatMode() const: x0 the player; true while the combat visor's weapons are in use

constexpr uint64_t kFnCursorUpdate = 0xcb6250;             // CAimingCursorMP1::Update(CFinalInput const&, float, CStateManager&): x0 the cursor, x1 the input, x2 the manager
constexpr uint64_t kFnGetCommandValue = 0x45a690;          // NControlCommands::GetDataEnumValueForCommand(ECommand): w0 the command
constexpr uint64_t kFnDigitalHeld = 0x4d7540;              // CCommandFrame::GetDigitalHeldInput(CDataEnumValue const&, CFinalInput const&, EFilterMode) const: x0 the frame, x1 the value, x2 the input
constexpr const char* kTag = "lockcam";
constexpr uint32_t kOrbitState = 0x4e0;      // CPlayerMP1: EPlayerOrbitState
constexpr uint32_t kGunTransform = 484;      // CPlayerGunMP1: the gun's world transform

struct Hooks {
    uint64_t base = 0;
    AimDriver* aim = nullptr;
    uint32_t ids[12] = {};
    std::atomic<uint64_t> player{0};      // the player object, learnt from the hooks that get it in x0
    int count = 0;
    std::atomic<bool> gun_drives_view{true};
    std::atomic<double> margin{0.15};
    std::atomic<uint64_t> orientation_skipped{0}, freelook_calls{0}, turn_calls{0}, cursor_calls{0}, view_clamped{0}, gun_moved{0};
};
Hooks g_h;

thread_local bool t_in_cursor = false, t_in_aimtarget = false, t_in_freelook = false, t_in_turn = false, t_in_camxf = false, t_in_validate = false, t_in_gun = false;

// the gun's share of the camera bob: vanilla camera * steady camera^-1, applied to the gun's world transform after the game made it
uint64_t g_cmd75_value = 0;      // the game's CDataEnumValue for command 75 (a stable object)
uint64_t g_cmd73_value = 0;      // the same for command 73 (see held_input_hook)
std::atomic<bool> g_pitch_accumulates{false};
float g_delta[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
std::atomic<bool> g_delta_on{false};
double g_turn_omega = 0.0;      // the free lock's turn speed (rad/s), main thread
constexpr double kTurnTauUp = 0.0262, kTurnTauDown = 0.0117;      // seconds: speeding up toward the stick, slowing down or reversing (fitted to the game's turn, see orientation_hook)
std::atomic<float> g_turn_input{0.f};      // what TurnInput returned the last time the free lock-on asked it (the right stick's turn)

double wrap_pi(double a) {
    while (a > 3.14159265358979323846) a -= 2.0 * 3.14159265358979323846;
    while (a < -3.14159265358979323846) a += 2.0 * 3.14159265358979323846;
    return a;
}

// Runs the game's own function with the player's orbit state read as 0 (no lock): the free look / turn code then behaves as it does without a lock.
// The state is put back afterwards. Only used inside functions that read the state and nothing else of the lock.
void run_as_unlocked(::Ctx* c, uint64_t addr, uint64_t player) {
    uint32_t state = 0;
    const uint32_t zero = 0;
    const bool swapped = guest_read(player + kOrbitState, &state, sizeof state) && state != 0 && guest_write(player + kOrbitState, &zero, sizeof zero);
    dispatch_call(c, addr);
    if (swapped) guest_write(player + kOrbitState, &state, sizeof state);
}

// The game's own SetTransform on the player, from inside a hook (the registers of the hooked call are kept).
void set_body_transform(::Ctx* c, uint64_t player, const float xf[12]) {
    alignas(16) float copy[12];
    std::memcpy(copy, xf, sizeof copy);
    ::Ctx saved = *c;
    c->x[0] = player;
    c->x[1] = reinterpret_cast<uint64_t>(copy);
    dispatch_call(c, g_h.base + kFnSetTransform);
    *c = saved;
}

// Holds the view so the target stays inside the margin (after the camera, from the post camera hook; the mouse part is in AimDriver::apply).
void clamp_view(::Ctx* c, uint64_t player, uint64_t mgr, double extra_yaw) {
    AimDriver* aim = g_h.aim;
    uint64_t xfp = 0;
    float xf[12];
    double pitch = 0;
    if (!aim->body_transform(player, &xfp, xf) || !aim->view_pitch(player, &pitch)) return;
    const double yaw = yaw_of_transform(xf);
    double y2 = yaw + extra_yaw, p2 = pitch;
    const bool clamped = aim->keep_target_in_view(player, mgr, &y2, &p2);
    if (clamped) g_h.view_clamped++;
    else if (extra_yaw == 0.0) return;
    if (std::fabs(wrap_pi(y2 - yaw)) > 1e-7) {
        float nx[12];
        rotate_transform_yaw(xf, wrap_pi(y2 - yaw), nx);
        set_body_transform(c, player, nx);
    }
    if (std::fabs(p2 - pitch) > 1e-7) {
        const float out = static_cast<float>(p2);
        const AimLayout& l = aim->layout();
        guest_write(player + l.free_look_pitch, &out, sizeof out);
        guest_write(player + l.free_look_target, &out, sizeof out);
        aim->set_pitch(p2);
    }
}

int orientation_hook(::Ctx* c, uint64_t, void*) {
    const uint64_t player = c->x[0];
    const float dt_in = c->v[0].f32[0];
    g_h.player.store(player, std::memory_order_relaxed);
    if (!g_h.aim || !g_h.aim->free_lock_active(player)) {
        g_turn_input.store(0.f, std::memory_order_relaxed);
        g_turn_omega = 0.0;
        return 0;
    }
    if (g_h.orientation_skipped++ == 0) NXLOG_I(kTag, "free lock-on: the game's turn to the target is skipped (first time); a full stick turn is %.3f rad/s", g_h.aim->stick_turn_rate());
    // the right stick turns the view as it does without a lock: the turn the game worked out in TurnInput (called as if unlocked) times the speed of a full turn
    const double dt = std::isfinite(dt_in) && dt_in > 0.f && dt_in < 0.25f ? dt_in : 0.0;
    // The game's own turn ramps: Samus's angular velocity is pushed toward stick * full speed (measured with the unlocked body at 60 steps per second, full right
    // stick from rest: 47, 72, 85, 92, 96 percent of full speed in the first five frames, and 45, 11, 2.6 percent in the three after the stick is let go).
    const double target = static_cast<double>(g_turn_input.load(std::memory_order_relaxed)) * g_h.aim->stick_turn_rate();
    const double omega = dt > 0.0 ? turn_ramp_step(&g_turn_omega, target, dt, kTurnTauUp, kTurnTauDown) : 0.0;
    const double stick_yaw = omega * dt;
    clamp_view(c, player, c->x[1], std::isfinite(stick_yaw) ? stick_yaw : 0.0);
    return 1;      // the body keeps the heading the mouse gave it; x0 (the player) is the return value, untouched
}

int freelook_hook(::Ctx* c, uint64_t addr, void*) {
    if (t_in_freelook || !g_h.aim) return 0;
    lockcam_prime(c);      // the first call finds the game's command values (needs the real game: this hook only runs inside it)
    const uint64_t player = c->x[0];
    g_h.player.store(player, std::memory_order_relaxed);
    if (!g_h.aim->free_lock_active(player)) return 0;
    g_h.freelook_calls++;
    t_in_freelook = true;
    run_as_unlocked(c, addr, player);
    t_in_freelook = false;
    return 1;
}

int turn_hook(::Ctx* c, uint64_t addr, void*) {
    if (t_in_turn || !g_h.aim) return 0;
    if (c->x[30] != g_h.base + kTurnInputFromComputeMovement) return 0;
    const uint64_t player = c->x[0];
    if (!g_h.aim->free_lock_active(player)) return 0;
    g_h.turn_calls++;
    t_in_turn = true;
    run_as_unlocked(c, addr, player);
    t_in_turn = false;
    g_turn_input.store(c->v[0].f32[0], std::memory_order_relaxed);      // the turn input is returned in s0
    return 1;
}

// The lock breaks (return 5) when the player's heading is more than the tweak's angle away from the target, once it has faced it. With the view
// free that is no reason to let go: every other reason (distance, line of sight, the target gone) still breaks it.
int validate_hook(::Ctx* c, uint64_t addr, void*) {
    if (t_in_validate || !g_h.aim) return 0;
    const uint64_t player = c->x[0];
    if (!g_h.aim->free_lock_active(player)) return 0;
    t_in_validate = true;
    dispatch_call(c, addr);
    t_in_validate = false;
    if (c->x[0] == 5) c->x[0] = 0;
    return 1;
}

int camera_transform_hook(::Ctx* c, uint64_t addr, void*) {
    if (t_in_camxf || !g_h.aim) return 0;
    const uint64_t player = g_h.player.load(std::memory_order_relaxed);
    if (!player || !g_h.aim->free_lock_active(player)) return 0;
    t_in_camxf = true;
    run_as_unlocked(c, addr, player);
    t_in_camxf = false;
    return 1;
}

// The aiming cursor (and so the arm cannon) follows the locked target only while the game has it as the aim target, and it does that for things that can be
// shot. Without it, free lock-on would leave the cursor in the middle of the screen, off the target. So the locked target is the aim target too for the length
// of the lock (the lock-on to the lights of a force field, say, which are not shootable otherwise).
int aim_target_hook(::Ctx* c, uint64_t addr, void*) {
    if (t_in_aimtarget || !g_h.aim) return 0;
    const uint64_t player = c->x[0];
    if (!g_h.aim->free_lock_active(player)) return 0;
    const ::Ctx saved = *c;
    t_in_aimtarget = true;
    dispatch_call(c, addr);
    uint32_t aim_id = 0, orbit_id = 0;
    guest_read(player + 0x5e0, &aim_id, sizeof aim_id);
    guest_read(player + 0x4ec, &orbit_id, sizeof orbit_id);
    if (aim_id == 0xffffffffu && orbit_id != 0xffffffffu && orbit_id != 0) {
        ::Ctx s2 = *c;
        c->x[0] = player;
        dispatch_call(c, g_h.base + kFnGetCombatMode);
        const bool combat = (c->x[0] & 1) != 0;
        *c = s2;
        if (combat) {
            c->x[0] = player;
            c->x[1] = orbit_id;
            dispatch_call(c, g_h.base + kFnSetAimTargetId);
        }
    }
    t_in_aimtarget = false;
    c->x[0] = saved.x[0];
    return 1;
}

// The aiming cursor eases toward the locked target's place on the screen only while the game's command 75 is held (nothing the pad sets in the normal lock,
// where the view centres the target, so the cursor stays in the middle and is on it). With the view free, the cursor has to go to the target: for the
// length of the cursor's own update the game is told that command 75 is held.
int cursor_update_hook(::Ctx* c, uint64_t addr, void*) {
    if (t_in_cursor || !g_h.aim) return 0;
    g_h.cursor_calls++;
    const uint64_t player = g_h.player.load(std::memory_order_relaxed);
    if (!player || !g_h.aim->free_lock_active(player)) return 0;
    if (!g_cmd75_value) {
        const ::Ctx saved = *c;
        c->x[0] = 75;
        dispatch_call(c, g_h.base + kFnGetCommandValue);
        g_cmd75_value = c->x[0];
        *c = saved;
    }
    t_in_cursor = true;
    dispatch_call(c, addr);
    t_in_cursor = false;
    return 1;
}

thread_local bool t_in_held = false;
int held_input_hook(::Ctx* c, uint64_t addr, void*) {
    if (g_cmd73_value && c->x[1] == g_cmd73_value && !t_in_held) {
        // command 73 is the game's switch for the stick's pitch scheme: held = the stick adds to the pitch target every frame (dual stick), not held = the
        // stick sets the target angle. Watched here, so play.cpp knows whether the stick's pitch needs the frame time put in (the right stick is scaled by it).
        t_in_held = true;
        dispatch_call(c, addr);
        t_in_held = false;
        g_pitch_accumulates.store((c->x[0] & 1) != 0, std::memory_order_relaxed);
        return 1;
    }
    if (!t_in_cursor || !g_cmd75_value || c->x[1] != g_cmd75_value) return 0;
    c->x[0] = 1;
    return 1;
}

int gun_update_hook(::Ctx* c, uint64_t addr, void*) {
    if (t_in_gun || !g_delta_on.load(std::memory_order_relaxed)) return 0;
    const uint64_t gun = c->x[0];
    t_in_gun = true;
    dispatch_call(c, addr);
    t_in_gun = false;
    float cur[12], out[12];
    if (guest_read(gun + kGunTransform, cur, sizeof cur)) {
        xf_mul(g_delta, cur, out);
        guest_write(gun + kGunTransform, out, sizeof out);
        g_h.gun_moved++;
    }
    return 1;
}

}  // namespace

double turn_ramp_step(double* omega, double target, double dt, double tau_up, double tau_down) {
    if (!(dt > 0.0) || !std::isfinite(dt) || !std::isfinite(target)) return *omega;
    const bool speeding_up = (target * *omega > 0.0 && std::fabs(target) > std::fabs(*omega)) || *omega == 0.0;
    const double tau = std::max(1e-4, speeding_up ? tau_up : tau_down);
    *omega += (target - *omega) * (1.0 - std::exp(-dt / tau));
    if (std::fabs(*omega) < 1e-5 && target == 0.0) *omega = 0.0;
    return *omega;
}

void xf_mul(const float a[12], const float b[12], float out[12]) {
    float r[12];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) r[4 * i + j] = a[4 * i] * b[j] + a[4 * i + 1] * b[4 + j] + a[4 * i + 2] * b[8 + j];
        r[4 * i + 3] = a[4 * i] * b[3] + a[4 * i + 1] * b[7] + a[4 * i + 2] * b[11] + a[4 * i + 3];
    }
    std::memcpy(out, r, sizeof r);
}

void xf_rel(const float a[12], const float b[12], float out[12]) {
    float r[12];
    // rotation: Ra * Rb^T, entry (i, j) = row i of a dotted with row j of b
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[4 * i + j] = a[4 * i] * b[4 * j] + a[4 * i + 1] * b[4 * j + 1] + a[4 * i + 2] * b[4 * j + 2];
    // translation: ta - (Ra * Rb^T) * tb
    for (int i = 0; i < 3; ++i) r[4 * i + 3] = a[4 * i + 3] - (r[4 * i] * b[3] + r[4 * i + 1] * b[7] + r[4 * i + 2] * b[11]);
    std::memcpy(out, r, sizeof r);
}

void lockcam_install(uint64_t game_base, AimDriver* aim) {
    lockcam_remove();
    g_h.base = game_base;
    g_h.aim = aim;
    g_h.ids[g_h.count++] = hook_add_entry(game_base + kFnUpdateOrbitOrientation, &orientation_hook);
    g_h.ids[g_h.count++] = hook_add_entry(game_base + kFnComputeFreeLook, &freelook_hook);
    g_h.ids[g_h.count++] = hook_add_entry(game_base + kFnTurnInput, &turn_hook);
    g_h.ids[g_h.count++] = hook_add_entry(game_base + kFnValidateCurrentOrbitTarget, &validate_hook);
    g_h.ids[g_h.count++] = hook_add_entry(game_base + kFnFpCamUpdateTransform, &camera_transform_hook);
    g_h.ids[g_h.count++] = hook_add_entry(game_base + kFnGunUpdate, &gun_update_hook);
    g_h.ids[g_h.count++] = hook_add_entry(game_base + kFnUpdateAimTarget, &aim_target_hook);
    g_h.ids[g_h.count++] = hook_add_entry(game_base + kFnCursorUpdate, &cursor_update_hook);
    g_h.ids[g_h.count++] = hook_add_entry(game_base + kFnDigitalHeld, &held_input_hook);
}

void lockcam_remove() {
    for (int i = 0; i < g_h.count; ++i)
        if (g_h.ids[i]) hook_remove(g_h.ids[i]);
    g_h.count = 0;
    g_h.aim = nullptr;
    g_delta_on.store(false);
}

void lockcam_set(const Settings& s) {
    g_h.gun_drives_view.store(s.camera.gun_drives_view, std::memory_order_relaxed);
    g_h.margin.store(s.aim.free_lock_margin, std::memory_order_relaxed);
    if (s.camera.gun_drives_view) lockcam_clear_gun_delta();
    if (g_h.aim) g_h.aim->set_free_lock(s.aim.lock_mode == LockMode::Free);
    lockcam_frame();
}

void lockcam_frame() {
    if (!g_h.aim) return;
    static const bool diag = std::getenv("NXRT_LOCKCAM_DIAG") != nullptr;
    static int frames = 0;
    if (diag && ++frames % 300 == 0) {
        float dev = 0;
        for (int i = 0; i < 12; ++i) dev = std::max(dev, std::fabs(g_delta[i] - ((i % 5 == 0 && i < 11) ? 1.f : 0.f)));
        NXLOG_I(kTag, "diag: orientation_skipped %llu freelook %llu turn %llu cursor %llu clamped %llu gun_moved %llu, gun delta on %d max |D-I| %.5f",
                static_cast<unsigned long long>(g_h.orientation_skipped.load()), static_cast<unsigned long long>(g_h.freelook_calls.load()), static_cast<unsigned long long>(g_h.turn_calls.load()),
                static_cast<unsigned long long>(g_h.cursor_calls.load()), static_cast<unsigned long long>(g_h.view_clamped.load()), static_cast<unsigned long long>(g_h.gun_moved.load()),
                g_delta_on.load() ? 1 : 0, dev);
    }
    const DisplayLive dl = display_live();
    const double aspect = dl.aspect > 0 ? dl.aspect : static_cast<double>(dl.render_w) / std::max(1, dl.render_h);
    g_h.aim->set_free_lock_view(g_h.margin.load(std::memory_order_relaxed), aspect, dl.fov_offset);
}

void lockcam_prime(::Ctx* c) {
    if (g_cmd73_value || !g_h.base) return;
    const ::Ctx saved = *c;
    c->x[0] = 73;
    dispatch_call(c, g_h.base + kFnGetCommandValue);
    g_cmd73_value = c->x[0];
    *c = saved;
}

bool lockcam_pitch_accumulates() { return g_pitch_accumulates.load(std::memory_order_relaxed); }

bool lockcam_gun_decoupled() { return !g_h.gun_drives_view.load(std::memory_order_relaxed); }

void lockcam_set_gun_delta(const float vanilla[12], const float steady[12]) {
    float d[12];
    xf_rel(vanilla, steady, d);
    for (float f : d)
        if (!std::isfinite(f)) {
            lockcam_clear_gun_delta();
            return;
        }
    std::memcpy(g_delta, d, sizeof d);
    g_delta_on.store(true, std::memory_order_relaxed);
}

void lockcam_clear_gun_delta() { g_delta_on.store(false, std::memory_order_relaxed); }

LockCamStats lockcam_stats() {
    LockCamStats st;
    st.orientation_skipped = g_h.orientation_skipped.load();
    st.freelook_calls = g_h.freelook_calls.load();
    st.turn_calls = g_h.turn_calls.load();
    st.cursor_calls = g_h.cursor_calls.load();
    st.view_clamped = g_h.view_clamped.load();
    st.gun_moved = g_h.gun_moved.load();
    return st;
}

}  // namespace nxrt::play
