// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode free lock-on (S32): keep_in_view and AimDriver::keep_target_in_view / free_lock_active (play_aim.cpp), and the transform
// helpers xf_mul / xf_rel (play_lockcam.cpp). The fake player is built on the host heap as in test_play_aim.cpp: guest pointers are host
// pointers, and every offset is set through AimLayout.
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include "play/play_aim.h"
#include "play/play_lockcam.h"
#include "play/play_lookrate.h"

NXTEST_GROUP(play_lockcam)

namespace {

namespace play = nxrt::play;
using play::AimBlock;
using play::AimDriver;
using play::AimLayout;
using play::AimParams;

#define CHECK_NEAR(got, want, eps)                                                                                  \
    do {                                                                                                            \
        const double nx_g = static_cast<double>(got), nx_w = static_cast<double>(want);                             \
        if (!(std::fabs(nx_g - nx_w) <= (eps)))                                                                     \
            ::nxtest::fail(__FILE__, __LINE__, #got " ~ " #want,                                                    \
                           "got " + std::to_string(nx_g) + ", want " + std::to_string(nx_w));                       \
    } while (0)

uint64_t A(const void* p) { return reinterpret_cast<uint64_t>(p); }

template <class T>
void put(uint64_t addr, T v) { std::memcpy(reinterpret_cast<void*>(addr), &v, sizeof v); }

// Camera-space angles of the direction d (world axes, from the eye) for a view with heading yaw and pitch: the same axes as keep_in_view.
// ax: horizontal angle (+ = left), ay: vertical angle (+ = up), z: distance ahead (must be > 0 for the target to be in front).
struct CamAngles {
    double z, ax, ay;
};
CamAngles cam_angles(const double d[3], double yaw, double pitch) {
    const double sy = std::sin(yaw), cy = std::cos(yaw), sp = std::sin(pitch), cp = std::cos(pitch);
    const double z = d[0] * sy * cp + d[1] * sp + d[2] * cy * cp;      // forward = (sin y cos p, sin p, cos y cos p)
    const double xl = d[0] * cy - d[2] * sy;                           // left = (cos y, 0, -sin y)
    const double yu = -d[0] * sy * sp + d[1] * cp - d[2] * cy * sp;    // up = (-sin y sin p, cos p, -cos y sin p)
    return {z, std::atan2(xl, z), std::atan2(yu, z)};
}

// A rigid 3x4 transform: rotation Ry(ay) * Rx(ax) (Ry has the layout of rotate_transform_yaw: rows 0 and 2 mix), then the translation t.
void make_rigid(double ay, double ax, const float t[3], float out[12]) {
    const double cy = std::cos(ay), sy = std::sin(ay), cx = std::cos(ax), sx = std::sin(ax);
    const double R[3][3] = {
        {cy, sy * sx, sy * cx},
        {0.0, cx, -sx},
        {-sy, cy * sx, cy * cx},
    };
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) out[4 * i + j] = static_cast<float>(R[i][j]);
        out[4 * i + 3] = t[i];
    }
}

// One fake world for the free lock tests: player (with its body transform), state manager (logic, camera manager), first-person camera
// object, game state, player state and tweak. Built by init(); it must not move afterwards (the pointers point into it).
// The camera object is bigger than in test_play_aim.cpp: fp_base_xf (660) and fp_vertical_fov (0x25c) lie inside it.
struct LockWorld {
    alignas(16) uint8_t player[0x800];
    alignas(16) uint8_t goc[0x100];
    alignas(16) uint8_t inner[0x100];
    alignas(16) float xf[12];
    alignas(16) uint8_t state_mgr[0x400];
    alignas(16) uint8_t logic[0x100];
    alignas(16) uint8_t cam_mgr[0x100];
    alignas(16) uint8_t fp_obj[0x600];
    alignas(16) uint8_t game_state[0x100];
    alignas(16) uint8_t player_state[0x100];
    alignas(16) uint8_t tweak[0x200];
    uint64_t cell_gs = 0;     // the gpGameState cell: holds the game state pointer
    uint64_t cell_tw = 0;     // the gpTweakPlayer cell: holds the tweak pointer
    AimLayout layout;

