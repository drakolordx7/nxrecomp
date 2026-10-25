// SPDX-License-Identifier: GPL-3.0-or-later
// Start-up and menu tweaks of Metroid Prime Remastered (S46), see mpr_fastboot.h for the design and docs/S46.md for what was found in the game.
#include "mpr_fastboot.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "nxrt/abi.h"
#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "play/play_aim.h"
#include "play/play_types.h"

namespace nxrt::play {

namespace {

constexpr const char* kTag = "fastboot";

// main-module offsets (Metroid Prime Remastered 1.0.0; names from the game's symbols)
constexpr uint64_t kFnDigitalPressed = 0x4d71f0;        // CCommandFrame::GetDigitalPressedInput(CDataEnumValue const&, CFinalInput const&, EFilterMode) const: bool in w0
constexpr uint64_t kRetSkipUserInput = 0xe51e78;        // return address of its call for the skip command (1) in CGameIOWinMP1::UserInput
constexpr uint64_t kRetSkipCameraManager = 0x4bd840;    // ... and in CCameraManager::CheckForCinematicSkip(CStateManager&, CFinalInput const&)
constexpr uint64_t kFnIsCinematicSkippable = 0x40a2dc;  // CGameState::IsCinematicSkippable(CGuid const&) const: bool in w0
constexpr uint64_t kFnMovieThink = 0x852b28;            // CStreamedMovieGOC::Think(CStateManager&, float): x0 the movie component, x1 the state manager
constexpr uint64_t kFnMovieSkip = 0x8536d4;             // CStreamedMovieGOC::Skip(CStateManager&, bool immediate)
constexpr uint64_t kFnMovieIsPlaying = 0x41fe48;        // CMovieIOWin::IsMoviePlaying(uint handle)
constexpr uint64_t kMovieInvalidHandle = 0x1d15334;     // CMovieIOWin::skInvalidId (u32)
constexpr uint64_t kMovieId = 128, kMovieHandle = 160, kMovieSkipState = 144;   // CStreamedMovieGOC: the FMV's CObjectId, the CMovieIOWin handle, the skip state (0 = none)
constexpr uint64_t kFnFrontEndTimerTick = 0x41a21c;     // CFrontEndIOWin::TimerTick(CArchMsgParmTimerTick const&, CArchitectureQueue&)
constexpr uint64_t kFnUpdateStateManager = 0x41b000;    // NIOWinStateManagerUtils::UpdateStateManager(CStateManager&, CArchMsgParmTimerTick const&): x1 the parm
constexpr uint64_t kTickDt = 8;                         // CArchMsgParmTimerTick: the step in seconds (float)
constexpr uint64_t kGpGameState = 0x459cc70;            // gpGameState: CGameStateMP1*; +0 world id, +0x10 world-state count, +0x18 world states (0x50 each, +0x10 current area)
constexpr double kMaxMenuStep = 0.1;                    // the front end never gets a step longer than this

// CObjectId bytes as the game keeps them (GUID text with its first three groups stored little-endian, tt_script.cpp tt_guid_to_bytes)
constexpr uint8_t kFrigateWorld[16] = {0x35, 0x86, 0x5e, 0x4b, 0x43, 0x0f, 0xfb, 0x4f, 0x94, 0x27, 0x2e, 0x62, 0x18, 0x6e, 0x51, 0x7d};   // 4b5e8635-0f43-4ffb-9427-2e62186e517d
constexpr uint8_t kDockingHangar[16] = {0xf6, 0x4f, 0x44, 0xa8, 0xa4, 0xe4, 0x83, 0x41, 0xbf, 0xec, 0x73, 0xd6, 0xd6, 0x40, 0x7f, 0xa5};  // a8444ff6-e4a4-4183-bfec-73d6d6407fa5
constexpr uint8_t kTitleIntro[16] = {0x7e, 0xdd, 0xd4, 0x45, 0xaf, 0x45, 0xf6, 0x40, 0xb6, 0xed, 0x8b, 0x76, 0xc3, 0x3c, 0x90, 0xfb};     // 45d4dd7e-45af-40f6-b6ed-8b76c33c90fb

struct State {
    uint64_t base = 0;
    uint32_t ids[8] = {};
    int count = 0;
    std::atomic<bool> skip_intro{false}, skip_logos{false};
    std::atomic<double> menu_rate{1.0};
    std::atomic<uint64_t> presses{0}, skippable{0}, movies{0}, ticks{0};
    std::mutex seen_mu;
    std::vector<std::string> seen_movies;   // FMV ids logged once each
    uint32_t skipped_handle = 0xffffffffu;  // the movie handle already told to skip
    // One opening per new game: the skip is armed when the front end runs (a new game starts from there) and stays on for kSkipWindow after its
    // first press, so a chain of cutscenes in the opening is skipped too but a later cutscene of the same room (a scan, a door) plays as usual.
    std::atomic<bool> armed{true};
    std::atomic<int64_t> first_press_ms{-1};
};
State g;
constexpr int64_t kSkipWindowMs = 10000;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Still within the opening: no press yet, or the first one less than kSkipWindowMs ago.
bool intro_window_open() {
    if (!g.armed.load(std::memory_order_relaxed)) return false;
    const int64_t t0 = g.first_press_ms.load(std::memory_order_relaxed);
    if (t0 < 0 || now_ms() - t0 < kSkipWindowMs) return true;
    g.armed.store(false, std::memory_order_relaxed);
    NXLOG_I(kTag, "skip_intro: the opening is over, later cutscenes play as usual");
    return false;
}

void rearm() {
    if (g.armed.load(std::memory_order_relaxed) && g.first_press_ms.load(std::memory_order_relaxed) < 0) return;
    g.armed.store(true, std::memory_order_relaxed);
    g.first_press_ms.store(-1, std::memory_order_relaxed);
}

thread_local bool t_in_fe = false, t_in_usm = false, t_in_movie = false;

std::string guid_text(const uint8_t b[16]) {
    uint8_t t[16];
    std::memcpy(t, b, 16);
    std::swap(t[0], t[3]); std::swap(t[1], t[2]); std::swap(t[4], t[5]); std::swap(t[6], t[7]);
    static const char d[] = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s.push_back('-');
        s.push_back(d[t[i] >> 4]);
        s.push_back(d[t[i] & 15]);
    }
    return s;
}

// The world and the current area, read the way the test probes read them (mpr_tt_probe.cpp read_state).
bool read_world_area(uint8_t world[16], uint8_t area[16]) {
    uint64_t gs = 0, arr = 0;
    int32_t n = 0;
    if (!guest_read(g.base + kGpGameState, &gs, 8) || !gs) return false;
    if (!guest_read(gs, world, 16) || !guest_read(gs + 0x10, &n, 4) || !guest_read(gs + 0x18, &arr, 8) || n <= 0 || n > 64 || !arr) return false;
    for (int32_t i = 0; i < n; ++i) {
        uint8_t w[16];
        if (guest_read(arr + i * 0x50ull, w, 16) && std::memcmp(w, world, 16) == 0) return guest_read(arr + i * 0x50ull + 0x10, area, 16);
    }
    return false;
}

bool in_intro_area() {
    uint8_t w[16], a[16];
    return read_world_area(w, a) && fastboot_is_intro_area(w, a);
}

uint64_t call(::Ctx* c, uint64_t fn, uint64_t a0, uint64_t a1 = 0, uint64_t a2 = 0) {
    const ::Ctx saved = *c;
    c->x[0] = a0;
    c->x[1] = a1;
    c->x[2] = a2;
    dispatch_call(c, fn);
    const uint64_t r = c->x[0];
    *c = saved;
    return r;
}

// The skip command, read by the two places that start a cutscene skip: "pressed" while the opening cutscene plays.
int pressed_hook(::Ctx* c, uint64_t, void*) {
    if (!g.skip_intro.load(std::memory_order_relaxed)) return 0;
    const uint64_t ret = c->x[30];
    if (ret != g.base + kRetSkipUserInput && ret != g.base + kRetSkipCameraManager) return 0;
    if (!intro_window_open() || !in_intro_area()) return 0;
    if (g.first_press_ms.load(std::memory_order_relaxed) < 0) g.first_press_ms.store(now_ms(), std::memory_order_relaxed);
    if (g.presses++ == 0)
        NXLOG_I(kTag, "skip_intro: the opening cutscene's skip is pressed for the game (%s)", ret == g.base + kRetSkipUserInput ? "CGameIOWinMP1::UserInput" : "CCameraManager::CheckForCinematicSkip");
    c->x[0] = 1;
    return 1;
}

int skippable_hook(::Ctx* c, uint64_t, void*) {
    if (!g.skip_intro.load(std::memory_order_relaxed) || !intro_window_open() || !in_intro_area()) return 0;
    uint8_t id[16] = {};
    guest_read(c->x[1], id, 16);
    if (g.skippable++ < 8) NXLOG_I(kTag, "skip_intro: cutscene %s is skippable (opening cutscene area)", guid_text(id).c_str());
    c->x[0] = 1;
    return 1;
}

int movie_think_hook(::Ctx* c, uint64_t, void*) {
    if (t_in_movie) return 0;
    const uint64_t self = c->x[0];
    uint8_t id[16];
    if (!guest_read(self + kMovieId, id, 16)) return 0;
    {
        const std::string t = guid_text(id);
        std::lock_guard<std::mutex> lk(g.seen_mu);
        if (g.seen_movies.size() < 64 && std::find(g.seen_movies.begin(), g.seen_movies.end(), t) == g.seen_movies.end()) {
            g.seen_movies.push_back(t);
            NXLOG_I(kTag, "streamed movie %s%s", t.c_str(), fastboot_is_title_intro_movie(id) ? " (the title intro)" : "");
        }
    }
    if (!g.skip_logos.load(std::memory_order_relaxed) || !fastboot_is_title_intro_movie(id)) return 0;
    uint32_t handle = 0, invalid = 0, skip_state = 0;
    if (!guest_read(self + kMovieHandle, &handle, 4) || !guest_read(g.base + kMovieInvalidHandle, &invalid, 4) || handle == invalid) return 0;
    if (handle == g.skipped_handle || !guest_read(self + kMovieSkipState, &skip_state, 4) || skip_state != 0) return 0;
    t_in_movie = true;
    const bool playing = (call(c, g.base + kFnMovieIsPlaying, handle) & 1) != 0;
    if (playing) {
        call(c, g.base + kFnMovieSkip, self, c->x[1], 1);     // immediate: the Think below ends it and sends its end messages
        g.skipped_handle = handle;
        g.movies++;
        NXLOG_I(kTag, "skip_boot_logos: the title intro movie is skipped");
    }
    t_in_movie = false;
    return 0;      // the movie's own Think runs now
}

int fe_tick_hook(::Ctx* c, uint64_t addr, void*) {
    // The front end is up and the game is not in the hangar: the next new game gets its opening skipped again. (Whether the front end's window
    // also ticks during play was not established, so the hangar itself never rearms the skip.)
    if (!t_in_fe && g.first_press_ms.load(std::memory_order_relaxed) >= 0 && !in_intro_area()) rearm();
    if (t_in_fe || g.menu_rate.load(std::memory_order_relaxed) <= 1.0) return 0;
    t_in_fe = true;
    dispatch_call(c, addr);
    t_in_fe = false;
    return 1;
}

int update_sm_hook(::Ctx* c, uint64_t addr, void*) {
    if (!t_in_fe || t_in_usm) return 0;
    const double rate = g.menu_rate.load(std::memory_order_relaxed);
    const uint64_t parm = c->x[1];
    float dt = 0.f;
    if (rate <= 1.0 || !guest_read(parm + kTickDt, &dt, 4) || !(dt > 0.f)) return 0;
    const float fast = static_cast<float>(std::min(static_cast<double>(dt) * rate, std::max(kMaxMenuStep, static_cast<double>(dt))));
    if (!guest_write(parm + kTickDt, &fast, 4)) return 0;
    t_in_usm = true;
    dispatch_call(c, addr);
    t_in_usm = false;
    guest_write(parm + kTickDt, &dt, 4);      // the same tick goes on to other windows of the queue unchanged
    if (g.ticks++ == 0) NXLOG_I(kTag, "fast_menus: the front end runs at %.0fx (first tick %.4f s -> %.4f s)", rate, dt, fast);
    return 1;
}

}  // namespace

