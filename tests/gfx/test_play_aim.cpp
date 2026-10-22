// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode raw mouse aim (src/gfx/play/play_aim.cpp, see play_aim.h): yaw and pitch maths, the gate, the pitch clamp, the callback order,
// the transform hook and the hints. The driver reads and writes guest memory with ReadProcessMemory, so a fake player built on the host
// heap works: guest pointers are host pointers. Every offset is set through AimLayout.
#include "test_framework.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include "play/play_aim.h"

NXTEST_GROUP(play_aim)

namespace {

namespace play = nxrt::play;
using play::AimBlock;
using play::AimDriver;
using play::AimLayout;
using play::AimParams;
using play::GameHints;

constexpr double kPi = 3.14159265358979323846;
constexpr double kYawRad = 0.000767945;   // AimParams default, radians per count

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

template <class T>
T get(uint64_t addr) {
    T v{};
    std::memcpy(&v, reinterpret_cast<const void*>(addr), sizeof v);
    return v;
}

double dot3(const float* a, const float* b) { return double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2]; }

// One fake world: player, its GOC and inner pointers, the transform, the state manager with its logic and camera manager, the game
// state, the player state and the tweak. Built by init(); it must not move afterwards (the pointers point into it).
struct World {
    alignas(16) uint8_t player[0x800];
    alignas(16) uint8_t goc[0x100];
    alignas(16) uint8_t inner[0x100];
    alignas(16) float xf[12];
    alignas(16) uint8_t state_mgr[0x400];
    alignas(16) uint8_t logic[0x100];
    alignas(16) uint8_t cam_mgr[0x100];
    alignas(16) uint8_t fp_obj[0x40];
    alignas(16) uint8_t game_state[0x100];
    alignas(16) uint8_t player_state[0x100];
    alignas(16) uint8_t tweak[0x200];
    uint64_t cell_gs = 0;     // the gpGameState cell: holds the game state pointer
    uint64_t cell_tw = 0;     // the gpTweakPlayer cell: holds the tweak pointer
    AimLayout layout;

    uint64_t pl() const { return A(player); }
    uint64_t mgr() const { return A(state_mgr); }

    void set_identity() {
        const float id[12] = {1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 30};
        std::memcpy(xf, id, sizeof xf);
    }

    // World open for aim: first person, unmorphed, not locked on, no cinematic, camera manager on the first-person object, alive.
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
        layout.free_look_pitch = 0x5cc;
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
        put<float>(pl() + layout.free_look_pitch, 0.0f);
        set_identity();
    }
};

// Captures what the transform hook receives.
struct XfCapture {
    uint64_t player = 0;
    float xf[12] = {};
    int calls = 0;
};
void capture_xf(void* user, uint64_t player, const float xf[12]) {
    auto* c = static_cast<XfCapture*>(user);
    c->player = player;
    std::memcpy(c->xf, xf, sizeof c->xf);
    ++c->calls;
}

// Gate cases: each one closes exactly one condition on an otherwise open world.
void s_camera(World& w) { put<uint32_t>(w.pl() + w.layout.camera_state, 1); }
void s_morph(World& w) { put<uint32_t>(w.pl() + w.layout.morph_state, 2); }
void s_orbit(World& w) { put<uint32_t>(w.pl() + w.layout.orbit_state, 1); }
void s_cine1(World& w) { put<uint32_t>(A(w.cam_mgr) + w.layout.cam_cinematic, 1); }
void s_cine2(World& w) { put<uint32_t>(A(w.cam_mgr) + w.layout.cam_cinematic2, 1); }
void s_fp_id(World& w) { put<uint32_t>(A(w.fp_obj) + w.layout.fp_object_id, 8); }
void s_dead(World& w) { put<uint8_t>(A(w.player_state) + w.layout.ps_alive_byte, 0); }

struct GateCase {
    const char* name;
    void (*setup)(World&);
    AimBlock expect;
};