    uint64_t pl() const { return A(player); }
    uint64_t mgr() const { return A(state_mgr); }

    // Open for aim: first person, unmorphed, not locked on, no cinematic, camera manager on the first-person object, alive, tweak 50 degrees.
    void init() {
        std::memset(player, 0, sizeof player);
        std::memset(goc, 0, sizeof goc);
        std::memset(inner, 0, sizeof inner);
        std::memset(state_mgr, 0, sizeof state_mgr);
        std::memset(logic, 0, sizeof logic);
        std::memset(cam_mgr, 0, sizeof cam_mgr);
        std::memset(fp_obj, 0, sizeof fp_obj);
        std::memset(game_state, 0, sizeof game_state);
        std::memset(player_state, 0, sizeof player_state);
        std::memset(tweak, 0, sizeof tweak);

        layout = AimLayout{};
        layout.player_goc = 0x08;
        layout.goc_inner = 0x20;
        layout.inner_xf = 0x40;
        layout.camera_state = 0x4d0;
        layout.morph_state = 0x4d4;
        layout.orbit_state = 0x4e0;
        layout.mgr_logic = 0x40;
        layout.logic_camera_mgr = 0x30;
        layout.gp_game_state_abs = A(&cell_gs);
        layout.gp_tweak_player_abs = A(&cell_tw);

        put<uint64_t>(pl() + layout.player_goc, A(goc));
        put<uint64_t>(A(goc) + layout.goc_inner, A(inner));
        put<uint64_t>(A(inner) + layout.inner_xf, A(xf));
        put<uint64_t>(mgr() + layout.mgr_logic, A(logic));
        put<uint64_t>(A(logic) + layout.logic_camera_mgr, A(cam_mgr));
        put<uint64_t>(A(cam_mgr) + layout.cam_fp_object, A(fp_obj));
        put<uint32_t>(A(cam_mgr) + layout.cam_current_id, 7);
        put<uint32_t>(A(cam_mgr) + layout.cam_cinematic, 0);
        put<uint32_t>(A(cam_mgr) + layout.cam_cinematic2, 0);
        put<uint32_t>(A(fp_obj) + layout.fp_object_id, 7);
        put<float>(A(fp_obj) + layout.fp_vertical_fov, 57.29578f);      // 1 radian, in degrees as the game keeps it

        cell_gs = A(game_state);
        put<uint64_t>(A(game_state) + 0x20, A(player_state));
        put<uint8_t>(A(player_state) + layout.ps_alive_byte, 1);
        put<uint32_t>(A(player_state) + layout.ps_current_beam, 0);
        put<uint32_t>(A(player_state) + layout.ps_visor, 0);
        for (uint32_t i = 0; i < 4; ++i) put<uint32_t>(A(player_state) + layout.ps_items + 8 * i, 1);

        cell_tw = A(tweak);
        put<float>(A(tweak) + layout.tweak_max_pitch, 50.0f);

        put<uint32_t>(pl() + layout.camera_state, 0);
        put<uint32_t>(pl() + layout.morph_state, 0);
        put<uint32_t>(pl() + layout.orbit_state, 0);
        put<uint8_t>(pl() + layout.orbit_reached, 0);
        put<float>(pl() + layout.free_look_pitch, 0.0f);

        const float id[12] = {1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 30};
        std::memcpy(xf, id, sizeof xf);
    }

    // The first-person camera's eye (the translation of fp_base_xf).
    void set_eye(float x, float y, float z) {
        const uint64_t fp = A(fp_obj);
        put<float>(fp + layout.fp_base_xf + 12, x);
        put<float>(fp + layout.fp_base_xf + 28, y);
        put<float>(fp + layout.fp_base_xf + 44, z);
    }

