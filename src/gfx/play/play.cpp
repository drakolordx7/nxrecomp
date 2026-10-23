// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: the glue. See include/nxrt/play.h for the start-up order and docs/S8.md for the design.
//
// Threads:
//   main (the guest main thread, which created the window)  the Update hook (frame boundary: event pump, input, settings changes) and the
//                                                            ProcessInput hook (raw mouse aim) run here
//   presenting thread (the game's render thread)             FrameLimiter::begin_frame (the pacing wait, and the pre-frame hook that changes the
//                                                            present mode), the ImGui overlay and the settings page
//   hid sampling thread                                      PlaySource::poll (200 Hz): reads the pad snapshot and asks the binding engine for the
//                                                            keyboard and mouse contribution at that instant
#include "nxrt/extras.h"
#include "nxrt/lowlat.h"
#include "extras/tonemap_hook.h"
#include "nxrt/play.h"

#include <windows.h>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "nxrt/ftrace.h"
#include "play_camlook.h"
#include "play_display.h"
#include "nxrt/kernel.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/input.h"
#include "nxrt/input_source.h"
#include "nxrt/log.h"
#include "nxrt/nvn_backend.h"
#include "nxrt/overlay.h"
#include "nxrt/platform.h"
#include "nxrt/runtime.h"
#include "nxrt/tt.h"
#include "play_aim.h"
#include "play_engine.h"
#include "play_selftest.h"
#include "play_types.h"
#include "play_ui.h"
#include "pad_map.h"

namespace nxrt::play {

namespace {

constexpr const char* kTag = "play";
constexpr uint64_t kFnUpdate = 0x3c81c8;          // CGameArchitectureSupport::Update(float*): once per rendered frame
constexpr uint64_t kFnProcessInput = 0xc70334;    // CPlayerMP1::ProcessInput(CFinalInput const&, CStateManager&)
constexpr uint64_t kFnSetTransform = 0xc4d588;    // CActorMP1::SetTransform(CTransform4f const&): x0 = the actor, x1 = the transform
constexpr uint64_t kFnFpCamThink = 0xd72314;      // CFirstPersonCameraMP1::Think(float, CStateManager&): x0 the camera
constexpr uint64_t kFnBallCamThink = 0xcff7ec;    // CBallCameraMP1::Think(float, CStateManager&): x0 the camera
constexpr uint64_t kMs = 1000000ull;

struct PadFrame {
    uint64_t buttons = 0;
    hid::StickValue left, right;
    hid::SixAxisSample six;
};

struct Touch {
    bool down = false;
    uint32_t x = 0, y = 0;     // on the 1280 x 720 touch screen
};

struct G;

// ---- the hid input source ------------------------------------------------------------------------------------------------------------
class PlaySource : public hid::InputSource {
public:
    explicit PlaySource(Engine* e) : engine_(e) {}
    void poll(hid::InputSnapshot& out) override {
        const EngineOut kb = engine_->output(FrameLimiter::now_ns());
        PadFrame pf;
        Touch t;
        {
            std::lock_guard<std::mutex> lk(mu_);
            pf = pad_;
            t = touch_;
        }
        hid::NpadInput& n = out.npad[0];
        n.connected = true;
        n.style = hid::NpadStyle::FullKey;
        n.buttons = pf.buttons | kb.buttons;
        n.left = combine(pf.left, kb.lx, kb.ly);
        n.right = combine(pf.right, kb.rx, kb.ry);
        n.six_axis = pf.six;
        out.touch.count = 0;
        if (t.down) {
            out.touch.count = 1;
            hid::TouchFinger& f = out.touch.fingers[0];
            f = hid::TouchFinger{};
            f.id = 0;
            f.x = t.x;
            f.y = t.y;
        }
    }
    void set_pad(const PadFrame& p) {
        std::lock_guard<std::mutex> lk(mu_);
        pad_ = p;
    }
    void set_touch(const Touch& t) {
        std::lock_guard<std::mutex> lk(mu_);
        touch_ = t;
    }

private:
    static hid::StickValue combine(hid::StickValue pad, float kx, float ky) {
        double x = pad.x + static_cast<double>(kx) * 32767.0, y = pad.y + static_cast<double>(ky) * 32767.0;
        const double len = std::sqrt(x * x + y * y);
        if (len > 32767.0) { x *= 32767.0 / len; y *= 32767.0 / len; }
        hid::StickValue r;
        r.x = static_cast<int32_t>(std::lround(x));
        r.y = static_cast<int32_t>(std::lround(y));
        return r;
    }
    Engine* engine_;
    std::mutex mu_;
    PadFrame pad_;
    Touch touch_;
};

// ---- the state ------------------------------------------------------------------------------------------------------------------------
struct G {
    // settings
    std::mutex mu;                       // guards settings (written on the main thread, copied by the page on the presenting thread) and pending
    Settings settings;
    Settings pending;
    bool have_pending = false;
    bool dirty = false;                  // settings changed in the game (Alt+Enter): written at stop
    std::string path;
    bool loaded = false, started = false, load_ok = true;

    FrameLimiter limiter;
    std::unique_ptr<Engine> engine;
    std::unique_ptr<PlaySource> source_owner;
    std::shared_ptr<hid::InputSource> source;
    std::unique_ptr<gfx::Overlay> overlay;
    std::unique_ptr<AimDriver> aim;
    gfx::Renderer* renderer = nullptr;
    gfx::Device* device = nullptr;
    bool window = false;