// Runs one apply on a closed gate and checks that nothing moved and the original still ran once.
void check_closed_gate(const GateCase& c) {
    World w;
    w.init();
    c.setup(w);
    AimDriver d(0, w.layout);
    AimParams p;
    put<float>(w.pl() + w.layout.free_look_pitch, 0.2f);
    float xf_before[12];
    std::memcpy(xf_before, w.xf, sizeof xf_before);
    int calls = 0;
    const bool ok = d.apply(w.pl(), w.mgr(), 100, -100, p, [&] { ++calls; });
    CHECK_MSG(!ok, c.name);
    CHECK_MSG(d.last_block() == c.expect, std::string(c.name) + ": block is " + play::aim_block_name(d.last_block()));
    CHECK_MSG(calls == 1, std::string(c.name) + ": original called " + std::to_string(calls) + " times");
    CHECK_MSG(std::memcmp(xf_before, w.xf, sizeof xf_before) == 0, std::string(c.name) + ": transform changed");
    CHECK_MSG(get<float>(w.pl() + w.layout.free_look_pitch) == 0.2f, std::string(c.name) + ": pitch memory changed");
    CHECK_MSG(!d.pitch_tracked(), std::string(c.name) + ": pitch still tracked");
}

}  // namespace

NXTEST(yaw_is_linear_in_dx) {
    AimParams p;

    World w;
    w.init();
    AimDriver d(0, w.layout);
    CHECK(d.apply(w.pl(), w.mgr(), 100, 0, p, {}));
    CHECK_NEAR(play::yaw_of_transform(w.xf), -100 * kYawRad, 1e-5);

    World neg;
    neg.init();
    AimDriver dn(0, neg.layout);
    CHECK(dn.apply(neg.pl(), neg.mgr(), -100, 0, p, {}));
    CHECK_NEAR(play::yaw_of_transform(neg.xf), 100 * kYawRad, 1e-5);

    World one;
    one.init();
    AimDriver d1(0, one.layout);
    CHECK(d1.apply(one.pl(), one.mgr(), 100, 0, p, {}));
    World two;
    two.init();
    AimDriver d2(0, two.layout);
    CHECK(d2.apply(two.pl(), two.mgr(), 50, 0, p, {}));
    CHECK(d2.apply(two.pl(), two.mgr(), 50, 0, p, {}));
    CHECK_NEAR(play::yaw_of_transform(two.xf), play::yaw_of_transform(one.xf), 1e-5);

    // 400 calls of 50 counts: 20000 counts is about -15.36 rad, past one turn, so the yaw is compared modulo 2 pi (atan2 wraps).
    World many;
    many.init();
    AimDriver dm(0, many.layout);
    for (int i = 0; i < 400; ++i) CHECK(dm.apply(many.pl(), many.mgr(), 50, 0, p, {}));
    CHECK_EQ(dm.applied_frames(), 400u);
    const double expected = -20000 * kYawRad;
    CHECK_NEAR(std::remainder(play::yaw_of_transform(many.xf) - expected, 2 * kPi), 0, 1e-3);
    const float* x = many.xf;
    CHECK_NEAR(std::sqrt(dot3(x, x)), 1, 1e-4);
    CHECK_NEAR(std::sqrt(dot3(x + 4, x + 4)), 1, 1e-4);
    CHECK_NEAR(std::sqrt(dot3(x + 8, x + 8)), 1, 1e-4);
    CHECK_NEAR(dot3(x, x + 4), 0, 1e-4);
}

NXTEST(translation_and_third_row_kept) {
    World w;
    w.init();
    AimDriver d(0, w.layout);
    CHECK(d.apply(w.pl(), w.mgr(), 100, 0, AimParams{}, {}));
    CHECK_NEAR(w.xf[3], 10, 1e-6);
    CHECK_NEAR(w.xf[7], 20, 1e-6);
    CHECK_NEAR(w.xf[11], 30, 1e-6);
    CHECK_NEAR(w.xf[8], 0, 1e-6);
    CHECK_NEAR(w.xf[9], 0, 1e-6);
    CHECK_NEAR(w.xf[10], 1, 1e-6);
}

NXTEST(rotate_transform_yaw_quarter_turn) {
    const float in[12] = {1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 30};
    float out[12];
    play::rotate_transform_yaw(in, kPi / 2, out);
    // forward column (xf[1], xf[5], xf[9]) turns from (0,1,0) into (-1,0,0)
    CHECK_NEAR(out[1], -1, 1e-5);
    CHECK_NEAR(out[5], 0, 1e-5);
    CHECK_NEAR(out[9], 0, 1e-5);
    // translation and row 3 kept
    CHECK_NEAR(out[3], 10, 1e-6);
    CHECK_NEAR(out[7], 20, 1e-6);
    CHECK_NEAR(out[11], 30, 1e-6);
    CHECK_NEAR(out[10], 1, 1e-6);
}