    // The locked target's world position, as CPlayerMP1::UpdateOrbitInput leaves it.
    void set_target(float x, float y, float z) {
        put<float>(pl() + layout.orbit_target_pos, x);
        put<float>(pl() + layout.orbit_target_pos + 4, y);
        put<float>(pl() + layout.orbit_target_pos + 8, z);
    }

    // The lock-on state: an object lock (orbit state 1) and the game's own lock-on has centred the target (reached).
    void set_locked_and_centred() {
        put<uint32_t>(pl() + layout.orbit_state, 1);
        put<uint8_t>(pl() + layout.orbit_reached, 1);
    }
};

}  // namespace

NXTEST(lockcam_keep_in_view_straight_ahead_untouched) {
    double yaw = 0, pitch = 0;
    const double d[3] = {0, 0, 10};
    CHECK(!play::keep_in_view(d, &yaw, &pitch, std::tan(0.6), std::tan(0.4), 1.5));
    CHECK(yaw == 0.0);
    CHECK(pitch == 0.0);
}

NXTEST(lockcam_keep_in_view_inside_box_untouched) {
    double yaw = 0.1, pitch = -0.05;
    const double d[3] = {2, 0, 10};     // about 11 degrees to the left, inside the 0.6 rad half width
    CHECK(!play::keep_in_view(d, &yaw, &pitch, std::tan(0.6), std::tan(0.4), 1.5));
    CHECK(yaw == 0.1);
    CHECK(pitch == -0.05);
}

NXTEST(lockcam_keep_in_view_far_left_and_right_stop_on_edge) {
    const double lim_x = 0.3;
    const double tan_v = std::tan(0.4);

    // target far to the left: the view turns left (yaw grows) until the target is on the edge of the box
    {
        double yaw = 0, pitch = 0;
        const double d[3] = {10, 0, 10};
        CHECK(play::keep_in_view(d, &yaw, &pitch, std::tan(lim_x), tan_v, 1.5));
        CHECK(yaw > 0);
        const CamAngles c = cam_angles(d, yaw, pitch);
        CHECK_NEAR(c.ax, lim_x, 1e-4);
    }
    // mirrored: target far to the right, the view turns right (yaw falls) to the edge at -0.3
    {
        double yaw = 0, pitch = 0;
        const double d[3] = {-10, 0, 10};
        CHECK(play::keep_in_view(d, &yaw, &pitch, std::tan(lim_x), tan_v, 1.5));
        CHECK(yaw < 0);
        const CamAngles c = cam_angles(d, yaw, pitch);
        CHECK_NEAR(c.ax, -lim_x, 1e-4);
    }
}

NXTEST(lockcam_keep_in_view_above_and_below_stop_on_edge) {
    const double lim_y = 0.2;
    const double tan_h = std::tan(0.6);

    // target above: the pitch rises to the edge, the heading stays
    {
        double yaw = 0, pitch = 0;
        const double d[3] = {0, 10, 10};
        CHECK(play::keep_in_view(d, &yaw, &pitch, tan_h, std::tan(lim_y), 1.5));
        CHECK(pitch > 0);
        CHECK_NEAR(yaw, 0, 1e-6);
        CHECK_NEAR(cam_angles(d, yaw, pitch).ay, lim_y, 1e-4);
    }
    // target below: the pitch falls to the edge, the heading stays
    {
        double yaw = 0, pitch = 0;
        const double d[3] = {0, -10, 10};
        CHECK(play::keep_in_view(d, &yaw, &pitch, tan_h, std::tan(lim_y), 1.5));
        CHECK(pitch < 0);
        CHECK_NEAR(yaw, 0, 1e-6);
        CHECK_NEAR(cam_angles(d, yaw, pitch).ay, -lim_y, 1e-4);
    }
}

