// SPDX-License-Identifier: GPL-3.0-or-later
// Start-up and menu tweaks (S46, games/mpr/mpr_fastboot.cpp), hook level: the entry hooks driven the way lifted code drives them
// (nx_hook_entry on the guest address) against stand-in guest functions and a fake game state in host memory, at a base of its own.
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "test_framework.h"

#include "nxrt/abi.h"
#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/tt_script.h"
#include "mpr/mpr_fastboot.h"
#include "play/play_types.h"

NXTEST_GROUP(play_fastboot_hooks)

namespace {

namespace play = nxrt::play;

constexpr uint64_t kBase = 0x5C00000000ull;
constexpr uint64_t kPressed = 0x4d71f0, kRetUserInput = 0xe51e78, kRetCamera = 0x4bd840, kSkippable = 0x40a2dc;
constexpr uint64_t kMovieThink = 0x852b28, kMovieSkip = 0x8536d4, kMoviePlaying = 0x41fe48, kInvalid = 0x1d15334;
constexpr uint64_t kFeTick = 0x41a21c, kUpdateSm = 0x41b000;

struct Fake {
    std::vector<uint8_t> gs = std::vector<uint8_t>(0x40), worlds = std::vector<uint8_t>(0x50), movie = std::vector<uint8_t>(0x100), parm = std::vector<uint8_t>(0x20);
    float dt_seen = 0;
    int update_calls = 0, skip_calls = 0;
    uint64_t skip_self = 0, skip_immediate = 0;
    bool ready = false;
};
Fake g_f;

uint64_t addr(const std::vector<uint8_t>& b) { return reinterpret_cast<uint64_t>(b.data()); }
void commit(uint64_t a, size_t n) { VirtualAlloc(reinterpret_cast<void*>(a), n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }

void set_area(const char* world, const char* area) {
    uint8_t w[16], a[16];
    nxrt::tt::tt_guid_to_bytes(world, w);
    nxrt::tt::tt_guid_to_bytes(area, a);
    std::memcpy(g_f.gs.data(), w, 16);
    int32_t n = 1;
    std::memcpy(g_f.gs.data() + 0x10, &n, 4);
    const uint64_t arr = addr(g_f.worlds);
    std::memcpy(g_f.gs.data() + 0x18, &arr, 8);
    std::memcpy(g_f.worlds.data(), w, 16);
    std::memcpy(g_f.worlds.data() + 0x10, a, 16);
}

// UpdateStateManager as lifted code has it: the entry hook first, then the body (records the step it would hand to CStateManager::Update)
void fake_update_sm(::Ctx* c) {
    if (nx_hook_entry(c, kBase + kUpdateSm)) return;
    g_f.update_calls++;
    std::memcpy(&g_f.dt_seen, reinterpret_cast<const void*>(c->x[1] + 8), 4);
}
// CFrontEndIOWin::TimerTick's body: its own state manager's update (CStateManagerIOWin::TimerTick)
void fake_fe_tick(::Ctx* c) { nxrt::dispatch_call(c, kBase + kUpdateSm); }
void fake_playing(::Ctx* c) { c->x[0] = 1; }
void fake_skip(::Ctx* c) {
    g_f.skip_calls++;
    g_f.skip_self = c->x[0];
    g_f.skip_immediate = c->x[2];
}

void setup(const play::FastbootSettings& f) {
    if (!g_f.ready) {
        g_f.ready = true;
        commit(kBase + 0x459c000, 0x2000);
        commit(kBase + 0x1d15000, 0x1000);
        *reinterpret_cast<uint64_t*>(kBase + 0x459cc70) = addr(g_f.gs);
        *reinterpret_cast<uint32_t*>(kBase + kInvalid) = 0xffffffffu;
        nxrt::fn_register(kBase + kUpdateSm, &fake_update_sm);
        nxrt::fn_register(kBase + kFeTick, &fake_fe_tick);
        nxrt::fn_register(kBase + kMoviePlaying, &fake_playing);
        nxrt::fn_register(kBase + kMovieSkip, &fake_skip);
        play::fastboot_install(kBase);
    }
    play::Settings s;
    s.fastboot = f;
    play::fastboot_set(s);
}

::Ctx ctx() {
    ::Ctx c;
    std::memset(&c, 0, sizeof c);
    return c;
}

const char* const kFrigate = "4b5e8635-0f43-4ffb-9427-2e62186e517d";
const char* const kHangar = "a8444ff6-e4a4-4183-bfec-73d6d6407fa5";
const char* const kOtherRoom = "11111111-2222-3333-4444-555555555555";
const char* const kTitleIntro = "45d4dd7e-45af-40f6-b6ed-8b76c33c90fb";

}  // namespace

NXTEST(ids_match_the_region_table) {
    uint8_t w[16], a[16], m[16];
    REQUIRE(nxrt::tt::tt_guid_to_bytes(kFrigate, w));
    REQUIRE(nxrt::tt::tt_guid_to_bytes(kHangar, a));
    REQUIRE(nxrt::tt::tt_guid_to_bytes(kTitleIntro, m));
    CHECK(play::fastboot_is_intro_area(w, a));
    CHECK(!play::fastboot_is_intro_area(a, w));
    CHECK(play::fastboot_is_title_intro_movie(m));
    m[15] ^= 1;
    CHECK(!play::fastboot_is_title_intro_movie(m));
}

NXTEST(skip_press_only_at_the_two_skip_sites_in_the_hangar) {
    play::FastbootSettings f;
    f.skip_intro = true;
    setup(f);
    set_area(kFrigate, kHangar);
    ::Ctx c = ctx();
    c.x[30] = kBase + kRetUserInput;
    CHECK_EQ(nx_hook_entry(&c, kBase + kPressed), 1);
    CHECK_EQ(c.x[0], 1ull);
    c = ctx();
    c.x[30] = kBase + kRetCamera;
    CHECK_EQ(nx_hook_entry(&c, kBase + kPressed), 1);
    c = ctx();
    c.x[30] = kBase + 0x123456;      // any other reader of a pressed button: untouched
    CHECK_EQ(nx_hook_entry(&c, kBase + kPressed), 0);
    uint8_t guid[16] = {1, 2, 3};
    c = ctx();
    c.x[1] = reinterpret_cast<uint64_t>(guid);
    CHECK_EQ(nx_hook_entry(&c, kBase + kSkippable), 1);
    CHECK_EQ(c.x[0], 1ull);
    CHECK(play::fastboot_stats().skip_presses >= 2);

    set_area(kFrigate, kOtherRoom);  // another room: the game's own answer
    c = ctx();
    c.x[30] = kBase + kRetUserInput;
    CHECK_EQ(nx_hook_entry(&c, kBase + kPressed), 0);
    c = ctx();
    c.x[1] = reinterpret_cast<uint64_t>(guid);
    CHECK_EQ(nx_hook_entry(&c, kBase + kSkippable), 0);

    set_area(kFrigate, kHangar);
    f.skip_intro = false;            // off: vanilla everywhere
    setup(f);
    c = ctx();
    c.x[30] = kBase + kRetUserInput;
    CHECK_EQ(nx_hook_entry(&c, kBase + kPressed), 0);
    c = ctx();
    c.x[1] = reinterpret_cast<uint64_t>(guid);
    CHECK_EQ(nx_hook_entry(&c, kBase + kSkippable), 0);
}

NXTEST(fast_menus_scales_only_the_front_end_step) {
    play::FastbootSettings f;
    f.fast_menus = true;
    f.menu_speed = 3;
    setup(f);
    const float dt = 1.0f / 60.0f;
    std::memcpy(g_f.parm.data() + 8, &dt, 4);
    ::Ctx c = ctx();
    c.x[1] = addr(g_f.parm);
    const int before = g_f.update_calls;
    CHECK_EQ(nx_hook_entry(&c, kBase + kFeTick), 1);     // handled: the tick ran inside the hook
    CHECK_EQ(g_f.update_calls, before + 1);
    CHECK(std::fabs(g_f.dt_seen - 3.0f * dt) < 1e-6f);
    float after = 0;
    std::memcpy(&after, g_f.parm.data() + 8, 4);
    CHECK_EQ(after, dt);                                 // the message keeps its own step for the other windows

    c = ctx();                                           // a state manager updated outside the front end's tick: unchanged
    c.x[1] = addr(g_f.parm);
    nxrt::dispatch_call(&c, kBase + kUpdateSm);
    CHECK_EQ(g_f.dt_seen, dt);

    const float big = 0.05f;                             // a long step is capped at 0.1 s
    std::memcpy(g_f.parm.data() + 8, &big, 4);
    c = ctx();
    c.x[1] = addr(g_f.parm);
    nx_hook_entry(&c, kBase + kFeTick);
    CHECK(std::fabs(g_f.dt_seen - 0.1f) < 1e-6f);

    f.fast_menus = false;                                // off: the front end's tick is not touched
    setup(f);
    std::memcpy(g_f.parm.data() + 8, &dt, 4);
    c = ctx();
    c.x[1] = addr(g_f.parm);
    CHECK_EQ(nx_hook_entry(&c, kBase + kFeTick), 0);
}

NXTEST(title_intro_movie_skipped_once) {
    play::FastbootSettings f;
    f.skip_boot_logos = true;
    setup(f);
    uint8_t id[16];
    REQUIRE(nxrt::tt::tt_guid_to_bytes(kTitleIntro, id));
    std::memcpy(g_f.movie.data() + 128, id, 16);
    const uint32_t handle = 5, state = 0;
    std::memcpy(g_f.movie.data() + 160, &handle, 4);
    std::memcpy(g_f.movie.data() + 144, &state, 4);
    ::Ctx c = ctx();
    c.x[0] = addr(g_f.movie);
    c.x[1] = 0x1234;
    CHECK_EQ(nx_hook_entry(&c, kBase + kMovieThink), 0);   // the movie's own Think still runs
    CHECK_EQ(g_f.skip_calls, 1);
    CHECK_EQ(g_f.skip_self, addr(g_f.movie));
    CHECK_EQ(g_f.skip_immediate, 1ull);
    CHECK_EQ(c.x[0], addr(g_f.movie));                   // the registers are as they were, for the Think
    CHECK_EQ(c.x[1], 0x1234ull);
    nx_hook_entry(&c, kBase + kMovieThink);
    CHECK_EQ(g_f.skip_calls, 1);                         // once per movie

    id[0] ^= 1;                                          // another movie (the title loop, attract clips): never
    std::memcpy(g_f.movie.data() + 128, id, 16);
    const uint32_t h2 = 6;
    std::memcpy(g_f.movie.data() + 160, &h2, 4);
    nx_hook_entry(&c, kBase + kMovieThink);
    CHECK_EQ(g_f.skip_calls, 1);

    f.skip_boot_logos = false;
    setup(f);
}
