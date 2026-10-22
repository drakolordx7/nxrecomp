// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode, hook level: the ProcessInput entry hook and the frame-boundary hook of src/gfx/play/play.cpp, driven the way lifted code drives
// them (nx_hook_entry on the registered guest address) against a fake player built in host memory. The game's own ProcessInput and
// CActorMP1::SetTransform are stand-in guest functions registered at the real addresses. This proves the whole path of the raw mouse aim
// (accumulated counts -> yaw on the transform through SetTransform, pitch in the free-look field after the game's function ran) without
// the game having to reach gameplay.
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>

#include "test_framework.h"

#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/input.h"
#include "nxrt/play.h"
#include "play/play_aim.h"

NXTEST_GROUP(play_hook)

namespace {

namespace play = nxrt::play;
namespace input = nxrt::input;

constexpr uint64_t kBase = 0x5A00000000ull;
constexpr uint64_t kUpdate = 0x3c81c8, kProcessInput = 0xc70334, kSetTransform = 0xc4d588;
constexpr double kYaw = 0.000767945;     // the factory sensitivity, radians per count

struct World {
    std::vector<uint8_t> player = std::vector<uint8_t>(0x800), mgr = std::vector<uint8_t>(0x4e0200), logic = std::vector<uint8_t>(0x100),
                         cam = std::vector<uint8_t>(0x100), fp = std::vector<uint8_t>(0x40), goc = std::vector<uint8_t>(0x100),
                         inner = std::vector<uint8_t>(0x100), tweak = std::vector<uint8_t>(0x200);
    alignas(16) float xf[12] = {1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 30};
    int original_calls = 0, set_transform_calls = 0;
    double yaw_seen_by_original = 0;
    float pitch_seen_by_original = 0;
    bool ready = false;
};
World g_w;

template <class T>
void put(std::vector<uint8_t>& b, size_t off, T v) { std::memcpy(b.data() + off, &v, sizeof v); }
template <class T>
T get(const std::vector<uint8_t>& b, size_t off) { T v; std::memcpy(&v, b.data() + off, sizeof v); return v; }
uint64_t addr(const std::vector<uint8_t>& b) { return reinterpret_cast<uint64_t>(b.data()); }

// the stand-in for CPlayerMP1::ProcessInput: records what the aim had already done, then does what the game's free-look code does (levels the pitch)
void fake_process_input(::Ctx*) {
    g_w.original_calls++;
    g_w.yaw_seen_by_original = play::yaw_of_transform(g_w.xf);
    g_w.pitch_seen_by_original = get<float>(g_w.player, 0x5cc);
    put<float>(g_w.player, 0x5cc, 0.0f);
}

// the stand-in for CActorMP1::SetTransform(this = x0, CTransform4f const* = x1)
void fake_set_transform(::Ctx* c) {
    g_w.set_transform_calls++;
    std::memcpy(g_w.xf, reinterpret_cast<const void*>(c->x[1]), sizeof g_w.xf);
}

void commit(uint64_t a, size_t n) { VirtualAlloc(reinterpret_cast<void*>(a), n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }

void setup() {
    if (g_w.ready) return;
    g_w.ready = true;
    // two guest globals at their real offsets from the base
    commit(kBase + 0x459c000, 0x2000);
    commit(kBase + 0x4b32000, 0x2000);
    put<uint64_t>(g_w.mgr, 0x4e0150, addr(g_w.logic));
    put<uint64_t>(g_w.logic, 0x30, addr(g_w.cam));
    put<uint32_t>(g_w.cam, 0x00, 7);
    put<uint64_t>(g_w.cam, 0x80, addr(g_w.fp));
    put<uint32_t>(g_w.fp, 0x10, 7);
    put<uint64_t>(g_w.player, 0x08, addr(g_w.goc));
    put<uint64_t>(g_w.goc, 0x20, addr(g_w.inner));
    put<uint64_t>(g_w.inner, 0x40, reinterpret_cast<uint64_t>(g_w.xf));
    put<float>(g_w.tweak, 0x138, 50.0f);
    *reinterpret_cast<uint64_t*>(kBase + 0x4b32880) = addr(g_w.tweak);
    *reinterpret_cast<uint64_t*>(kBase + 0x459cc70) = 0;     // no game state: the hints are not valid, the gate does not look at the player state

    const auto dir = std::filesystem::temp_directory_path() / "nxrt_play_hook_test";
    std::filesystem::create_directories(dir);
    nxrt::config_set("play", "controls", (dir / "controls.ini").string().c_str());
    nxrt::config_set("play", "game_base", "0x5A00000000");
    nxrt::fn_register(kBase + kProcessInput, &fake_process_input);
    nxrt::fn_register(kBase + kSetTransform, &fake_set_transform);
    play::load();
    play::StartOptions o;
    o.overlay = false;
    CHECK(play::start(o));
}

void reset_world() {
    setup();
    const float id[12] = {1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 30};
    std::memcpy(g_w.xf, id, sizeof id);
    put<uint32_t>(g_w.player, 0x4d0, 0);
    put<uint32_t>(g_w.player, 0x4d4, 0);
    put<uint32_t>(g_w.player, 0x4e0, 0);
    put<float>(g_w.player, 0x5cc, 0.0f);
    put<uint32_t>(g_w.cam, 0x08, 0);
    put<uint32_t>(g_w.cam, 0x18, 0);
    // a frame with the gate closed makes the aim forget its tracked pitch (as a lock-on does) so every case starts from the pitch set above
    put<uint32_t>(g_w.player, 0x4e0, 1);
    ::Ctx c{};
    nx_hook_entry(&c, kBase + kUpdate);
    c.x[0] = addr(g_w.player);
    c.x[2] = addr(g_w.mgr);
    nx_hook_entry(&c, kBase + kProcessInput);
    put<uint32_t>(g_w.player, 0x4e0, 0);
    put<float>(g_w.player, 0x5cc, 0.0f);
    std::memcpy(g_w.xf, id, sizeof id);
    g_w.original_calls = g_w.set_transform_calls = 0;
    input::mouse_delta_take();
}

int frame_boundary(::Ctx* c) { return nx_hook_entry(c, kBase + kUpdate); }

// one rendered frame with one ProcessInput after the boundary; the counts arrive before the call (late sampling)
int frame(double dx, double dy) {
    ::Ctx c{};
    frame_boundary(&c);
    input::mouse_delta_add(dx, dy);
    c.x[0] = addr(g_w.player);
    c.x[2] = addr(g_w.mgr);
    return nx_hook_entry(&c, kBase + kProcessInput);
}

double yaw() { return play::yaw_of_transform(g_w.xf); }
double pitch() { return get<float>(g_w.player, 0x5cc); }

}  // namespace

NXTEST(hook_turns_the_player_by_counts_times_sensitivity) {
    reset_world();
    CHECK_EQ(frame(100, 0), 1);               // handled: the hook ran the game's function itself
    CHECK_EQ(g_w.original_calls, 1);
    CHECK_EQ(g_w.set_transform_calls, 1);     // through the game's SetTransform
    CHECK(std::fabs(yaw() - (-100 * kYaw)) < 1e-5);
    CHECK(g_w.xf[3] == 10.0f && g_w.xf[7] == 20.0f && g_w.xf[11] == 30.0f);    // position untouched
    // and left
    reset_world();
    frame(-250, 0);
    CHECK(std::fabs(yaw() - (250 * kYaw)) < 1e-5);
}

NXTEST(hook_pitch_survives_the_games_own_free_look_code) {
    reset_world();
    frame(0, -100);                           // mouse up 100 counts
    // the game's function saw the new yaw/pitch state and then levelled the pitch to 0; the hook put the mouse's pitch back afterwards
    CHECK(g_w.original_calls == 1);
    CHECK(std::fabs(pitch() - 100 * kYaw) < 1e-5);
    frame(0, 40);                             // down 40
    CHECK(std::fabs(pitch() - 60 * kYaw) < 1e-5);
}

NXTEST(hook_pitch_is_clamped_to_the_tweak_limit) {
    reset_world();
    frame(0, -1000000);
    CHECK(std::fabs(pitch() - 50.0 * 3.14159265358979323846 / 180.0) < 1e-4);     // tweak: 50 degrees
    frame(0, 4000000);
    CHECK(std::fabs(pitch() + 50.0 * 3.14159265358979323846 / 180.0) < 1e-4);
}

NXTEST(hook_is_linear_in_the_counts_whatever_the_frame_rate) {
    reset_world();
    frame(1000, -400);
    const double y1 = yaw(), p1 = pitch();
    reset_world();
    for (int i = 0; i < 100; ++i) frame(10, -4);
    CHECK(std::fabs(yaw() - y1) < 1e-4);
    CHECK(std::fabs(pitch() - p1) < 1e-4);
    reset_world();
    for (int i = 0; i < 1000; ++i) frame(1, -0.4);         // counts below one per frame add up too (late sampling keeps the fractions)
    CHECK(std::fabs(yaw() - y1) < 1e-3);
}

NXTEST(hook_takes_the_counts_once_per_rendered_frame) {
    reset_world();
    ::Ctx c{};
    frame_boundary(&c);
    c.x[0] = addr(g_w.player);
    c.x[2] = addr(g_w.mgr);
    input::mouse_delta_add(100, 0);
    nx_hook_entry(&c, kBase + kProcessInput);                // first tick of the frame takes them
    input::mouse_delta_add(100, 0);
    nx_hook_entry(&c, kBase + kProcessInput);                // second tick of the same frame: the new counts wait for the next frame
    CHECK(g_w.original_calls == 2);
    CHECK(std::fabs(yaw() - (-100 * kYaw)) < 1e-5);
    frame(0, 0);                                             // next frame: the 100 that waited are applied
    CHECK(std::fabs(yaw() - (-200 * kYaw)) < 1e-5);
}

NXTEST(hook_leaves_the_view_to_the_game_when_aim_is_not_allowed) {
    struct Case { uint32_t off, val; const char* why; };
    const Case cases[] = {{0x4e0, 1, "locked on"}, {0x4d4, 2, "morph ball"}, {0x4d0, 1, "not first person"}, {0x4d4, 0, "cinematic"}};
    for (const Case& cs : cases) {
        reset_world();
        if (cs.off == 0x4d4 && cs.val == 0) put<uint32_t>(g_w.cam, 0x08, 1);     // a cinematic camera is active
        else put<uint32_t>(g_w.player, cs.off, cs.val);
        const float before_pitch = 0.25f;
        put<float>(g_w.player, 0x5cc, before_pitch);
        frame(500, -500);
        CHECK_MSG(g_w.original_calls == 1, cs.why);          // the game's function still runs
        CHECK_MSG(g_w.set_transform_calls == 0, cs.why);     // no yaw
        CHECK_MSG(std::fabs(yaw()) < 1e-9, cs.why);
        // the game's stand-in levelled the pitch to 0 and the hook must not have put anything back
        CHECK_MSG(std::fabs(pitch()) < 1e-9, cs.why);
    }
}

NXTEST(hook_does_not_replay_counts_collected_while_locked_on) {
    reset_world();
    put<uint32_t>(g_w.player, 0x4e0, 1);                     // locked on
    for (int i = 0; i < 20; ++i) frame(300, 0);              // the player flicks the mouse while locked
    put<uint32_t>(g_w.player, 0x4e0, 0);                     // lock released
    frame(0, 0);
    CHECK(std::fabs(yaw()) < 1e-9);                          // nothing is applied afterwards
    frame(10, 0);
    CHECK(std::fabs(yaw() - (-10 * kYaw)) < 1e-6);           // and aiming resumes from where the view is
}

NXTEST(hook_resumes_from_the_games_pitch_after_a_lock) {
    reset_world();
    frame(0, -100);
    put<uint32_t>(g_w.player, 0x4e0, 1);
    frame(0, 0);
    put<float>(g_w.player, 0x5cc, 0.05f);                    // the game moved the view while it was in control
    put<uint32_t>(g_w.player, 0x4e0, 0);
    frame(0, 0);
    CHECK(std::fabs(pitch() - 0.05) < 1e-6);                 // adopted, not the old mouse pitch
}

NXTEST(hook_counts_show_up_in_the_stats) {
    reset_world();
    const play::Stats a = play::stats();
    frame(30, -40);
    const play::Stats b = play::stats();
    CHECK_EQ(b.frames - a.frames, 1u);
    CHECK_EQ(b.process_input_calls - a.process_input_calls, 1u);
    CHECK_EQ(b.aim_frames - a.aim_frames, 1u);
    CHECK_EQ(b.mouse_counts_applied - a.mouse_counts_applied, 70u);
    CHECK(!b.pump_thread_mismatch);
}

NXTEST(hook_stop_removes_the_hooks) {
    reset_world();
    play::stop();
    ::Ctx c{};
    c.x[0] = addr(g_w.player);
    c.x[2] = addr(g_w.mgr);
    CHECK_EQ(nx_hook_entry(&c, kBase + kProcessInput), 0);    // not handled: the lifted function would run by itself
    CHECK_EQ(g_w.original_calls, 0);
}