NXTEST(lockcam_keep_in_view_target_behind_turns_round) {
    double yaw = 0, pitch = 0;
    const double d[3] = {0, 0, -10};
    CHECK(play::keep_in_view(d, &yaw, &pitch, std::tan(0.6), std::tan(0.4), 1.5));
    const CamAngles c = cam_angles(d, yaw, pitch);
    CHECK_MSG(c.z > 0, "target still behind the view");
    CHECK_MSG(std::fabs(c.ax) <= 0.6 + 1e-4, "horizontal angle " + std::to_string(c.ax));
    CHECK_MSG(std::fabs(c.ay) <= 0.4 + 1e-4, "vertical angle " + std::to_string(c.ay));
}

NXTEST(lockcam_keep_in_view_pitch_limit_holds) {
    // target almost straight up: the pitch goes to the limit and no further
    double yaw = 0, pitch = 0;
    const double d[3] = {0, 10, 1};
    CHECK(play::keep_in_view(d, &yaw, &pitch, std::tan(0.6), std::tan(0.4), 0.5));
    CHECK(pitch <= 0.5 + 1e-9);
    CHECK_NEAR(pitch, 0.5, 1e-9);
    CHECK_NEAR(yaw, 0, 1e-9);
}

NXTEST(lockcam_keep_in_view_random_sweep) {
    // 200 fixed pseudo-random cases (LCG, fixed seed): the target ends inside the box unless the pitch hit its limit,
    // and a second call changes nothing.
    uint32_t seed = 12345u;
    auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<double>(seed >> 8) / 16777216.0;      // [0, 1)
    };
    const double tan_h = std::tan(0.7), tan_v = std::tan(0.45), lim = 1.5;
    int checked = 0;
    for (int i = 0; i < 200; ++i) {
        double yaw = -3.0 + 6.0 * rnd();
        double pitch = -1.0 + 2.0 * rnd();
        double d[3] = {0, 0, 0};
        double n = 0;
        while (n < 0.1) {      // random direction
            for (double& comp : d) comp = 2.0 * rnd() - 1.0;
            n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        }
        const double len = 1.0 + 29.0 * rnd();      // distance 1..30
        for (double& comp : d) comp *= len / n;
        const std::string tag = "case " + std::to_string(i);

        play::keep_in_view(d, &yaw, &pitch, tan_h, tan_v, lim);
        const bool again = play::keep_in_view(d, &yaw, &pitch, tan_h, tan_v, lim);
        CHECK_MSG(!again, tag + ": second call changed the view");
        if (std::fabs(std::fabs(pitch) - lim) < 1e-9) continue;      // the pitch hit its limit: the box may stay out of reach
        const CamAngles c = cam_angles(d, yaw, pitch);
        CHECK_MSG(c.z > 0, tag + ": target behind the view");
        CHECK_MSG(std::fabs(c.ax) <= 0.7 + 1e-3, tag + ": horizontal angle " + std::to_string(c.ax));
        CHECK_MSG(std::fabs(c.ay) <= 0.45 + 1e-3, tag + ": vertical angle " + std::to_string(c.ay));
        ++checked;
    }
    CHECK(checked > 150);      // most cases must reach the box without touching the pitch limit
}