    std::thread::id pump_thread;
    uint64_t base = 0x7100000000ull;
    uint32_t hook_update = 0, hook_input = 0, hook_fpcam = 0, hook_ballcam = 0;
    LookOffsets free_look, ball_look;     // free look (first person, Alt held) and the morph ball camera orbit; main thread only
    bool capture_mouse = true, quit_on_close = true, log_status = false;
    bool late_sample = true;           // pump the window's events again right before the mouse counts are taken (S11: the counts are as new as ProcessInput)
    double pitch_limit_deg = 0;

    // frame state (main thread)
    uint64_t update_seq = 0, aim_seq = 0;
    std::atomic<uint64_t> last_pi_ns{0};
    std::atomic<uint64_t> last_pi_frame{0};       // value of `frames` at the last ProcessInput (the game is playing while it keeps being called)
    std::atomic<uint64_t> player{0};
    uint64_t map_acts_seen = 0;
    bool map_hint = false;
    bool overlay_was_visible = false;
    bool was_in_game = false;
    GameHints hints;
    float mouse_x = 0, mouse_y = 0;
    bool mouse1 = false;
    uint64_t last_status_ns = 0;
    bool warned_thread = false;

    // pads
    uint64_t last_rumble_ns = 0;
    uint16_t rumble_lo = 0, rumble_hi = 0;

    // present mode requested by the page, applied on the presenting thread
    std::atomic<int> want_present{-1};

    // vibration from the game (hid thread) -> pads (main thread)
    std::atomic<float> vib[2][2] = {};   // [hand][low, high] amplitude

    // rebinding
    std::atomic<int> capture_state{0};   // 0 idle, 1 waiting for an input
    std::mutex cap_mu;
    bool cap_have = false;
    InputId cap_id;

    // live status
    mutable std::mutex st_mu;
    LiveStatus live;
    uint64_t fps_last_frames = 0, fps_last_ns = 0;
    double fps = 0;

    SelfTest selftest;

    // counters
    std::atomic<uint64_t> frames{0}, pi_calls{0}, aim_frames{0}, events{0}, counts{0};
    std::atomic<bool> thread_mismatch{false};
};

SelfTestHost* selftest_host();      // defined after the settings page host

G& g() {
    static G inst;
    return inst;
}

uint64_t now_ns() { return FrameLimiter::now_ns(); }

gfx::PresentMode present_mode_for(bool vsync) { return vsync ? gfx::PresentMode::Fifo : gfx::PresentMode::Immediate; }

const char* window_mode_name(platform::FullscreenMode m) {
    switch (m) {
        case platform::FullscreenMode::Windowed: return "windowed";
        case platform::FullscreenMode::Borderless: return "borderless";
        case platform::FullscreenMode::Exclusive: return "exclusive";
    }
    return "?";
}

void apply_frame_cap(const Settings& s) {
    G& st = g();
    double cap = s.display.fps_cap;
    if (s.display.vsync) {
        // the display paces; a cap only matters when it is below the refresh rate
        const float hz = st.window ? platform::display_refresh_hz() : 0.f;
        if (cap <= 0 || (hz > 0 && cap >= hz)) cap = 0;
    }
    st.limiter.set_target_fps(cap);
    if (extras::LowLatency* ll = extras::lowlat()) ll->set_cap_fps(cap);     // Reflex / Anti-Lag pace to the same cap
}

// ---- hooks ----------------------------------------------------------------------------------------------------------------------------
void pre_frame(void*) {
    // On the presenting thread, before the frame: the one place a present-mode change cannot collide with a frame in flight.
    G& st = g();
    const int want = st.want_present.exchange(-1);
    if (st.renderer && want >= 0) st.renderer->set_present_mode(static_cast<gfx::PresentMode>(want));
    if (st.overlay) st.overlay->apply_present_pending();
}

bool key_sink(const char* name, bool down) {
    InputId id;
    if (!name || !parse_input(name, &id)) return false;
    input::Event e;
    e.timestamp_ns = 0;
    switch (id.kind) {
        case InKind::Key:
            e.type = down ? input::EventType::KeyDown : input::EventType::KeyUp;
            e.u.key.scancode = id.code;
            e.u.key.keycode = static_cast<int32_t>(SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(id.code), SDL_KMOD_NONE, false));
            break;
        case InKind::Mouse:
            e.type = down ? input::EventType::MouseButtonDown : input::EventType::MouseButtonUp;
            e.u.button.button = static_cast<uint8_t>(id.code);
            e.u.button.clicks = 1;
            break;
        case InKind::Wheel:
            if (!down) return true;      // a notch has no release
            e.type = input::EventType::MouseWheel;
            e.u.wheel.dy = static_cast<float>(id.code);
            break;
        default: return false;
    }
    input::queue_push(e);
    return true;
}

// @shot in a script: the renderer writes the next presented frame as a PNG; the call returns when the file exists (or after 5 s).
bool screenshot_provider(const char* path, void*) {
    G& st = g();
    if (!st.renderer || !path) return false;
    DeleteFileA(path);
    if (std::strstr(path, "nvntrace_")) nvn::backend_trace_next_frame();   // `@shot nvntrace_<name>`: also trace the frame that is captured
    const uint64_t before = st.renderer->stats().screenshots;
    st.renderer->request_screenshot(path);
    for (int i = 0; i < 500; ++i) {
        if (st.renderer->stats().screenshots > before) return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;   // counted after the file is written
        Sleep(10);
    }
    return false;
}

void vib_sink(uint32_t slot, int hand, const hid::VibrationValue& v, void*) {
    if (slot != 0 || hand < 0 || hand > 1) return;
    G& st = g();
    st.vib[hand][0].store(v.amp_low);
    st.vib[hand][1].store(v.amp_high);
}

}  // namespace

