// SPDX-License-Identifier: GPL-3.0-or-later
// The jump timer's last frame (S50, games/mpr/play_jump.cpp): the force rule, and a jump stepped through the game's integration rule at many step patterns.
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "mpr/mpr_sites.h"
#include "mpr/play_jump.h"
#include "nxrt/config.h"
#include "nxrt/hooks.h"

NXTEST_GROUP(play_jump)

namespace {
namespace play = nxrt::play;

#define CHECK_NEAR(got, want, eps)                                                                                  \
    do {                                                                                                            \
        const double nx_g = static_cast<double>(got), nx_w = static_cast<double>(want);                             \
        if (!(std::fabs(nx_g - nx_w) <= (eps)))                                                                     \
            ::nxtest::fail(__FILE__, __LINE__, #got " ~ " #want,                                                    \
                           "got " + std::to_string(nx_g) + ", want " + std::to_string(nx_w));                       \
    } while (0)

// The game's numbers for a standing jump (S49 trace): jump 50 u/s^2, gravity -35 u/s^2, rise cap 11.667 u/s, jump timer 0.25 s.
constexpr float kAJump = 50.f, kAGrav = -35.f, kVmax = 11.667f, kTimer = 0.25f;
constexpr float kMass = 200.f;   // any mass: the rule works in force units and divides it out again

// The vertical speed at the end of the step for a force: what the game adds on top of the capped entering speed.
double v_end(float v, float force, float dt, float mass) { return std::fmin(v, kVmax) + static_cast<double>(force) / mass * dt; }

// A standing jump stepped the way the S49 trace shows the game does it (fits every run to 1e-4): the speed entering a step is capped, the player
// moves with it, then the step's force is added. JumpInput: the jump force while the timer lasts, the blend in the step where it ends, then gravity.
// The timer is a float that CPlayerMP1::Think counts down by the float step, as the game does (0.25 - 15 * float(1/60) leaves 1.1e-8 s).
double apex(const std::vector<double>& steps, bool fix) {
    double v = 0, y = 0, top = 0;
    float timer = kTimer;
    bool rising = true;
    for (size_t i = 0; i < 100000 && (i < 4 || y > 0); i++) {
        const float dt = static_cast<float>(steps[i % steps.size()]);
        const double vc = std::fmin(v, static_cast<double>(kVmax));
        y += vc * dt;
        double a = kAGrav;
        if (rising) {
            if (timer >= dt) {
                a = kAJump;
            } else {
                const float f = timer / dt;
                const float jp = f * (kAJump * kMass), gp = ((1.f - f) * kAGrav) * kMass;
                const float force = fix ? play::jump_part_capped(jp, static_cast<float>(v), kVmax, dt, kMass) + gp : jp + gp;
                a = static_cast<double>(force) / kMass;
                rising = false;
            }
            timer -= dt;
        }
        v = vc + a * dt;
        top = std::max(top, y);
    }
    return top;
}
}  // namespace

NXTEST(below_the_cap_the_force_is_unchanged_bit_for_bit) {
    // Rising slowly: the jump part keeps the speed under the cap, so the game's own value comes back unchanged.
    const float jp = 0.6f * kAJump * kMass;
    CHECK_EQ(play::jump_part_capped(jp, 5.f, kVmax, 1.f / 60.f, kMass), jp);
    CHECK_EQ(play::jump_part_capped(jp, 11.0f, kVmax, 1.f / 144.f, kMass), jp);   // 11 + 0.6 * 50 / 144 = 11.21 < 11.667
    CHECK_EQ(play::jump_part_capped(0.f, kVmax, kVmax, 1.f / 60.f, kMass), 0.f);
}

NXTEST(at_the_cap_only_the_gravity_part_acts) {
    // Already at the cap: the jump part has no room left, the speed ends at vmax + a_gravity * (1 - f) * dt.
    for (float dt : {1.f / 240.f, 1.f / 144.f, 1.f / 60.f, 0.0161f, 1.f / 30.f}) {
        for (float f : {0.05f, 0.4f, 0.6f, 0.95f}) {
            const float timer = f * dt;
            const float force = play::jump_end_force(kVmax, kVmax, kAJump, kAGrav, timer, dt, kMass);
            const double want = kVmax + static_cast<double>(kAGrav) * (1.0 - static_cast<double>(timer / dt)) * dt;
            CHECK_NEAR(v_end(kVmax, force, dt, kMass), want, 1e-4);
        }
    }
}

NXTEST(above_the_cap_counts_as_the_cap) {
    // The speed may enter above the cap (the previous step added the full jump force on top of it); the cap takes it back first.
    const float dt = 0.01612f, timer = 0.393f * dt;
    const float force = play::jump_end_force(11.9804f, kVmax, kAJump, kAGrav, timer, dt, kMass);
    CHECK_NEAR(v_end(11.9804f, force, dt, kMass), kVmax + kAGrav * (1.0 - 0.393) * dt, 1e-4);   // 11.325
    // The game alone: 11.667 + (0.393 * 50 - 0.607 * 35) * 0.01612 = 11.641, the S49 run b2_c144_1.
    const float f = timer / dt;
    const float game = f * (kAJump * kMass) + ((1.f - f) * kAGrav) * kMass;
    CHECK_NEAR(v_end(11.9804f, game, dt, kMass), 11.641, 1e-3);
}