NXTEST(lockcam_xf_rel_then_mul_round_trips) {
    const float ta[3] = {1, 2, 3};
    const float tb[3] = {-4, 0.5f, 2};
    float a[12], b[12];
    make_rigid(0.7, 0.0, ta, a);        // rotation about Y by 0.7, translation (1, 2, 3)
    make_rigid(-0.3, 0.2, tb, b);       // Y by -0.3 combined with X by 0.2, translation (-4, 0.5, 2)

    const float id[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    float out[12], rel[12];

    // xf_rel(v, s) = v * s^-1; then xf_mul(rel, s) = v * s^-1 * s = v
    for (int k = 0; k < 2; ++k) {
        const float* v = k ? b : a;
        const float* s = k ? a : b;
        play::xf_rel(v, s, rel);
        play::xf_mul(rel, s, out);
        for (int i = 0; i < 12; ++i) CHECK_MSG(std::fabs(out[i] - v[i]) <= 1e-4, "element " + std::to_string(i) + " of case " + std::to_string(k));
    }

    // the identity is neutral on the left
    play::xf_mul(id, a, out);
    for (int i = 0; i < 12; ++i) CHECK_NEAR(out[i], a[i], 1e-6);

    // a transform relative to itself is the identity
    play::xf_rel(a, a, out);
    for (int i = 0; i < 12; ++i) CHECK_NEAR(out[i], id[i], 1e-5);
}

NXTEST(lockcam_free_lock_active_needs_setting_orbit_reached_and_visor) {
    LockWorld w;
    w.init();
    w.set_locked_and_centred();
    AimDriver d(0, w.layout);

    // setting off: the game's own lock-on, whatever the state
    CHECK(!d.free_lock_active(w.pl()));

    d.set_free_lock(true);
    CHECK(d.free_lock_active(w.pl()));

    // the game's lock-on still centring the target (reached 0): not yet
    put<uint8_t>(w.pl() + w.layout.orbit_reached, 0);
    CHECK(!d.free_lock_active(w.pl()));
    put<uint8_t>(w.pl() + w.layout.orbit_reached, 1);

    // a grapple, forced orbit, point or carcass (orbit state 2 here): not the ordinary lock
    put<uint32_t>(w.pl() + w.layout.orbit_state, 2);
    CHECK(!d.free_lock_active(w.pl()));
    put<uint32_t>(w.pl() + w.layout.orbit_state, 1);

    // the scan visor (2) keeps the classic lock; the combat visor (0) does not
    put<uint32_t>(A(w.player_state) + w.layout.ps_visor, 2);
    CHECK(!d.free_lock_active(w.pl()));
    put<uint32_t>(A(w.player_state) + w.layout.ps_visor, 0);
    CHECK(d.free_lock_active(w.pl()));
}

NXTEST(lockcam_free_lock_view_held_inside_margin) {
    // The target 10 units straight ahead of the eye; a mouse move that would turn the view by 2 radians.
    AimParams p;
    const double vfov = 1.0;                       // what LockWorld::init writes
    const double aspect = 16.0 / 9.0, margin = 0.15;
    const double tan_half = std::tan(vfov * 0.5);
    const double bound_x = std::atan(tan_half * aspect * (1.0 - 2.0 * margin));    // the margin box: 0.7 of the half picture
    const double bound_y = std::atan(tan_half * (1.0 - 2.0 * margin));

    {
        LockWorld w;
        w.init();
        w.set_eye(0.0f, 1.5f, 0.0f);
        w.set_target(0.0f, 1.5f, 10.0f);
        w.set_locked_and_centred();
        AimDriver d(0, w.layout);
        d.set_free_lock(true);
        d.set_free_lock_view(margin, aspect, 0.0);

        const double dx = 2.0 / p.yaw_rad;         // 2 radians of turn (the sign is the game's: mouse right turns right)
        CHECK(d.apply(w.pl(), w.mgr(), dx, 0, p, {}));
        const double yaw = play::yaw_of_transform(w.xf);
        const double pitch = d.pitch();
        const double dv[3] = {0, 0, 10};           // target minus eye
        const CamAngles c = cam_angles(dv, yaw, pitch);
        CHECK_MSG(c.z > 0, "target behind the view");
        CHECK_MSG(std::fabs(c.ax) <= bound_x + 1e-3, "horizontal angle " + std::to_string(c.ax) + " past " + std::to_string(bound_x));
        CHECK_MSG(std::fabs(c.ay) <= bound_y + 1e-3, "vertical angle " + std::to_string(c.ay) + " past " + std::to_string(bound_y));
    }

    // a small turn (0.1 rad, inside the margin) is not held back: the body turns as the mouse says
    {
        LockWorld w;
        w.init();
        w.set_eye(0.0f, 1.5f, 0.0f);
        w.set_target(0.0f, 1.5f, 10.0f);
        w.set_locked_and_centred();
        AimDriver d(0, w.layout);
        d.set_free_lock(true);
        d.set_free_lock_view(margin, aspect, 0.0);

        const double dx = 0.1 / p.yaw_rad;         // dyaw = -0.1 (mouse right turns right)
        CHECK(d.apply(w.pl(), w.mgr(), dx, 0, p, {}));
        CHECK_NEAR(play::yaw_of_transform(w.xf), -0.1, 1e-5);
    }
}

NXTEST(lockcam_gate_open_only_with_free_lock) {
    LockWorld w;
    w.init();
    w.set_locked_and_centred();
    AimDriver d(0, w.layout);

    // classic: the ordinary lock closes the gate
    d.set_free_lock(false);
    const AimBlock classic = d.gate(w.pl(), w.mgr());
    CHECK_MSG(classic == AimBlock::LockedOn, std::string("classic gate: ") + play::aim_block_name(classic));

    // free lock: the gate stays open with the same orbit state
    d.set_free_lock(true);
    const AimBlock free_lock = d.gate(w.pl(), w.mgr());
    CHECK_MSG(free_lock == AimBlock::None, std::string("free lock gate: ") + play::aim_block_name(free_lock));
}

NXTEST(turn_ramp_speeds_up_with_tau_up_from_rest) {
    const double dt = 1.0 / 60.0, tau_up = 0.0262, target = 3.0;
    double omega = 0.0;
    const double first = play::turn_ramp_step(&omega, target, dt, tau_up, 0.0117);
    CHECK_NEAR(omega, target * (1.0 - std::exp(-dt / tau_up)), 0.01);
    CHECK_NEAR(first, omega, 0.0);

    for (int i = 1; i < 5; ++i) play::turn_ramp_step(&omega, target, dt, tau_up, 0.0117);
    CHECK_NEAR(omega, target * (1.0 - std::exp(-5.0 * dt / tau_up)), 0.01);

    // it approaches the target from below and never passes it
    for (int i = 0; i < 300; ++i) {
        const double before = omega;
        play::turn_ramp_step(&omega, target, dt, tau_up, 0.0117);
        CHECK_MSG(omega <= target, "omega " + std::to_string(omega) + " past the target");
        CHECK_MSG(omega >= before, "omega fell while speeding up");
    }
}

NXTEST(turn_ramp_frame_rate_independent) {
    // one second at 60 steps a second and at 144 steps a second, speeding up with tau_up 0.2
    const double tau_up = 0.2, target = 3.0;
    double a = 0.0, b = 0.0;
    for (int i = 0; i < 60; ++i) play::turn_ramp_step(&a, target, 1.0 / 60.0, tau_up, 0.05);
    for (int i = 0; i < 144; ++i) play::turn_ramp_step(&b, target, 1.0 / 144.0, tau_up, 0.05);
    CHECK_NEAR(a, b, 1e-6);
    CHECK_NEAR(a, target * (1.0 - std::exp(-1.0 / tau_up)), 1e-6);
}

NXTEST(turn_ramp_release_decays_with_tau_down_and_zeroes) {
    const double dt = 1.0 / 60.0, tau_down = 0.0117;
    double omega = 3.0;
    play::turn_ramp_step(&omega, 0.0, dt, 0.0262, tau_down);
    CHECK_NEAR(omega, 3.0 * std::exp(-dt / tau_down), 1e-12);

    // below 1e-5 with the target at 0 the speed is set to exactly 0
    double small = 2e-5;
    play::turn_ramp_step(&small, 0.0, dt, 0.0262, tau_down);
    CHECK(small == 0.0);

    // a full release comes to exactly 0 well within a second
    int steps = 1;
    while (omega != 0.0 && steps < 100) {
        play::turn_ramp_step(&omega, 0.0, dt, 0.0262, tau_down);
        ++steps;
    }
    CHECK(omega == 0.0);
    CHECK_MSG(steps < 60, "took " + std::to_string(steps) + " steps to stop");
}

NXTEST(turn_ramp_reversing_uses_tau_down_first) {
    // omega +2 and the stick asks for -3: the first step is slowing down, so tau_down (not tau_up) sets its size
    const double dt = 1.0 / 60.0, tau_up = 0.0262, tau_down = 0.0117;
    double omega = 2.0;
    play::turn_ramp_step(&omega, -3.0, dt, tau_up, tau_down);
    CHECK_NEAR(omega, 2.0 + (-3.0 - 2.0) * (1.0 - std::exp(-dt / tau_down)), 1e-12);
}

NXTEST(turn_ramp_ignores_bad_dt_and_nan_target) {
    const double tau_up = 0.0262, tau_down = 0.0117;
    double omega = 1.5;
    CHECK(play::turn_ramp_step(&omega, 3.0, 0.0, tau_up, tau_down) == 1.5);
    CHECK(omega == 1.5);
    play::turn_ramp_step(&omega, 3.0, -1.0 / 60.0, tau_up, tau_down);
    CHECK(omega == 1.5);
    play::turn_ramp_step(&omega, 3.0, std::nan(""), tau_up, tau_down);
    CHECK(omega == 1.5);
    play::turn_ramp_step(&omega, std::nan(""), 1.0 / 60.0, tau_up, tau_down);
    CHECK(omega == 1.5);
}

namespace {

// One step of the freelook hook as it runs the carry (play_lockcam.cpp), against a float on the host heap that stands for the game's pitch target. game(t)
// is what the game's own call does to the target. Returns the number in the target after the step.
struct CarryRig {
    alignas(16) float target = 0.f;
    play::LookCarry carry;
    uint64_t at() const { return A(&target); }
    template <class F>
    float step(F game, double phase, double limit = 1.5184) {
        if (!carry.begin(1, at())) return target;
        carry.restore(at());
        target = game(target);
        carry.after_call(at());
        carry.finish(at(), phase, limit);
        return target;
    }
};

}  // namespace

NXTEST(carry_accumulating_target_is_carried_on_and_restored) {
    CarryRig r;
    auto up = [](float t) { return t + 0.02f; };
    r.step(up, 0.5);      // history is still flat: nothing carried
    CHECK_NEAR(r.target, 0.02, 1e-6);
    r.step(up, 0.5);      // two equal speeds in a row: carried half a call on
    CHECK_NEAR(r.carry.cur(), 0.04, 1e-6);
    CHECK_NEAR(r.target, 0.04 + 0.01, 1e-6);
    r.step(up, 0.5);      // the game gets its own 0.04 back and adds 0.02
    CHECK_NEAR(r.carry.cur(), 0.06, 1e-6);
    CHECK_NEAR(r.target, 0.06 + 0.01, 1e-6);
    r.step(up, 0.0);      // 60 steps per second: no carry at all
    CHECK_NEAR(r.target, 0.08, 1e-6);
}

NXTEST(carry_target_set_outright_is_not_extrapolated) {
    // Classic with ZR held, Pointer: the game sets the target from the input, it does not accumulate
    CarryRig r;
    auto hold0 = [](float) { return 0.f; };
    auto set5 = [](float) { return 0.5f; };
    r.step(hold0, 1.0);
    r.step(hold0, 1.0);
    r.step(set5, 1.0);      // a jump from rest
    CHECK_NEAR(r.target, 0.5, 1e-6);      // it used to be 1.0 here (0.5 + 0.5 * phase)
    r.step(set5, 1.0);
    CHECK_NEAR(r.target, 0.5, 1e-6);
    r.step(hold0, 1.0);      // back to level
    CHECK_NEAR(r.target, 0.0, 1e-6);
}

NXTEST(carry_stops_at_once_when_the_stick_is_let_go) {
    CarryRig r;
    auto up = [](float t) { return t + 0.02f; };
    auto rest = [](float t) { return t; };
    for (int i = 0; i < 4; ++i) r.step(up, 0.75);
    r.step(rest, 0.75);
    CHECK_NEAR(r.target, 0.08, 1e-6);      // the game's own target, no lead left
    r.step(rest, 0.75);
    CHECK_NEAR(r.target, 0.08, 1e-6);
}

NXTEST(carry_is_held_inside_the_pitch_limits) {
    CarryRig r;
    r.target = 1.46f;
    auto up = [](float t) { return std::min(t + 0.02f, 1.50f); };      // the game's own limit is a little inside the aim's
    for (int i = 0; i < 8; ++i) {
        r.step(up, 1.0, 1.5184);
        CHECK_MSG(r.target <= 1.5184f + 1e-6f, "step " + std::to_string(i) + ": " + std::to_string(r.target));
    }
    CarryRig d;
    d.target = -1.46f;
    auto down = [](float t) { return std::max(t - 0.02f, -1.50f); };
    for (int i = 0; i < 8; ++i) {
        d.step(down, 1.0, 1.5184);
        CHECK_MSG(d.target >= -1.5184f - 1e-6f, "step " + std::to_string(i) + ": " + std::to_string(d.target));
    }
}

NXTEST(carry_release_puts_the_games_target_back) {
    // the gyro button is pressed (the clock goes off): the carried offset must not stay in the game's target
    CarryRig r;
    auto up = [](float t) { return t + 0.02f; };
    for (int i = 0; i < 4; ++i) r.step(up, 0.5);
    CHECK_NEAR(r.target, 0.08 + 0.01, 1e-6);
    CHECK_NEAR(r.carry.cur(), 0.08, 1e-6);
    r.carry.release(1, r.at());
    CHECK_NEAR(r.target, 0.08, 1e-6);
    CHECK(!r.carry.have());
    // nothing to release now: the memory is left alone, whatever it holds
    r.target = 0.3f;
    r.carry.release(1, r.at());
    CHECK(r.target == 0.3f);
    // and the next timed step starts from what the game has
    r.step(up, 0.5);
    CHECK_NEAR(r.target, 0.32, 1e-6);
}

NXTEST(carry_release_keeps_what_else_moved_the_target) {
    CarryRig r;
    auto up = [](float t) { return t + 0.02f; };
    for (int i = 0; i < 4; ++i) r.step(up, 0.5);
    r.target += 0.1f;      // the mouse moved the target by 0.1 after the hook left it
    r.carry.release(1, r.at());
    CHECK_NEAR(r.target, 0.18, 1e-6);
}

NXTEST(carry_release_for_another_player_or_an_unreadable_target_does_nothing) {
    auto up = [](float t) { return t + 0.02f; };
    CarryRig r;
    for (int i = 0; i < 4; ++i) r.step(up, 0.5);
    const float before = r.target;
    r.carry.release(2, r.at());      // another player object
    CHECK(r.target == before);
    CHECK(!r.carry.have());

    CarryRig n;
    for (int i = 0; i < 4; ++i) n.step(up, 0.5);
    n.target = std::nanf("");
    n.carry.release(1, n.at());
    CHECK(std::isnan(n.target));
    CHECK(!n.carry.have());
}

NXTEST(carry_begin_fails_on_a_bad_target_and_drops_the_carry) {
    CarryRig r;
    auto up = [](float t) { return t + 0.02f; };
    for (int i = 0; i < 3; ++i) r.step(up, 0.5);
    CHECK(r.carry.have());
    r.target = std::nanf("");
    CHECK(!r.carry.begin(1, r.at()));
    CHECK(!r.carry.have());
    CHECK(!r.carry.begin(1, 0));      // an address that cannot be read
}