[[noreturn]] void exit_process(int code, const char* verdict, const std::string& message) {
    G& st = g();
    NXLOG_I(kTag, "ending the process (%s): %s", verdict, message.c_str());
    if (st.dirty) {
        std::string err;
        std::lock_guard<std::mutex> lk(st.mu);
        if (!save_settings(st.path, st.settings, &err)) NXLOG_W(kTag, "cannot save %s: %s", st.path.c_str(), err.c_str());
        st.dirty = false;
    }
    nvn::backend_flush_caches();                    // the pipeline cache; this path does not shut the back end down
    nxrt::log_flush();
    if (tt::enabled() && !tt::finished()) tt::end_run(code, verdict, message);     // writes result.json, then terminates
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    for (;;) Sleep(1000);
}

namespace {

[[noreturn]] void quit_now(const char* why) { exit_process(0, "pass", why); }

void set_window_mode(const std::string& mode) {
    G& st = g();
    if (!st.window) return;
    const platform::FullscreenMode want = mode == "borderless" ? platform::FullscreenMode::Borderless
                                         : mode == "exclusive" ? platform::FullscreenMode::Exclusive
                                                               : platform::FullscreenMode::Windowed;
    if (platform::fullscreen_mode() == want) return;
    if (!platform::set_fullscreen(want)) NXLOG_W(kTag, "window mode '%s' was refused", mode.c_str());
    st.limiter.reset_schedule();
}

void toggle_fullscreen() {
    G& st = g();
    if (!st.window) return;
    std::string target;
    if (platform::fullscreen_mode() == platform::FullscreenMode::Windowed)
        target = st.settings.display.window_mode == "exclusive" ? "exclusive" : "borderless";
    else
        target = "windowed";
    set_window_mode(target);
    std::lock_guard<std::mutex> lk(st.mu);
    st.settings.display.window_mode = window_mode_name(platform::fullscreen_mode());
    st.dirty = true;
}

// Window pixels to the 1280 x 720 touch screen, honouring the letterbox the present pass draws.
bool touch_point(float px, float py, Touch* t) {
    G& st = g();
    int pw = 0, ph = 0;
    platform::pixel_size(&pw, &ph);
    if (pw <= 0 || ph <= 0) return false;
    double x = px, y = py, w = pw, h = ph, ox = 0, oy = 0;
    if (st.settings.display.keep_aspect) {
        const double s = std::min(w / 1280.0, h / 720.0);
        ox = (w - 1280.0 * s) / 2.0;
        oy = (h - 720.0 * s) / 2.0;
        x = (x - ox) / s;
        y = (y - oy) / s;
    } else {
        x = x / w * 1280.0;
        y = y / h * 720.0;
    }
    if (x < 0 || y < 0 || x >= 1280 || y >= 720) return false;
    t->x = static_cast<uint32_t>(x);
    t->y = static_cast<uint32_t>(y);
    return true;
}

void handle_event(const input::Event& e, uint64_t now) {
    G& st = g();
    using input::EventType;
    st.events++;
    // rebinding capture comes first: the page asked for the next input
    if (st.capture_state.load() == 1) {
        InputId id;
        bool got = false, cancel = false;
        if (e.type == EventType::KeyDown && !(e.flags & input::kFlagRepeat)) {
            if (e.u.key.scancode == SDL_SCANCODE_ESCAPE) cancel = true;
            else { id = {InKind::Key, e.u.key.scancode}; got = true; }
        } else if (e.type == EventType::MouseButtonDown) {
            id = {InKind::Mouse, e.u.button.button}; got = true;
        } else if (e.type == EventType::MouseWheel && e.u.wheel.dy != 0) {
            id = {InKind::Wheel, e.u.wheel.dy > 0 ? 1 : -1}; got = true;
        }
        if (got) {
            std::lock_guard<std::mutex> lk(st.cap_mu);
            st.cap_id = id;
            st.cap_have = true;
            st.capture_state.store(0);
            return;
        }
        if (cancel) { st.capture_state.store(0); return; }
        if (e.type == EventType::KeyDown || e.type == EventType::KeyUp) return;    // nothing reaches the game while capturing
    }
    if (st.overlay && st.overlay->handle_event(e)) return;
    const bool menu_blocked = st.overlay && st.overlay->visible();
    switch (e.type) {
        case EventType::Quit:
            if (st.quit_on_close) quit_now("window closed");
            break;
        case EventType::KeyDown:
            if ((e.u.key.scancode == SDL_SCANCODE_RETURN || e.u.key.scancode == SDL_SCANCODE_KP_ENTER) && (e.mods & SDL_KMOD_ALT)) {
                if (!(e.flags & input::kFlagRepeat)) toggle_fullscreen();
                return;
            }
            if (!(e.flags & input::kFlagRepeat) && !menu_blocked) st.engine->key(e.u.key.scancode, true, now);
            break;
        case EventType::KeyUp:
            st.engine->key(e.u.key.scancode, false, now);
            break;
        case EventType::MouseButtonDown:
            if (e.u.button.button == 1) st.mouse1 = true;
            if (!menu_blocked) st.engine->mouse_button(e.u.button.button, true, now);
            break;
        case EventType::MouseButtonUp:
            if (e.u.button.button == 1) st.mouse1 = false;
            st.engine->mouse_button(e.u.button.button, false, now);
            break;
        case EventType::MouseWheel: {
            if (menu_blocked) break;
            const float dy = e.u.wheel.dy;
            const int n = std::max(1, static_cast<int>(std::lround(std::fabs(dy))));
            if (dy != 0) for (int i = 0; i < n; ++i) st.engine->wheel(dy > 0 ? 1 : -1, now);
            break;
        }
        case EventType::MouseMotion:
            if (!(e.flags & input::kFlagRelative)) { st.mouse_x = e.u.motion.x; st.mouse_y = e.u.motion.y; }
            break;
        case EventType::WindowFocusLost:
            st.engine->release_all(now);
            st.mouse1 = false;
            break;
        case EventType::WindowFocusGained:
        case EventType::WindowRestored:
        case EventType::WindowMinimized:
            st.limiter.reset_schedule();
            break;
        case EventType::GamepadAdded:
            NXLOG_I(kTag, "controller %u connected", static_cast<unsigned>(e.u.gamepad.id));
            break;
        case EventType::GamepadRemoved:
            NXLOG_I(kTag, "controller %u disconnected", static_cast<unsigned>(e.u.gamepad.id));
            break;
        default: break;
    }
}

// Reads every open pad, merges them (buttons OR, the stick with the larger deflection wins) and publishes the result to the source.
void poll_pads(uint64_t now) {
    G& st = g();
    PadFrame pf;
    input::GamepadInfo infos[8];
    const size_t n = input::gamepad_list(infos, 8);
    bool have_six = false;
    uint16_t lo = 0, hi = 0;
    {
        const float l = std::max(st.vib[0][0].load(), st.vib[1][0].load()), h = std::max(st.vib[0][1].load(), st.vib[1][1].load());
        map_rumble(l, h, st.settings.pad, &lo, &hi);
    }
    const bool rumble_changed = lo != st.rumble_lo || hi != st.rumble_hi;
    const bool rumble_refresh = (lo || hi) && now - st.last_rumble_ns > 100 * kMs;
    for (size_t i = 0; i < n; ++i) {
        platform::GamepadRaw raw;
        if (!platform::gamepad_poll(infos[i].id, &raw)) continue;
        hid::NpadInput tmp;
        map_pad(raw, st.settings.pad, &tmp);
        pf.buttons |= tmp.buttons;
        auto big = [](hid::StickValue a, hid::StickValue b) {
            return static_cast<int64_t>(a.x) * a.x + static_cast<int64_t>(a.y) * a.y >= static_cast<int64_t>(b.x) * b.x + static_cast<int64_t>(b.y) * b.y ? a : b;
        };
        pf.left = big(pf.left, tmp.left);
        pf.right = big(pf.right, tmp.right);
        if (!have_six) {
            float a[3], gy[3];
            if (platform::gamepad_sensors(infos[i].id, a, gy)) {
                map_sensors(a, gy, st.settings.pad, &pf.six);
                have_six = true;
            }
        }
        if (rumble_changed || rumble_refresh) platform::gamepad_rumble(infos[i].id, lo, hi, 250);
    }
    if (rumble_changed || rumble_refresh) {
        st.rumble_lo = lo;
        st.rumble_hi = hi;
        st.last_rumble_ns = now;
    }
    st.source_owner->set_pad(pf);
}

int update_hook(::Ctx*, uint64_t, void*) {
    G& st = g();
    ft::mark("main.update");
    // low latency (stream S14): the sleep that lets the frame start as late as the GPU allows, then the input sample marker; the events are pumped right after
    if (extras::LowLatency* ll = extras::lowlat()) {
        ll->sim_begin();
        ll->input_sample();
    }
    const uint64_t now = now_ns();
    st.frames++;
    st.update_seq++;

    const bool on_pump_thread = std::this_thread::get_id() == st.pump_thread;
    if (!on_pump_thread && !st.warned_thread) {
        st.warned_thread = true;
        st.thread_mismatch.store(true);
        NXLOG_W(kTag, "the frame-boundary hook runs on another thread than the one that started play mode: SDL events are not pumped");
    }
    if (st.window && on_pump_thread) platform::pump_events();

    // settings queued by the page
    {
        std::unique_lock<std::mutex> lk(st.mu);
        if (st.have_pending) {
            Settings s = st.pending;
            st.have_pending = false;
            const std::string old_mode = st.settings.display.window_mode;
            st.settings = s;
            lk.unlock();
            st.engine->set_settings(s, now);
            apply_frame_cap(s);
            display_apply(s.display);
            extras::set_settings(s.extras);
            extras::hdr_refresh_levels();
            st.want_present.store(static_cast<int>(present_mode_for(s.display.vsync)));
            if (s.display.window_mode != old_mode) set_window_mode(s.display.window_mode);
            std::string err;
            if (!save_settings(st.path, s, &err)) NXLOG_W(kTag, "cannot save %s: %s", st.path.c_str(), err.c_str());
            else NXLOG_I(kTag, "settings applied and saved to %s", st.path.c_str());
        }
    }

    // events
    input::Event buf[128];
    for (;;) {
        const size_t got = input::queue_drain(buf, 128);
        if (!got) break;
        for (size_t i = 0; i < got; ++i) handle_event(buf[i], now);
    }
    poll_pads(now);
    if (st.engine) st.engine->frame_tick();

    // what the game is doing: the context, the beam, the pointer
    const uint64_t pi = st.last_pi_ns.load();
    // "In game" = the game called ProcessInput during one of the last few frame boundaries. Counted in frames, not host time: a slow frame (the game
    // can take 100 ms or more per frame under load) must not read as a menu.
    const bool in_game = pi != 0 && st.frames.load() - st.last_pi_frame.load() <= 4;
    st.hints = st.aim->hints(st.player.load());
    if (st.hints.valid) {
        static const int game_to_mine[4] = {0, 2, 1, 3};      // game: power ice wave plasma; bindings: power wave ice plasma
        unsigned mask = 0;
        for (int i = 0; i < 4; ++i)
            if (st.hints.owned_beams & (1u << i)) mask |= 1u << game_to_mine[i];
        st.engine->set_game_hints(st.hints.beam >= 0 ? game_to_mine[st.hints.beam] : -1, mask, now);
    }
    const uint64_t map_acts = st.engine->activations(Act::Map);
    if (in_game && map_acts != st.map_acts_seen) st.map_hint = true;
    st.map_acts_seen = map_acts;
    if (in_game && !st.was_in_game) st.map_hint = false;
    st.was_in_game = in_game;
    Ctx ctx;
    if (!in_game) ctx = st.map_hint ? Ctx::Map : Ctx::Menu;
    else if (st.hints.morphed) ctx = Ctx::Morph;
    else if (st.hints.visor == 2) ctx = Ctx::Scan;
    else ctx = Ctx::Gameplay;
    st.engine->set_context(ctx, now);

    const bool overlay_visible = st.overlay && st.overlay->visible();
    if (overlay_visible && !st.overlay_was_visible) st.engine->release_all(now);
    st.overlay_was_visible = overlay_visible;

    if (st.window) {
        const bool want = st.capture_mouse && platform::focused() && !platform::minimized() && !overlay_visible && in_game &&
                          st.capture_state.load() == 0;
        if (want != platform::relative_mouse()) {
            platform::set_relative_mouse(want);
            if (want) input::mouse_delta_take();      // nothing that piled up before the capture
        }
        // menus: the left click as a touch
        Touch t;
        if (!want && !overlay_visible && st.settings.mouse.menu_touch && (ctx == Ctx::Menu || ctx == Ctx::Map) && st.mouse1) {
            t.down = touch_point(st.mouse_x, st.mouse_y, &t);
        }
        st.source_owner->set_touch(t);
    }
    if (!in_game) input::mouse_delta_take();         // aim only counts what arrives while the game is playing

    if (st.log_status && now - st.last_status_ns > 1000 * kMs) {
        st.last_status_ns = now;
        NXLOG_I(kTag, "status: ctx=%s in_game=%d aim=%s frames=%llu process_input=%llu aim_frames=%llu events=%llu pads=%zu captured=%d display_hook_calls=%u", ctx_name(ctx),
                in_game ? 1 : 0, aim_block_name(st.aim->last_block()), static_cast<unsigned long long>(st.frames.load()),
                static_cast<unsigned long long>(st.pi_calls.load()), static_cast<unsigned long long>(st.aim_frames.load()),
                static_cast<unsigned long long>(st.events.load()), input::gamepad_count(), platform::relative_mouse() ? 1 : 0, display_hook_calls());
    }
    if (st.selftest.active()) st.selftest.tick(st.renderer, &st.limiter, selftest_host());
    return 0;
}

// Calls the game's own SetTransform(player, xf) from inside the hook.
struct XfCall { ::Ctx* c; };
void guest_set_transform(void* user, uint64_t player, const float xf[12]) {
    G& st = g();
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
    G& st = g();
    const uint64_t now = now_ns();
    st.last_pi_ns.store(now);
    st.last_pi_frame.store(st.frames.load());
    st.pi_calls++;
    const uint64_t player = c->x[0], mgr = c->x[2];
    st.player.store(player);

    double dx = 0, dy = 0;
    if (st.update_seq != st.aim_seq) {                // the first ProcessInput since the frame boundary takes the mouse counts
        st.aim_seq = st.update_seq;
        // Late sample: the frame boundary pumped the events at the start of Update; the tick that reaches ProcessInput is a little later (the
        // whole Update body runs first). Pump again so the counts include whatever the mouse did in between. Main thread only, like the pump
        // in the frame-boundary hook; the key and button events it queues are handled at the next boundary.
        if (st.late_sample && st.window && std::this_thread::get_id() == st.pump_thread) platform::pump_events();
        if (extras::LowLatency* ll = extras::lowlat()) ll->input_sample();
        const input::MouseDelta d = input::mouse_delta_take();
        dx = d.dx;
        dy = d.dy;
    }
    // Camera look instead of body aim: the morph ball camera takes the mouse in the ball; the free look key makes the first-person mouse turn the
    // view and not Samus. The counts are consumed here (the aim gets none), and the camera Think hooks below apply the offsets.
    {
        const AimBlock gate = st.aim->gate(player, mgr);
        const MouseSettings& m = st.settings.mouse;
        uint32_t morph_word = 0;                       // 0 unmorphed, 2 and 3 transitions, 1 in the ball (the gate stops at "not first person" first)
        guest_read(player + 0x4d4, &morph_word, sizeof morph_word);
        if (morph_word == 1) {
            LookParams lp;
            lp.yaw_rad = m.morph_yaw_rad;
            lp.pitch_rad = m.morph_pitch_rad;
            lp.invert_x = m.invert_x;
            lp.invert_y = m.invert_y;
            if (dx != 0 || dy != 0) st.ball_look.add(dx, dy, lp);
            dx = dy = 0;
        } else {
            st.ball_look.reset();
        }
        if (gate == AimBlock::None && st.engine && st.engine->act_active(Act::FreeLook, now)) {
            LookParams lp;
            lp.yaw_rad = m.yaw_rad;
            lp.pitch_rad = m.pitch_rad;
            lp.invert_x = m.invert_x;
            lp.invert_y = m.invert_y;
            if (dx != 0 || dy != 0) st.free_look.add(dx, dy, lp);
            dx = dy = 0;
        } else {
            st.free_look.reset();
        }
    }
    AimParams p;
    {
        const MouseSettings& m = st.settings.mouse;
        p.yaw_rad = m.yaw_rad;
        p.pitch_rad = m.pitch_rad;
        p.invert_x = m.invert_x;
        p.invert_y = m.invert_y;
        p.pitch_limit_rad = st.pitch_limit_deg * kPi / 180.0;
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
    if (st.log_status && (dx != 0 || dy != 0))
        NXLOG_I(kTag, "aim: dx=%.1f dy=%.1f block=%s applied=%d pitch=%.4f", dx, dy, aim_block_name(st.aim->last_block()), applied ? 1 : 0, st.aim->pitch());
    if (applied) {
        st.aim_frames++;
        st.counts += static_cast<uint64_t>(std::fabs(dx) + std::fabs(dy));
    }
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
    G& st = g();
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
    if (!off.active() || !read_xf(cam, &xfp, cur)) return 1;
    float pivot[3] = {cur[3], cur[7], cur[11]};          // free look: the eye itself
    if (ball) {
        float pxf[12];
        uint64_t pp = 0;
        const uint64_t player = st.player.load();
        if (!player || !read_xf(player, &pp, pxf)) return 1;
        pivot[0] = pxf[3]; pivot[1] = pxf[7]; pivot[2] = pxf[11];
    }
    float out[12];
    orbit_transform(cur, pivot, off.yaw(), off.pitch(), out);
    std::memcpy(memo.orig, cur, sizeof cur);
    guest_set_transform(&xc, cam, out);
    if (read_xf(cam, &xfp, memo.applied)) { memo.have = true; memo.cam = cam; }
    return 1;
}

// ---- the settings page host -----------------------------------------------------------------------------------------------------------
class Host : public UiHost {
public:
    Settings settings() const override { G& st = g(); std::lock_guard<std::mutex> lk(st.mu); return st.have_pending ? st.pending : st.settings; }
    Settings factory() const override { return default_settings(); }
    void apply(const Settings& s) override { G& st = g(); std::lock_guard<std::mutex> lk(st.mu); st.pending = s; st.have_pending = true; }
    LiveStatus status() const override {
        G& st = g();
        LiveStatus s;
        s.ctx = st.engine ? st.engine->context() : Ctx::Menu;
        s.aim = aim_block_name(st.aim->last_block());
        if (st.pi_calls.load() == 0) s.aim = "not in game";
        s.mouse_captured = st.window && platform::relative_mouse();
        input::GamepadInfo infos[4];
        s.pads = static_cast<int>(input::gamepad_count());
        if (input::gamepad_list(infos, 4) > 0) {
            s.pad_name = infos[0].name;
            s.pad_gyro = infos[0].gyro;
            s.pad_rumble = infos[0].rumble;
        }
        if (st.renderer) {
            s.present_mode = gfx::present_mode_name(st.renderer->present_mode());
            const uint64_t now = now_ns();
            const uint64_t frames = st.renderer->stats().frames;
            std::lock_guard<std::mutex> lk(st.st_mu);
            if (now - st.fps_last_ns > 1000 * kMs) {
                if (st.fps_last_ns) st.fps = static_cast<double>(frames - st.fps_last_frames) * 1e9 / static_cast<double>(now - st.fps_last_ns);
                st.fps_last_frames = frames;
                st.fps_last_ns = now;
            }
            s.fps = st.fps;
        }
        if (st.window) {
            s.window_mode = window_mode_name(platform::fullscreen_mode());
            s.refresh_hz = platform::display_refresh_hz();
            platform::pixel_size(&s.width, &s.height);
        }
        s.out = st.engine->output(now_ns());
        s.pitch_deg = st.aim->pitch() * 180.0 / kPi;
        s.input_calls = st.pi_calls.load();
        return s;
    }
    std::string settings_path() const override { return g().path; }
    void begin_capture() override {
        G& st = g();
        std::lock_guard<std::mutex> lk(st.cap_mu);
        st.cap_have = false;
        st.capture_state.store(1);
    }
    bool capturing() const override { return g().capture_state.load() == 1; }
    bool capture_result(InputId* out) override {
        G& st = g();
        std::lock_guard<std::mutex> lk(st.cap_mu);
        if (!st.cap_have) return false;
        st.cap_have = false;
        if (out) *out = st.cap_id;
        return true;
    }
    void cancel_capture() override { g().capture_state.store(0); }
};
Host g_host;

// The self-test's window onto play mode: the same entry points the settings page uses.
class SelfTestHostImpl : public SelfTestHost {
public:
    bool set(const std::string& key, const std::string& value) override {
        G& st = g();
        Settings s = g_host.settings();
        const double d = std::atof(value.c_str());
        if (key == "vsync") s.display.vsync = d != 0;
        else if (key == "fps_cap") s.display.fps_cap = static_cast<int>(d);
        else if (key == "window_mode") s.display.window_mode = value;
        else if (key == "yaw_source") s.mouse.yaw_rad = rad_per_count(d);
        else if (key == "pitch_source") s.mouse.pitch_rad = rad_per_count(d);
        else if (key == "invert_y") s.mouse.invert_y = d != 0;
        else if (key == "invert_x") s.mouse.invert_x = d != 0;
        else return false;
        g_host.apply(s);
        (void)st;
        return true;
    }
    std::string present_mode() override { G& st = g(); return st.renderer ? gfx::present_mode_name(st.renderer->present_mode()) : ""; }
    std::string settings_text() override {
        G& st = g();
        std::string text;
        if (FILE* f = std::fopen(st.path.c_str(), "rb")) {
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
            std::fclose(f);
        }
        return text;
    }
};

SelfTestHost* selftest_host() {
    static SelfTestHostImpl h;
    return &h;
}

// mpr.ini and the environment override the settings file for the display extras. A key that is set there ([hdr] mode in mpr.ini, or NXRT_HDR_MODE)
// replaces the file's value with the one extras::settings_from_config() reads; other keys keep the file's value (the F1 page edits the file).
extras::Settings merge_env(const extras::Settings& from_file) {
    const extras::Settings env = extras::settings_from_config();
    extras::Settings r = from_file;
    auto set = [](const char* name) {
        const char* v = std::getenv(name);
        return v != nullptr && v[0] != '\0';
    };
    if (config_has("hdr", "mode")) r.hdr.mode = env.hdr.mode;
    if (config_has("hdr", "format")) r.hdr.format = env.hdr.format;
    if (config_has("hdr", "peak_nits")) r.hdr.peak_nits = env.hdr.peak_nits;
    if (config_has("hdr", "paper_white_nits")) r.hdr.paper_white_nits = env.hdr.paper_white_nits;
    if (config_has("hdr", "highlight_boost")) r.hdr.highlight_boost = env.hdr.highlight_boost;
    if (config_has("upscale", "mode")) r.upscale.mode = env.upscale.mode;
    if (config_has("upscale", "sharpness")) r.upscale.sharpness = env.upscale.sharpness;
    if (config_has("upscale", "scene_scale")) r.upscale.scene_scale = env.upscale.scene_scale;
    if (config_has("upscale", "fsr_dll")) r.upscale.fsr_dll = env.upscale.fsr_dll;
    if (config_has("upscale", "dlss_dll")) r.upscale.dlss_dll = env.upscale.dlss_dll;
    if (config_has("upscale", "xess_dll")) r.upscale.xess_dll = env.upscale.xess_dll;
    if (config_has("upscale", "frame_generation")) r.upscale.frame_generation = env.upscale.frame_generation;
    if (config_has("latency", "mode")) r.latency.mode = env.latency.mode;
    if (config_has("latency", "log_timings")) r.latency.log_timings = env.latency.log_timings;
    return r;
}

}  // namespace

// ---- public API ---------------------------------------------------------------------------------------------------------------------------
bool load() {
    G& st = g();
    if (st.loaded) return st.load_ok;
    st.loaded = true;
    st.path = config_get_str("play", "controls", "controls.ini");
    std::string err;
    const bool existed = GetFileAttributesA(st.path.c_str()) != INVALID_FILE_ATTRIBUTES;
    st.load_ok = load_settings(st.path, &st.settings, &err);
    if (!existed) {
        std::string e2;
        if (save_settings(st.path, st.settings, &e2)) NXLOG_I(kTag, "wrote the factory settings to %s", st.path.c_str());
    } else if (!st.load_ok) {
        NXLOG_W(kTag, "%s: %s (the factory settings are in use)", st.path.c_str(), err.c_str());
    } else {
        NXLOG_I(kTag, "settings loaded from %s (%zu bindings)", st.path.c_str(), st.settings.bindings.size());
    }
    // Display extras: a value set in mpr.ini or in the environment (NXRT_HDR_MODE ...) overrides the file; the file overrides the built-in defaults.
    st.settings.extras = merge_env(st.settings.extras);
    extras::set_settings(st.settings.extras);
    st.capture_mouse = config_get_bool("play", "capture_mouse", true);
    st.late_sample = config_get_bool("play", "late_sample", true);
    st.quit_on_close = config_get_bool("play", "quit_on_close", true);
    st.log_status = config_get_bool("play", "log_status", false);
    st.pitch_limit_deg = config_get_double("play", "pitch_limit_deg", 0.0);
    st.base = static_cast<uint64_t>(config_get_int("play", "game_base", 0x7100000000ll));
    apply_frame_cap(st.settings);
    return st.load_ok;
}

DisplayConfig display() {
    G& st = g();
    load();
    DisplayConfig d;
    d.width = st.settings.display.width;
    d.height = st.settings.display.height;
    d.window_mode = st.settings.display.window_mode;
    d.vsync = st.settings.display.vsync;
    d.fps_cap = st.settings.display.fps_cap;
    d.keep_aspect = st.settings.display.keep_aspect;
    d.present_mode = present_mode_for(st.settings.display.vsync);
    return d;
}

FrameLimiter* limiter() {
    load();
    return &g().limiter;
}

std::string settings_path() {
    load();
    return g().path;
}

bool start(const StartOptions& opt) {
    G& st = g();
    load();
    if (st.started) return true;
    st.renderer = opt.renderer;
    st.device = opt.device;
    st.window = opt.window && platform::has_window();
    kernel::sched_set_process_priority(st.window);     // a person is playing: keep the frame out of the background load
    st.pump_thread = std::this_thread::get_id();
    st.engine = std::make_unique<Engine>(st.settings);
    st.engine->set_context(Ctx::Menu, now_ns());
    st.source_owner = std::make_unique<PlaySource>(st.engine.get());
    st.source = std::shared_ptr<hid::InputSource>(st.source_owner.get(), [](hid::InputSource*) {});   // owned by G
    st.aim = std::make_unique<AimDriver>(st.base);
    hid::set_input_source(st.source);
    hid::set_vibration_sink(&vib_sink, nullptr);
    tt::set_mouse_sink(&input::mouse_delta_add);
    tt::set_key_sink(&key_sink);
    if (st.renderer) tt::set_screenshot_provider(&screenshot_provider, nullptr);
    if (const std::string script = config_get_str("play", "selftest", ""); !script.empty()) {
        std::string err;
        if (!st.selftest.parse(script, &err)) {
            NXLOG_E(kTag, "[play] selftest: %s", err.c_str());
            nxrt::log_flush();
            TerminateProcess(GetCurrentProcess(), 2);
        }
        NXLOG_I(kTag, "selftest script: %s", script.c_str());
    }

    if (st.renderer) {
        st.limiter.set_pre_frame_hook(&pre_frame, nullptr);
        if (!st.renderer->headless()) st.renderer->set_present_mode(present_mode_for(st.settings.display.vsync));
        if (opt.overlay && opt.device) {
            gfx::Overlay::Context oc;
            oc.device = opt.device;
            oc.renderer = opt.renderer;
            oc.limiter = &st.limiter;
            oc.has_window = st.window;
            std::string err;
            st.overlay = gfx::Overlay::create(oc, &err);
            if (!st.overlay) {
                NXLOG_W(kTag, "no overlay: %s", err.c_str());
            } else {
                st.overlay->add_panel("Controls", [] { draw_settings_page(g_host); });
                st.overlay->install_on_renderer();
            }
        }
    }
    if (st.window && st.settings.display.window_mode != "windowed") set_window_mode(st.settings.display.window_mode);

    {   // display options: the internal resolution comes from the window as it is now (after the window mode change above)
        int ww = 1600, wh = 900;
        if (st.window) platform::pixel_size(&ww, &wh);
        if (ww <= 0 || wh <= 0) { ww = st.settings.display.width; wh = st.settings.display.height; }
        display_install(st.base, st.settings.display, ww, wh);
    extras::tonemap_hook_install(st.base);
    }
    st.hook_update = hook_add_entry(st.base + kFnUpdate, &update_hook);
    st.hook_input = hook_add_entry(st.base + kFnProcessInput, &process_input_hook);
    st.hook_fpcam = hook_add_entry(st.base + kFnFpCamThink, &cam_think_hook, nullptr);
    st.hook_ballcam = hook_add_entry(st.base + kFnBallCamThink, &cam_think_hook, &st);
    st.started = true;
    NXLOG_I(kTag, "play mode started (%s, overlay %s, base 0x%llx, hooks %u/%u)", st.window ? "window" : "no window", st.overlay ? "on" : "off",
            static_cast<unsigned long long>(st.base), st.hook_update, st.hook_input);
    return true;
}

void stop() {
    G& st = g();
    if (!st.started) return;
    if (st.hook_update) hook_remove(st.hook_update);
    if (st.hook_input) hook_remove(st.hook_input);
    if (st.hook_fpcam) hook_remove(st.hook_fpcam);
    if (st.hook_ballcam) hook_remove(st.hook_ballcam);
    st.hook_update = st.hook_input = st.hook_fpcam = st.hook_ballcam = 0;
    display_remove();
    hid::set_vibration_sink(nullptr, nullptr);
    if (hid::input_source() == st.source) hid::set_input_source(nullptr);
    if (st.dirty) {
        std::string err;
        std::lock_guard<std::mutex> lk(st.mu);
        save_settings(st.path, st.settings, &err);
        st.dirty = false;
    }
    st.limiter.set_pre_frame_hook(nullptr, nullptr);
    st.started = false;
}

Stats stats() {
    G& st = g();
    Stats s;
    s.frames = st.frames.load();
    s.process_input_calls = st.pi_calls.load();
    s.aim_frames = st.aim_frames.load();
    s.events = st.events.load();
    s.mouse_counts_applied = st.counts.load();
    s.pump_thread_mismatch = st.thread_mismatch.load();
    return s;
}

}  // namespace nxrt::play
