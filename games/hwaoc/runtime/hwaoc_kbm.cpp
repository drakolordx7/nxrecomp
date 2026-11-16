// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode, the Age of Calamity side, input half (play_game.h GamePlay; the table is in hwaoc_play.cpp): the mouse turns the battle camera, and the camera's activity tells play mode the game
// is in battle (the Play context; everything else is Menu).
//
// The camera (three update functions, one per camera mode: the battle camera sub_FAA1C0 and two others) asks two small functions for its turn this
// update: kCamYawRate and kCamPitchRate read the right stick from the game's pad state, apply the dead zone and the game's invert option, and return
// an angle in radians (the camera caps its move by angle / (pi/180), its pitch limits compare against the same angle). A mid hook right after each
// call adds the mouse counts gathered since the last camera update, times [mouse] yaw / pitch, to that angle. So the mouse goes into the camera's own
// turn: no stick emulation, no dead zone, no rate cap, and the stick still adds its own turn on top. The game clamps the pitch itself.
// Signs (from the code): stick right gives a negative yaw angle; a positive pitch angle raises the camera above the target (it looks down more).
// Mouse right turns right and mouse down looks down, unless [mouse] invert_x / invert_y.
#include "hwaoc_game.h"
#include "hwaoc_addrs.h"

#include <atomic>
#include <cstdio>
#include <chrono>
#include <thread>

#include "hwaoc_kbm.h"
#include "nxrt/game/play_game.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"

namespace nxrt::play {

namespace {

namespace addr = nxrt::games::hwaoc::addr;

struct Site {
    uint64_t off;
    bool yaw;
    int pad_reg;      // the callee-saved register that still holds the pad index the rate was asked for (1 = the second player)
};
// The returns of kCamYawRate / kCamPitchRate in the three camera updates (addrs.tsv, M7 section).
const Site kSites[] = {
    {addr::kCamYawRet0, true, 20},   {addr::kCamPitchRet0, false, 20},
    {addr::kCamYawRet1, true, 21},   {addr::kCamPitchRet1, false, 21},
    {addr::kCamYawRet2, true, 21},   {addr::kCamPitchRet2, false, 21},
};
constexpr size_t kSiteCount = sizeof kSites / sizeof kSites[0];

struct H {
    GameHost* host = nullptr;
    uint32_t hooks[kSiteCount] = {};
    double pend_x = 0, pend_y = 0;           // counts taken from the host and not yet given to a camera update (guest main thread)
    uint32_t wait_hook = 0, frame_hook = 0;
    std::thread::id main_id;                 // the guest main thread: the one that installed us and owns the window
    std::atomic<uint64_t> main_ticks{0};
    std::atomic<uint64_t> cam_calls{0};
    std::atomic<uint64_t> applied{0};
    double sum_yaw = 0, sum_pitch = 0;       // radians added so far (status line)
};
H& h() {
    static H s;
    return s;
}

int cam_rate_hook(::Ctx* c, uint64_t, void* user) {
    H& st = h();
    if (!st.host) return 0;
    const Site& site = *static_cast<const Site*>(user);
    if (static_cast<uint32_t>(c->x[site.pad_reg]) == 1) return 0;     // the second player's camera keeps its stick
    st.cam_calls++;
    // The first camera call after a frame boundary takes the counts since then (and marks the game as in battle).
    double dx = 0, dy = 0;
    st.host->input_tick(&dx, &dy);
    st.pend_x += dx;
    st.pend_y += dy;
    const MouseSettings& ms = st.host->settings().mouse;
    float& s0 = c->v[0].f[0];
    if (site.yaw) {
        if (st.pend_x == 0) return 0;
        const double a = -st.pend_x * ms.yaw_rad * (ms.invert_x ? -1.0 : 1.0);
        s0 += static_cast<float>(a);
        st.sum_yaw += a;
        st.host->aim_applied(st.pend_x, 0);
        st.pend_x = 0;
    } else {
        if (st.pend_y == 0) return 0;
        const double a = st.pend_y * ms.pitch_rad * (ms.invert_y ? -1.0 : 1.0);
        s0 += static_cast<float>(a);
        st.sum_pitch += a;
        st.host->aim_applied(0, st.pend_y);
        st.pend_y = 0;
    }
    st.applied++;
    return 0;
}

// Two sites that run on the guest main thread: the job wait loop the game boots and loads in (after its YieldThread call), and the per-frame wait of the
// render-submit loop (sub_17C9800) the main thread sits in once the game runs. The frame update (the frame boundary of play mode) is a job on a worker thread,
// and the window belongs to the main thread: the main thread's passes pump the window and run what the frame boundary queued for it (GameHost::main_tick).
// Passes on other threads, and passes closer than 2 ms to the last one, return at once.
int main_wait_hook(::Ctx*, uint64_t, void*) {
    H& st = h();
    if (!st.host || std::this_thread::get_id() != st.main_id) return 0;
    static uint64_t last_ns = 0;
    const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    if (now - last_ns < 2000000) return 0;
    last_ns = now;
    st.main_ticks++;
    st.host->main_tick();
    return 0;
}

void install(uint64_t base, GameHost* host, const Settings&) {
    H& st = h();
    st.host = host;
    st.main_id = std::this_thread::get_id();
    st.wait_hook = hook_add_mid(base + addr::kMainYield, &main_wait_hook, nullptr);
    st.frame_hook = hook_add_mid(base + addr::kMainRender, &main_wait_hook, nullptr);
    for (size_t i = 0; i < kSiteCount; ++i) st.hooks[i] = hook_add_mid(base + kSites[i].off, &cam_rate_hook, const_cast<Site*>(&kSites[i]));
    NXLOG_I("hwaoc", "mouse camera: %zu hooks on the camera's stick-rate returns", kSiteCount);
}

void remove() {
    H& st = h();
    for (uint32_t& id : st.hooks) {
        if (id) hook_remove(id);
        id = 0;
    }
    for (uint32_t* id : {&st.wait_hook, &st.frame_hook}) {
        if (*id) hook_remove(*id);
        *id = 0;
    }
    st.host = nullptr;
}

void frame(const GameFrameIn& in, const GameHost&, GameFrame* out) {
    out->ctx = in.in_game ? Ctx::Play : Ctx::Menu;
}

void status(GameStatus* out) { out->aim = h().cam_calls.load() ? "camera" : "no camera update yet"; }

void status_text(char* buf, size_t n) {
    H& st = h();
    std::snprintf(buf, n, " cam(calls=%llu applied=%llu yaw=%.3f pitch=%.3f) main_ticks=%llu", static_cast<unsigned long long>(st.cam_calls.load()),
                  static_cast<unsigned long long>(st.applied.load()), st.sum_yaw, st.sum_pitch, static_cast<unsigned long long>(st.main_ticks.load()));
}

}  // namespace

void kbm_install(uint64_t base, GameHost* host, const Settings& s) { install(base, host, s); }
void kbm_remove() { remove(); }
void kbm_frame(const GameFrameIn& in, const GameHost& host, GameFrame* out) { frame(in, host, out); }
void kbm_status(GameStatus* out) { status(out); }
void kbm_status_text(char* buf, size_t n) { status_text(buf, n); }

}  // namespace nxrt::play
