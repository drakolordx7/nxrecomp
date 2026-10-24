// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: free lock-on and camera feel options, see play_lockcam.h.
#include "play_lockcam.h"

#include <atomic>

#include "nxrt/abi.h"
#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"

namespace nxrt::play {

namespace {

// main-module relative addresses (Metroid Prime Remastered 1.0.0); the hook sites are base + these
constexpr uint64_t kFnUpdateOrbitOrientation = 0xc77854;   // CPlayerMP1::UpdateOrbitOrientation(float, CStateManager&): x0 the player; turns the body to face the orbit target
constexpr uint64_t kFnComputeFreeLook = 0xc71638;          // CPlayerMP1::ComputeFreeLook(CFinalInput const&, CStateManager const&): x0 the player; called from ProcessInput
constexpr uint64_t kFnTurnInput = 0xcefa54;                // CPlayerMP1::TurnInput(CStateManager const&, CFinalInput const&) const: x0 the player; returns s0
constexpr uint64_t kTurnInputFromComputeMovement = 0xcefba0;   // the return address of its call inside CPlayerMP1::ComputeMovement (the other callers are footsteps and the dash)
constexpr uint64_t kFnValidateCurrentOrbitTarget = 0xc765d0;   // CPlayerMP1::ValidateCurrentOrbitTargetId(CStateManager&): x0 the player; returns 0 = still valid, else the reason the lock breaks (5 = the view turned too far from it)
constexpr uint64_t kFnFpCamUpdateTransform = 0xd710ac;     // CFirstPersonCameraMP1::UpdateTransform(CStateManager&, float): x0 the camera; with a lock it turns its own view to the target
constexpr uint64_t kFnUpdateViewWander = 0xd0ada0;         // CPlayerCameraBobMP1::UpdateViewWander(float, CStateManager&, CEntityMP1&): x0 the bob object
constexpr uint64_t kFnCalcMovingTranslation = 0xd0b17c;    // CPlayerCameraBobMP1::CalculateMovingTranslation(float&, float&) const: x0 the bob object, x1 and x2 the outputs

constexpr const char* kTag = "lockcam";
constexpr uint32_t kOrbitState = 0x4e0;      // CPlayerMP1: EPlayerOrbitState
constexpr uint32_t kWanderTransform = 212;   // CPlayerCameraBobMP1: the view wander transform (3x4 floats, row major), composed into the bob transform each Update

struct Hooks {
    uint64_t base = 0;
    AimDriver* aim = nullptr;
    uint32_t ids[7] = {};
    std::atomic<uint64_t> player{0};      // the player object, learnt from the hooks that get it in x0
    int count = 0;
    std::atomic<bool> idle_sway{true}, walk_bob{true};
    std::atomic<uint64_t> orientation_skipped{0}, freelook_calls{0}, turn_calls{0}, sway_skipped{0}, bob_zeroed{0};
};
Hooks g_h;

thread_local bool t_in_freelook = false, t_in_turn = false, t_in_camxf = false, t_in_validate = false;

// Runs the game's own function with the player's orbit state read as 0 (no lock): the free look / turn code then behaves as it does without a lock.
// The state is put back afterwards. Only used inside functions that read the state and nothing else of the lock.
void run_as_unlocked(::Ctx* c, uint64_t addr, uint64_t player) {
    uint32_t state = 0;
    const uint32_t zero = 0;
    const bool swapped = guest_read(player + kOrbitState, &state, sizeof state) && state != 0 && guest_write(player + kOrbitState, &zero, sizeof zero);
    dispatch_call(c, addr);
    if (swapped) guest_write(player + kOrbitState, &state, sizeof state);
}

int orientation_hook(::Ctx* c, uint64_t, void*) {
    g_h.player.store(c->x[0], std::memory_order_relaxed);
    if (!g_h.aim || !g_h.aim->free_lock_active(c->x[0])) return 0;
    if (g_h.orientation_skipped++ == 0) NXLOG_I(kTag, "free lock-on: the game's turn to the target is skipped (first time)");
    return 1;      // the body keeps the heading the mouse gave it; x0 (the player) is the return value, untouched
}

int freelook_hook(::Ctx* c, uint64_t addr, void*) {
    if (t_in_freelook || !g_h.aim) return 0;
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

int wander_hook(::Ctx* c, uint64_t, void*) {
    if (g_h.idle_sway.load(std::memory_order_relaxed)) return 0;
    // Skip the wander update and keep its transform at identity, so the bob transform is the walking bob alone.
    static const float kIdentity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    guest_write(c->x[0] + kWanderTransform, kIdentity, sizeof kIdentity);
    if (g_h.sway_skipped++ == 0) NXLOG_I(kTag, "idle sway off: the view wander is skipped (first time)");
    return 1;
}

int moving_translation_hook(::Ctx* c, uint64_t, void*) {
    if (g_h.walk_bob.load(std::memory_order_relaxed)) return 0;
    const float zero = 0.f;
    guest_write(c->x[1], &zero, sizeof zero);
    guest_write(c->x[2], &zero, sizeof zero);
    if (g_h.bob_zeroed++ == 0) NXLOG_I(kTag, "walk bob off: the walking bob is zeroed (first time)");
    return 1;
}

}  // namespace

void lockcam_install(uint64_t game_base, AimDriver* aim) {
    lockcam_remove();
    g_h.base = game_base;
    g_h.aim = aim;
    g_h.ids[g_h.count++] = hook_add_entry(game_base + kFnUpdateOrbitOrientation, &orientation_hook);
    g_h.ids[g_h.count++] = hook_add_entry(game_base + kFnComputeFreeLook, &freelook_hook);
    g_h.ids[g_h.count++] = hook_add_entry(game_base + kFnTurnInput, &turn_hook);
    g_h.ids[g_h.count++] = hook_add_entry(game_base + kFnValidateCurrentOrbitTarget, &validate_hook);
    g_h.ids[g_h.count++] = hook_add_entry(game_base + kFnFpCamUpdateTransform, &camera_transform_hook);
    g_h.ids[g_h.count++] = hook_add_entry(game_base + kFnUpdateViewWander, &wander_hook);
    g_h.ids[g_h.count++] = hook_add_entry(game_base + kFnCalcMovingTranslation, &moving_translation_hook);
}

void lockcam_remove() {
    for (int i = 0; i < g_h.count; ++i)
        if (g_h.ids[i]) hook_remove(g_h.ids[i]);
    g_h.count = 0;
    g_h.aim = nullptr;
}

void lockcam_set(const Settings& s) {
    g_h.idle_sway.store(s.camera.idle_sway, std::memory_order_relaxed);
    g_h.walk_bob.store(s.camera.walk_bob, std::memory_order_relaxed);
    if (g_h.aim) g_h.aim->set_free_lock(s.aim.lock_mode == LockMode::Free);
}

LockCamStats lockcam_stats() {
    LockCamStats st;
    st.orientation_skipped = g_h.orientation_skipped.load();
    st.freelook_calls = g_h.freelook_calls.load();
    st.turn_calls = g_h.turn_calls.load();
    st.sway_skipped = g_h.sway_skipped.load();
    st.bob_zeroed = g_h.bob_zeroed.load();
    return st;
}

}  // namespace nxrt::play