NXTEST(pitch_adopts_and_moves_with_dy) {
    AimParams p;

    // adopts the player's value and keeps it when dy is 0
    World w;
    w.init();
    put<float>(w.pl() + w.layout.free_look_pitch, 0.2f);
    AimDriver d(0, w.layout);
    CHECK(d.apply(w.pl(), w.mgr(), 0, 0, p, {}));
    CHECK(d.pitch_tracked());
    CHECK_NEAR(d.pitch(), 0.2, 1e-5);
    CHECK_NEAR(get<float>(w.pl() + w.layout.free_look_pitch), 0.2, 1e-5);

    // mouse up (dy < 0) raises the pitch
    CHECK(d.apply(w.pl(), w.mgr(), 0, -100, p, {}));
    CHECK_NEAR(d.pitch(), 0.2 + 100 * kYawRad, 1e-5);
    CHECK_NEAR(get<float>(w.pl() + w.layout.free_look_pitch), 0.2 + 100 * kYawRad, 1e-5);

    // mouse down (dy > 0) lowers it
    World dn;
    dn.init();
    put<float>(dn.pl() + dn.layout.free_look_pitch, 0.2f);
    AimDriver dd(0, dn.layout);
    CHECK(dd.apply(dn.pl(), dn.mgr(), 0, 100, p, {}));
    CHECK_NEAR(dd.pitch(), 0.2 - 100 * kYawRad, 1e-5);

    // invert_y flips it
    World iy;
    iy.init();
    put<float>(iy.pl() + iy.layout.free_look_pitch, 0.2f);
    AimDriver di(0, iy.layout);
    AimParams piy;
    piy.invert_y = true;
    CHECK(di.apply(iy.pl(), iy.mgr(), 0, -100, piy, {}));
    CHECK_NEAR(di.pitch(), 0.2 - 100 * kYawRad, 1e-5);

    // invert_x flips the yaw sign
    World ix;
    ix.init();
    AimDriver dx(0, ix.layout);
    AimParams pix;
    pix.invert_x = true;
    CHECK(dx.apply(ix.pl(), ix.mgr(), 100, 0, pix, {}));
    CHECK_NEAR(play::yaw_of_transform(ix.xf), 100 * kYawRad, 1e-5);

    // pitch_rad and yaw_rad are separate: pitch uses 2 * yaw_rad, yaw still uses yaw_rad
    World sep;
    sep.init();
    put<float>(sep.pl() + sep.layout.free_look_pitch, 0.2f);
    AimDriver ds(0, sep.layout);
    AimParams ps;
    ps.pitch_rad = 2 * kYawRad;
    CHECK(ds.apply(sep.pl(), sep.mgr(), 100, -100, ps, {}));
    CHECK_NEAR(play::yaw_of_transform(sep.xf), -100 * kYawRad, 1e-5);
    CHECK_NEAR(ds.pitch(), 0.2 + 200 * kYawRad, 1e-5);
}