NXTEST(crossing_the_cap_inside_the_step) {
    // 11.5 + 50 * 0.8 / 60 = 12.17: the jump part reaches the cap partway, the rest of it is lost, then gravity for the (1 - f) part.
    const float dt = 1.f / 60.f, timer = 0.8f * dt;
    const float force = play::jump_end_force(11.5f, kVmax, kAJump, kAGrav, timer, dt, kMass);
    CHECK_NEAR(v_end(11.5f, force, dt, kMass), kVmax + kAGrav * 0.2 * dt, 1e-4);
    // The 14.0 cap of item 15 leaves room for all of it: unchanged.
    const float f = timer / dt;
    const float game = f * (kAJump * kMass) + ((1.f - f) * kAGrav) * kMass;
    CHECK_EQ(play::jump_end_force(11.5f, 14.f, kAJump, kAGrav, timer, dt, kMass), game);
}

NXTEST(horizontal_scale_is_the_caps_own) {
    // f = 1 (the jump part is the whole jump force): the factor the next frame's cap applies, 11.667 / 12.5 at 1/60.
    const float dt = 1.f / 60.f;
    CHECK_NEAR(play::jump_cap_scale(kAJump * kMass, kVmax, kVmax, dt, kMass), kVmax / (kVmax + kAJump * dt), 1e-6);
    CHECK_NEAR(play::jump_cap_scale(kAJump * kMass, kVmax, kVmax, dt, kMass), 11.667 / 12.5, 1e-4);
    // part of the step: only that part acts against the cap
    CHECK_NEAR(play::jump_cap_scale(0.4f * kAJump * kMass, kVmax, kVmax, dt, kMass), kVmax / (kVmax + 0.4 * kAJump * dt), 1e-6);
    // entering above the cap counts as the cap; below the cap with room left: 1
    CHECK_NEAR(play::jump_cap_scale(0.4f * kAJump * kMass, 12.3f, kVmax, dt, kMass), kVmax / (kVmax + 0.4 * kAJump * dt), 1e-6);
    CHECK_EQ(play::jump_cap_scale(0.4f * kAJump * kMass, 10.f, kVmax, dt, kMass), 1.f);
    CHECK_EQ(play::jump_cap_scale(1.f, kVmax, kVmax, 0.f, kMass), 1.f);
}

NXTEST(unusable_inputs_leave_the_game_value) {
    const float jp = 123.f;
    CHECK_EQ(play::jump_part_capped(jp, kVmax, kVmax, 0.f, kMass), jp);
    CHECK_EQ(play::jump_part_capped(jp, kVmax, kVmax, 1.f / 60.f, 0.f), jp);
    CHECK_EQ(play::jump_part_capped(jp, std::nanf(""), kVmax, 1.f / 60.f, kMass), jp);
    CHECK_EQ(play::jump_part_capped(jp, kVmax, INFINITY, 1.f / 60.f, kMass), jp);
}

NXTEST(apex_is_the_same_at_any_step_pattern) {
    const double ref = apex({1.0 / 60.0}, false);           // the console: the timer ends on a step boundary, nothing to fix
    CHECK_NEAR(ref, 3.500, 0.002);
    CHECK_NEAR(apex({1.0 / 60.0}, true), ref, 1e-6);         // at 1/60 the timer ends with 1.1e-8 s left: f = 7e-7, nothing to speak of
    // Steps where the timer ends inside a long one. Before: up to +3.3 %; after: within the integration noise of the uneven steps.
    const std::vector<std::vector<double>> patterns = {
        {1.0 / 144.0}, {1.0 / 240.0}, {1.0 / 120.0}, {0.0071},
        {0.0061, 0.0161}, {0.0065, 0.0158, 0.0062, 0.0064},      // the alternating short and long frames of the S49 144 runs
        {0.016}, {0.0163}, {0.0166}, {0.017},                   // near 60 but not 1/60: the timer ends at any point of a 16 ms step
    };
    double worst_before = 0, worst_after = 0;
    for (const auto& p : patterns) {
        const double before = apex(p, false), after = apex(p, true);
        worst_before = std::max(worst_before, std::fabs(before - ref) / ref);
        worst_after = std::max(worst_after, std::fabs(after - ref) / ref);
        CHECK(after <= before + 1e-9);                          // the fix only ever takes height away
        CHECK_NEAR(after, ref, 0.004 * ref);                    // within 0.4 % of the console
    }
    CHECK(worst_before > 0.02);                                 // the patterns do reach the bug (+2 % and more before)
    CHECK(worst_after < 0.004);
}

NXTEST(hooks_install_and_config_off) {
    nxrt::hooks_clear();
    nxrt::config_clear();
    constexpr uint64_t kBase = 0x7100000000ull;
    namespace site = nxrt::games::mpr::site;
    play::jump_install(kBase);
    CHECK_EQ(nxrt::hook_count_at(kBase + site::kJumpInput, nxrt::HookKind::Entry), 1u);
    CHECK_EQ(nxrt::hook_count_at(kBase + site::kJumpInputBlend, nxrt::HookKind::Mid), 1u);
    play::jump_install(kBase);                                  // idempotent
    CHECK_EQ(nxrt::hook_count_at(kBase + site::kJumpInput, nxrt::HookKind::Entry), 1u);
    play::jump_remove();
    CHECK_EQ(nxrt::hook_count_at(kBase + site::kJumpInputBlend, nxrt::HookKind::Mid), 0u);
    nxrt::config_set("physics", "jump_fix", "0");
    play::jump_install(kBase);
    CHECK_EQ(nxrt::hook_count_at(kBase + site::kJumpInput, nxrt::HookKind::Entry), 0u);
    play::jump_remove();
    nxrt::config_clear();
    nxrt::hooks_clear();
}
