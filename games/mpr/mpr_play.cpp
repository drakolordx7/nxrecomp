// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode, the Metroid Prime Remastered side (play_game.h GamePlay): raw mouse aim on CPlayerMP1::ProcessInput, camera look on the camera Think
// functions, free lock-on and the decoupled gun (play_lockcam.h), the display hooks (display_hooks.h), the tone curve (mpr_tonemap.h), the menu
// pointer on the game's own UI (menu_mouse_game.h), the glow colour (play_glow.h), the start-up and menu tweaks (mpr_fastboot.h), the suit and
// arm cannon paint (mpr_tint.h), and what the player state says about the context and the beams.
//
// The hook bodies are the ones play.cpp had before the game boundary (docs/GAME_BOUNDARY.md); what they took from play mode's state now comes
// through GameHost.
#include "mpr_game.h"
#include "mpr_sites.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#include "display_hooks.h"
#include "menu_mouse_game.h"
#include "mpr_fastboot.h"
#include "mpr_tint.h"
#include "mpr_tonemap.h"
#include "nxrt/dispatch.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "play_aim.h"
#include "play_camlook.h"
#include "play_game.h"
#include "play_glow.h"
#include "play_lockcam.h"
#include "play_scenefix.h"
#include "play_lookrate.h"

namespace nxrt::play {

namespace {

constexpr const char* kTag = "play";
constexpr uint64_t kFnProcessInput = 0xc70334;    // CPlayerMP1::ProcessInput(CFinalInput const&, CStateManager&)
constexpr uint64_t kFnSetTransform = 0xc4d588;    // CActorMP1::SetTransform(CTransform4f const&): x0 = the actor, x1 = the transform
constexpr uint64_t kFnFpCamThink = 0xd72314;      // CFirstPersonCameraMP1::Think(float, CStateManager&): x0 the camera
constexpr uint64_t kFnBallCamThink = 0xcff7ec;    // CBallCameraMP1::Think(float, CStateManager&): x0 the camera

struct M {
    GameHost* host = nullptr;
    uint64_t base = 0x7100000000ull;
    std::unique_ptr<AimDriver> aim;
    std::atomic<uint64_t> player{0};
    LookOffsets free_look, ball_look;     // free look (first person, Alt held) and the morph ball camera orbit; main thread only
    uint32_t hook_input = 0, hook_fpcam = 0, hook_ballcam = 0;
};

M& m() {
    static M inst;
    return inst;
}

uint64_t now_ns() { return FrameLimiter::now_ns(); }

// Calls the game's own SetTransform(player, xf) from inside the hook.
struct XfCall { ::Ctx* c; };
void guest_set_transform(void* user, uint64_t player, const float xf[12]) {
    M& st = m();
    ::Ctx* c = static_cast<XfCall*>(user)->c;
    alignas(16) float copy[12];
    std::memcpy(copy, xf, sizeof copy);
    ::Ctx saved = *c;
    c->x[0] = player;
    c->x[1] = reinterpret_cast<uint64_t>(copy);
    dispatch_call(c, st.base + kFnSetTransform);
    *c = saved;
}

thread_local bool t_in_process_input = false;

int process_input_hook(::Ctx* c, uint64_t addr, void*) {
    if (t_in_process_input) return 0;                 // the nested call that runs the game's own function
    M& st = m();
    GameHost& host = *st.host;
    const uint64_t now = now_ns();
    const uint64_t player = c->x[0], mgr = c->x[2];
    st.player.store(player);
    glow_player_tick(c, player);   // S44: the ball's inner glow colour (restores *c)

    // The first ProcessInput since the frame boundary takes the mouse counts (late sample: the host pumps the window's events once more first).
    double dx = 0, dy = 0;
    host.input_tick(&dx, &dy);
    // Camera look instead of body aim: the morph ball camera takes the mouse in the ball; the free look key makes the first-person mouse turn the
    // view and not Samus. The counts are consumed here (the aim gets none), and the camera Think hooks below apply the offsets.
    {
        const AimBlock gate = st.aim->gate(player, mgr);
        const MouseSettings& ms = host.settings().mouse;
        uint32_t morph_word = 0;                       // 0 unmorphed, 2 and 3 transitions, 1 in the ball (the gate stops at "not first person" first)
        guest_read(player + 0x4d4, &morph_word, sizeof morph_word);
        if (morph_word == 1) {
            LookParams lp;
            lp.yaw_rad = ms.alt_yaw_rad;
            lp.pitch_rad = ms.alt_pitch_rad;
            lp.invert_x = ms.invert_x;
            lp.invert_y = ms.invert_y;
            if (dx != 0 || dy != 0) st.ball_look.add(dx, dy, lp);
            dx = dy = 0;
            // the right stick orbits the camera too, at a speed in radians per second (the game gives the stick no job in the ball, so nothing else moves with it)
            float sx = 0, sy = 0, dt_in = 0;
            if (host.settings().pad.alt_orbit > 0 && guest_read(c->x[1], &dt_in, sizeof dt_in) && dt_in > 1e-4f && dt_in < 0.25f) {
                host.right_stick(&sx, &sy);
                if (sx != 0 || sy != 0) {
                    const double rate = host.settings().pad.alt_orbit * static_cast<double>(dt_in);
                    st.ball_look.add_angles(-static_cast<double>(sx) * rate, static_cast<double>(sy) * rate, lp);
                }
            }
        } else {
            st.ball_look.reset();
        }
        if (gate == AimBlock::None && host.action_active(Act::LookHold, now)) {
            LookParams lp;
            lp.yaw_rad = ms.yaw_rad;
            lp.pitch_rad = ms.pitch_rad;
            lp.invert_x = ms.invert_x;
            lp.invert_y = ms.invert_y;
            if (dx != 0 || dy != 0) st.free_look.add(dx, dy, lp);
            dx = dy = 0;
        } else {
            st.free_look.reset();
        }
    }
    AimParams p;
    {
        const MouseSettings& ms = host.settings().mouse;
        p.yaw_rad = ms.yaw_rad;
        p.pitch_rad = ms.pitch_rad;
        p.invert_x = ms.invert_x;
        p.invert_y = ms.invert_y;
        p.pitch_limit_rad = host.pitch_limit_deg() * kPi / 180.0;
        // The pad is the look device while the stick or the gyro moves the pitch, and while the game's gyro camera or Pointer scheme is on: the game then sets the
        // pitch target from the pad's orientation, and the aim must not write its own angle over it.
        p.pad_look = host.has_input() && (host.pad_look(host.settings().pad.gyro) || lockcam_gyro_freelook(c, mgr));
    }
    XfCall xc{c};
    st.aim->set_transform_fn(&guest_set_transform, &xc);
    const bool applied = st.aim->apply(player, mgr, dx, dy, p, [&] {
        ::Ctx saved = *c;
        t_in_process_input = true;
        dispatch_call(c, addr);
        t_in_process_input = false;
        *c = saved;
    });
    if (host.log_status() && (dx != 0 || dy != 0))
        NXLOG_I(kTag, "aim: dx=%.1f dy=%.1f block=%s applied=%d pitch=%.4f", dx, dy, aim_block_name(st.aim->last_block()), applied ? 1 : 0, st.aim->pitch());
    if (applied) host.aim_applied(dx, dy);
    return 1;
}

// Camera Think hooks: after the game's own Think the camera transform is turned by the current look offset (free look: about the eye; morph ball:
// about the ball). Before the next Think the game's own transform is put back, so its camera logic never sees the offset.
struct CamMemo { bool have = false; float orig[12] = {}, applied[12] = {}; uint64_t cam = 0; };
thread_local bool t_in_cam_think = false;
CamMemo g_cam_memo[2];

bool read_xf(uint64_t actor, uint64_t* xfp, float xf[12]) {
    *xfp = 0;
    uint64_t a = 0;
    if (!guest_read(actor + 8, &a, 8) || !a || !guest_read(a + 0x20, &a, 8) || !a || !guest_read(a + 0x40, &a, 8) || !a) return false;
    *xfp = a;
    return guest_read(a, xf, 12 * sizeof(float));
}

int cam_think_hook(::Ctx* c, uint64_t addr, void* user) {
    if (t_in_cam_think) return 0;
    M& st = m();
    const bool ball = user != nullptr;
    LookOffsets& off = ball ? st.ball_look : st.free_look;
    CamMemo& memo = g_cam_memo[ball ? 1 : 0];
    const uint64_t cam = c->x[0];
    XfCall xc{c};
    uint64_t xfp = 0;
    float cur[12];
    if (memo.have && memo.cam == cam && read_xf(cam, &xfp, cur) && std::memcmp(cur, memo.applied, sizeof cur) == 0) guest_set_transform(&xc, cam, memo.orig);
    memo.have = false;
    {
        ::Ctx saved = *c;
        t_in_cam_think = true;
        dispatch_call(c, addr);
        t_in_cam_think = false;
        *c = saved;
    }
    if (!read_xf(cam, &xfp, cur)) return 1;
    // Decoupled gun ([camera] gun_drives_view = 0): the camera keeps its own transform without the camera bob (CFirstPersonCameraMP1 keeps that one at +660),
    // and the difference goes to the gun (play_lockcam.cpp), so the bob moves the gun on screen and not the view.
    float work[12];
    std::memcpy(work, cur, sizeof work);
    bool changed = false;
    if (!ball && lockcam_gun_decoupled()) {
        float steady[12];
        if (guest_read(cam + st.aim->layout().fp_base_xf, steady, sizeof steady)) {
            bool finite = true;
            for (float f : steady) finite = finite && std::isfinite(f);
            if (finite) {
                lockcam_set_gun_delta(cur, steady);
                std::memcpy(work, steady, sizeof work);
                changed = true;
            }
        }
    } else if (!ball) {
        lockcam_clear_gun_delta();
    }
    if (off.active()) {
        float pivot[3] = {work[3], work[7], work[11]};          // free look: the eye itself
        if (ball) {
            float pxf[12];
            uint64_t pp = 0;
            const uint64_t player = st.player.load();
            if (!player || !read_xf(player, &pp, pxf)) return 1;
            pivot[0] = pxf[3]; pivot[1] = pxf[7]; pivot[2] = pxf[11];
        }
        float out[12];
        orbit_transform(work, pivot, off.yaw(), off.pitch(), out);
        std::memcpy(work, out, sizeof work);
        changed = true;
    }
    if (!changed) return 1;
    std::memcpy(memo.orig, cur, sizeof cur);
    guest_set_transform(&xc, cam, work);
    if (read_xf(cam, &xfp, memo.applied)) { memo.have = true; memo.cam = cam; }
    return 1;
}

// ---- the GamePlay entries ---------------------------------------------------------------------------------------------------------------
void install_render(uint64_t base, const DisplaySettings& d, int window_w, int window_h) {
    M& st = m();
    st.base = base;
    st.aim = std::make_unique<AimDriver>(base);     // a fresh one per play-mode start, as the old start() made
    display_install(base, d, window_w, window_h);
    extras::tonemap_hook_install(base);
}

void install(uint64_t base, GameHost* host, const Settings& s) {
    M& st = m();
    st.base = base;
    st.host = host;
    if (!st.aim) st.aim = std::make_unique<AimDriver>(base);
    st.hook_input = hook_add_entry(base + kFnProcessInput, &process_input_hook);
    st.hook_fpcam = hook_add_entry(base + kFnFpCamThink, &cam_think_hook, nullptr);
    st.hook_ballcam = hook_add_entry(base + kFnBallCamThink, &cam_think_hook, &st);
    lockcam_install(base, st.aim.get());
    scenefix_install(base);
    lockcam_set(s);
    glow_install(base);   // after lockcam: both hook CPlayerGunMP1::Update
    glow_set(s.tweaks);
    mm::game_install(base);
    fastboot_install(base);
    fastboot_set(s);
    tint_install();
    tint_set(s.tweaks);
}

void remove() {
    M& st = m();
    tint_remove();
    fastboot_remove();
    mm::game_remove();
    glow_remove();
    lockcam_remove();
    scenefix_remove();
    if (st.hook_input) hook_remove(st.hook_input);
    if (st.hook_fpcam) hook_remove(st.hook_fpcam);
    if (st.hook_ballcam) hook_remove(st.hook_ballcam);
    st.hook_input = st.hook_fpcam = st.hook_ballcam = 0;
    display_remove();
}

void settings_changed(const Settings& s) {
    display_apply(s.display);
    lockcam_set(s);
    fastboot_set(s);
    glow_set(s.tweaks);
    tint_set(s.tweaks);
}

void frame_begin() {
    lockcam_frame();
    glow_frame();
    tint_frame();
}

void frame(bool in_game, double step, GameFrame* out) {
    M& st = m();
    // The gyro's rate is added to the pitch target once per call, with no time step (play_lookrate.h): the game gets the rate times (step * 60), so a step of
    // 1/120 s adds half as much, twice as often. Menus and the map get the gyro as it is. The stick needs no such thing: its pitch code runs once per 1/60 s
    // (the hook on ComputeFreeLook).
    out->look_scale = in_game ? static_cast<float>(look_rate_scale(step)) : 1.f;
    if (!st.aim) return;
    const GameHints hints = st.aim->hints(st.player.load());
    if (hints.valid) {
        static const int game_to_mine[4] = {0, 2, 1, 3};      // game: power ice wave plasma; bindings: power wave ice plasma
        unsigned mask = 0;
        for (int i = 0; i < 4; ++i)
            if (hints.owned_beams & (1u << i)) mask |= 1u << game_to_mine[i];
        out->hints_valid = true;
        out->cycle_index = hints.beam >= 0 ? game_to_mine[hints.beam] : -1;     // set A = the beams
        out->cycle_mask = mask;
    }
    if (hints.morphed) out->play_ctx = Ctx::Alt;
    else if (hints.visor == 2) out->play_ctx = Ctx::Inspect;
    else out->play_ctx = Ctx::Play;
}

Act menu_frame(bool menu_up, const mm::Geometry& g) { return mm::game_frame(menu_up, g); }
bool menu_over_value() { return mm::game_over_value(); }
bool menu_active() { return mm::game_snapshot().active; }

void status(GameStatus* out) {
    M& st = m();
    if (!st.aim) return;
    out->aim = aim_block_name(st.aim->last_block());
    out->pitch_rad = st.aim->pitch();
}

// " display_hook_calls=... menumouse(...)": the part of the status line the base printed for this game before the boundary.
void status_text(char* buf, size_t n) {
    std::snprintf(buf, n, " display_hook_calls=%u menumouse(active=%d hovers=%llu clicks=%llu widget_calls=%llu hook_calls=%llu)", display_hook_calls(),
                  mm::game_snapshot().active ? 1 : 0, static_cast<unsigned long long>(mm::game_stats().hovers), static_cast<unsigned long long>(mm::game_stats().clicks),
                  static_cast<unsigned long long>(mm::game_stats().widget_calls), static_cast<unsigned long long>(mm::game_hook_calls()));
}

}  // namespace

bool mpr_mouse_tab(int where, Settings& edit);     // mpr_ui.cpp

const GamePlay& mpr_game_play() {
    static const GamePlay t = [] {
        GamePlay p;
        p.frame_update = games::mpr::site::kUpdate;     // CGameArchitectureSupport::Update(float*): once per rendered frame
        p.install_render = &install_render;
        p.install = &install;
        p.remove = &remove;
        p.settings_changed = &settings_changed;
        p.frame_begin = &frame_begin;
        p.frame = &frame;
        p.display_live = &display_live;
        p.display_hook_calls = &display_hook_calls;
        p.menu_frame = &menu_frame;
        p.menu_over_value = &menu_over_value;
        p.menu_active = &menu_active;
        p.status = &status;
        p.status_text = &status_text;
        p.mouse_tab = &mpr_mouse_tab;
        p.tweaks_tab = &glow_tab;                      // play_glow_ui.cpp
        return p;
    }();
    return t;
}

}  // namespace nxrt::play