NXTEST(pitch_clamped_by_tweak_and_override) {
    AimParams p;

    // tweak 50.0 is degrees
    World w;
    w.init();
    AimDriver d(0, w.layout);
    CHECK(d.apply(w.pl(), w.mgr(), 0, 0, p, {}));
    CHECK_NEAR(d.last_pitch_limit(), 50.0 * kPi / 180.0, 1e-4);

    // tweak 1.0 is radians
    World r;
    r.init();
    put<float>(A(r.tweak) + r.layout.tweak_max_pitch, 1.0f);
    AimDriver dr(0, r.layout);
    CHECK(dr.apply(r.pl(), r.mgr(), 0, 0, p, {}));
    CHECK_NEAR(dr.last_pitch_limit(), 1.0, 1e-4);

    // a huge dy clamps the stored pitch to the limit, and to minus the limit
    World h;
    h.init();
    AimDriver dh(0, h.layout);
    CHECK(dh.apply(h.pl(), h.mgr(), 0, -1e6, p, {}));
    const double lim = 50.0 * kPi / 180.0;
    CHECK_NEAR(dh.pitch(), lim, 1e-5);
    CHECK_NEAR(get<float>(h.pl() + h.layout.free_look_pitch), lim, 1e-5);
    CHECK(dh.apply(h.pl(), h.mgr(), 0, 1e6, p, {}));
    CHECK_NEAR(dh.pitch(), -lim, 1e-5);
    CHECK_NEAR(get<float>(h.pl() + h.layout.free_look_pitch), -lim, 1e-5);

    // unreadable tweak (the cell holds 0): 87 degrees
    World u;
    u.init();
    u.cell_tw = 0;
    AimDriver du(0, u.layout);
    CHECK(du.apply(u.pl(), u.mgr(), 0, 0, p, {}));
    CHECK_NEAR(du.last_pitch_limit(), 1.5184, 1e-4);

    // pitch_limit_rad overrides the tweak (tweak is 50 degrees here)
    World o;
    o.init();
    AimDriver dob(0, o.layout);
    AimParams po;
    po.pitch_limit_rad = 0.3;
    CHECK(dob.apply(o.pl(), o.mgr(), 0, -1e6, po, {}));
    CHECK_NEAR(dob.last_pitch_limit(), 0.3, 1e-6);
    CHECK_NEAR(dob.pitch(), 0.3, 1e-6);
}

NXTEST(original_runs_once_and_pitch_written_after) {
    World w;
    w.init();
    put<float>(w.pl() + w.layout.free_look_pitch, 0.2f);
    AimDriver d(0, w.layout);
    AimParams p;

    int calls = 0;
    double yaw_seen = 99;
    const bool ok = d.apply(w.pl(), w.mgr(), 100, -100, p, [&] {
        ++calls;
        yaw_seen = play::yaw_of_transform(w.xf);     // the new yaw is already in the transform
        put<float>(w.pl() + w.layout.free_look_pitch, 0.0f);   // the game writes its own pitch
    });
    CHECK(ok);
    CHECK_EQ(calls, 1);
    CHECK_NEAR(yaw_seen, -100 * kYawRad, 1e-5);
    CHECK_NEAR(d.pitch(), 0.2 + 100 * kYawRad, 1e-5);
    CHECK_NEAR(get<float>(w.pl() + w.layout.free_look_pitch), d.pitch(), 1e-5);

    // gate closed: the original still runs once
    put<uint32_t>(w.pl() + w.layout.orbit_state, 1);
    calls = 0;
    CHECK(!d.apply(w.pl(), w.mgr(), 100, -100, p, [&] { ++calls; }));
    CHECK_EQ(calls, 1);
}

NXTEST(gate_closes_for_each_game_state) {
    const GateCase cases[] = {
        {"camera state", s_camera, AimBlock::NotFirstPerson},
        {"morph ball", s_morph, AimBlock::Morphed},
        {"locked on", s_orbit, AimBlock::LockedOn},
        {"cinematic field 1", s_cine1, AimBlock::Cinematic},
        {"cinematic field 2", s_cine2, AimBlock::Cinematic},
        {"fp object id differs", s_fp_id, AimBlock::NotFirstPerson},
        {"player dead", s_dead, AimBlock::Dead},
    };
    for (const GateCase& c : cases) check_closed_gate(c);
}

NXTEST(gate_no_player_or_manager) {
    World w;
    w.init();
    AimDriver d(0, w.layout);
    AimParams p;
    int calls = 0;
    CHECK(!d.apply(0, w.mgr(), 100, -100, p, [&] { ++calls; }));
    CHECK(d.last_block() == AimBlock::NoPlayer);
    CHECK(!d.apply(w.pl(), 0, 100, -100, p, [&] { ++calls; }));
    CHECK(d.last_block() == AimBlock::NoPlayer);
    CHECK_EQ(calls, 2);
    CHECK_NEAR(w.xf[1], 0, 1e-6);
}