bool fastboot_is_intro_area(const uint8_t world[16], const uint8_t area[16]) {
    return std::memcmp(world, kFrigateWorld, 16) == 0 && std::memcmp(area, kDockingHangar, 16) == 0;
}

bool fastboot_is_title_intro_movie(const uint8_t id[16]) { return std::memcmp(id, kTitleIntro, 16) == 0; }

void fastboot_install(uint64_t game_base) {
    fastboot_remove();
    g.base = game_base;
    g.skipped_handle = 0xffffffffu;
    g.ids[g.count++] = hook_add_entry(game_base + kFnDigitalPressed, &pressed_hook);
    g.ids[g.count++] = hook_add_entry(game_base + kFnIsCinematicSkippable, &skippable_hook);
    g.ids[g.count++] = hook_add_entry(game_base + kFnMovieThink, &movie_think_hook);
    g.ids[g.count++] = hook_add_entry(game_base + kFnFrontEndTimerTick, &fe_tick_hook);
    g.ids[g.count++] = hook_add_entry(game_base + kFnUpdateStateManager, &update_sm_hook);
}

void fastboot_remove() {
    for (int i = 0; i < g.count; ++i)
        if (g.ids[i]) hook_remove(g.ids[i]);
    g.count = 0;
}

void fastboot_set(const Settings& s) {
    const FastbootSettings& f = s.fastboot;
    const bool was_intro = g.skip_intro.exchange(f.skip_intro), was_logos = g.skip_logos.exchange(f.skip_boot_logos);
    const double was_rate = g.menu_rate.exchange(fastboot_menu_rate(f));
    if (was_intro != f.skip_intro || was_logos != f.skip_boot_logos || was_rate != fastboot_menu_rate(f) || g.base == 0)
        NXLOG_I(kTag, "tweaks: skip_intro %d, skip_boot_logos %d, fast_menus %d (menu speed %.0fx)", f.skip_intro ? 1 : 0, f.skip_boot_logos ? 1 : 0, f.fast_menus ? 1 : 0,
                fastboot_menu_rate(f));
}

FastbootStats fastboot_stats() {
    FastbootStats st;
    st.skip_presses = g.presses.load();
    st.skippable_answers = g.skippable.load();
    st.movies_skipped = g.movies.load();
    st.menu_ticks = g.ticks.load();
    return st;
}

}  // namespace nxrt::play