NXTEST(gate_bad_state_when_unreadable) {
    // camera manager pointer behind the logic block is 0
    World w;
    w.init();
    put<uint64_t>(A(w.logic) + w.layout.logic_camera_mgr, 0);
    AimDriver d(0, w.layout);
    AimParams p;
    int calls = 0;
    CHECK(!d.apply(w.pl(), w.mgr(), 100, -100, p, [&] { ++calls; }));
    CHECK(d.last_block() == AimBlock::BadState);
    CHECK_EQ(calls, 1);

    // player pointer below 0x10000 is never read: unreadable
    World u;
    u.init();
    AimDriver du(0, u.layout);
    CHECK(!du.apply(0x8000, u.mgr(), 100, -100, p, {}));
    CHECK(du.last_block() == AimBlock::BadState);
}

NXTEST(resync_adopts_game_pitch_after_gate_reopens) {
    World w;
    w.init();
    AimDriver d(0, w.layout);
    AimParams p;
    put<float>(w.pl() + w.layout.free_look_pitch, 0.3f);
    CHECK(d.apply(w.pl(), w.mgr(), 0, 0, p, {}));
    CHECK_NEAR(d.pitch(), 0.3, 1e-6);

    put<uint32_t>(w.pl() + w.layout.orbit_state, 1);
    CHECK(!d.apply(w.pl(), w.mgr(), 0, 0, p, {}));
    CHECK(!d.pitch_tracked());

    put<float>(w.pl() + w.layout.free_look_pitch, 0.05f);
    put<uint32_t>(w.pl() + w.layout.orbit_state, 0);
    CHECK(d.apply(w.pl(), w.mgr(), 0, 0, p, {}));
    CHECK(d.pitch_tracked());
    CHECK_NEAR(d.pitch(), 0.05, 1e-6);
}

NXTEST(transform_hook_replaces_memory_write) {
    World w;
    w.init();
    XfCapture cap;
    AimDriver d(0, w.layout);
    d.set_transform_fn(&capture_xf, &cap);
    AimParams p;
    CHECK(d.apply(w.pl(), w.mgr(), 100, 0, p, {}));
    CHECK_EQ(cap.calls, 1);
    CHECK_EQ(cap.player, w.pl());
    CHECK_NEAR(play::yaw_of_transform(cap.xf), -100 * kYawRad, 1e-5);
    // the hook took the transform: the memory still holds the old one
    CHECK_NEAR(w.xf[1], 0, 1e-6);
    CHECK_NEAR(w.xf[5], 1, 1e-6);

    // without a hook the memory is written
    World m;
    m.init();
    AimDriver dm(0, m.layout);
    CHECK(dm.apply(m.pl(), m.mgr(), 100, 0, p, {}));
    CHECK_NEAR(play::yaw_of_transform(m.xf), -100 * kYawRad, 1e-5);
}

NXTEST(hints_read_player_state) {
    World w;
    w.init();
    const uint64_t ps = A(w.player_state);
    put<uint32_t>(ps + w.layout.ps_current_beam, 2);
    put<uint32_t>(ps + w.layout.ps_visor, 1);
    for (uint32_t i = 0; i < 4; ++i) put<uint32_t>(ps + w.layout.ps_items + 8 * i, (i == 0 || i == 2) ? 1 : 0);
    AimDriver d(0, w.layout);

    GameHints h = d.hints(w.pl());
    CHECK(h.valid);
    CHECK(h.alive);
    CHECK_EQ(h.beam, 2);
    CHECK_EQ(h.visor, 1);
    CHECK_EQ(h.owned_beams, 5u);
    CHECK(!h.morphed);

    put<uint32_t>(w.pl() + w.layout.morph_state, 2);
    CHECK(d.hints(w.pl()).morphed);

    put<uint8_t>(ps + w.layout.ps_alive_byte, 0);
    h = d.hints(w.pl());
    CHECK(h.valid);
    CHECK(!h.alive);

    // player 0: only the player-state fields are read
    h = d.hints(0);
    CHECK(h.valid);
    CHECK(!h.morphed);

    // unreadable game state (the cell holds 0): not valid
    put<uint8_t>(ps + w.layout.ps_alive_byte, 1);
    w.cell_gs = 0;
    CHECK(!d.hints(w.pl()).valid);
}

NXTEST(aim_block_names_nonempty) {
    for (int i = 0; i <= static_cast<int>(AimBlock::BadState); ++i) {
        const char* name = play::aim_block_name(static_cast<AimBlock>(i));
        CHECK(name != nullptr);
        CHECK_MSG(name != nullptr && name[0] != '\0', "block " + std::to_string(i) + " has no name");
    }
}
