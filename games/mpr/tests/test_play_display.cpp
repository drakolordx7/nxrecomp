// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode display options (play_display.h): the arithmetic (internal size, aspect, HUD cameras, field of view) and the entry hooks that
// apply the options to game objects. The hooks are driven the way lifted code drives them (nx_hook_entry on the registered guest address)
// against fake objects in host memory, with a fake module base.
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "test_framework.h"

#include "nxrt/gfx_tuning.h"
#include "nxrt/hooks.h"
#include "mpr/display_hooks.h"
#include "play/play_display.h"

NXTEST_GROUP(play_display)

namespace {

namespace play = nxrt::play;

constexpr uint64_t kBase = 0x5B00000000ull;
constexpr uint64_t kGetScreenWidth = 0x1248, kGetScreenHeight = 0x1250;
constexpr uint64_t kResolutionScale = 0x3c08a0, kCameraNodes = 0xafe41c, kGuiProjection = 0xbd52fc;
constexpr uint64_t kVerticalFov = 0x4c2998, kBloom = 0x3a450c, kChromatic = 0x3a69b0;
constexpr uint64_t kDefaultAspect = 0x20e7a7c;
constexpr double kPi = 3.14159265358979323846;
constexpr double kAspect21x9 = 21.0 / 9.0;
constexpr double kAspect16x9 = 16.0 / 9.0;

bool close_to(double got, double want, double eps = 1e-4) { return std::fabs(got - want) < eps; }

// The guest global the camera hook writes to: commit its page once.
void commit_globals() {
    static bool done = false;
    if (done) return;
    done = true;
    CHECK(VirtualAlloc(reinterpret_cast<void*>(kBase + 0x20e7000), 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != nullptr);
}

void install(const play::DisplaySettings& d, int window_w = 1280, int window_h = 720) {
    commit_globals();
    play::display_install(kBase, d, window_w, window_h);
}

int call(uint64_t off, ::Ctx* c) { return nx_hook_entry(c, kBase + off); }

float read_f(const void* p) {
    float v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

void write_f(void* p, float v) { std::memcpy(p, &v, sizeof v); }

}  // namespace

// ---- arithmetic -----------------------------------------------------------------------------------------------------------------

NXTEST(internal_size_scales_the_window) {
    play::DisplaySettings d;
    int w = 0, h = 0;
    play::internal_size(d, 1280, 720, &w, &h);            // default scale is 100 percent
    CHECK_EQ(w, 1280);
    CHECK_EQ(h, 720);
    d.render_scale = 200;
    play::internal_size(d, 1280, 720, &w, &h);
    CHECK_EQ(w, 2560);
    CHECK_EQ(h, 1440);
    d.render_scale = 50;
    play::internal_size(d, 1280, 720, &w, &h);
    CHECK_EQ(w, 640);
    CHECK_EQ(h, 360);
}

NXTEST(internal_size_fixed_size_wins_and_a_single_side_is_ignored) {
    play::DisplaySettings d;
    int w = 0, h = 0;
    d.render_width = 3840;
    d.render_height = 1600;
    d.render_scale = 50;
    play::internal_size(d, 1280, 720, &w, &h);
    CHECK_EQ(w, 3840);
    CHECK_EQ(h, 1600);
    // only one side set: the fixed size is ignored and the window (at 100 percent) is used
    d.render_scale = 100;
    d.render_width = 3840;
    d.render_height = 0;
    play::internal_size(d, 1280, 720, &w, &h);
    CHECK_EQ(w, 1280);
    CHECK_EQ(h, 720);
    d.render_width = 0;
    d.render_height = 1600;
    play::internal_size(d, 1280, 720, &w, &h);
    CHECK_EQ(w, 1280);
    CHECK_EQ(h, 720);
}

NXTEST(internal_size_rounds_odd_results_down_to_even) {
    play::DisplaySettings d;
    int w = 0, h = 0;
    play::internal_size(d, 1281, 721, &w, &h);
    CHECK_EQ(w, 1280);
    CHECK_EQ(h, 720);
}

NXTEST(internal_size_caps_the_area_and_keeps_the_aspect) {
    play::DisplaySettings d;
    d.render_width = 16384;
    d.render_height = 16384;
    int w = 0, h = 0;
    play::internal_size(d, 1280, 720, &w, &h);
    CHECK(static_cast<double>(w) * static_cast<double>(h) <= 134217728.0);     // 8192 * 8192 * 2
    CHECK(std::fabs(static_cast<double>(w) / h - 1.0) < 0.01);
    CHECK(w >= 320 && w <= 16384);
    CHECK(h >= 320 && h <= 16384);
}

NXTEST(internal_size_raises_a_tiny_window_to_the_minimum) {
    play::DisplaySettings d;
    d.render_scale = 25;
    int w = 0, h = 0;
    play::internal_size(d, 100, 50, &w, &h);             // 25 x 12.5 before the clamp
    CHECK_EQ(w, 320);
    CHECK_EQ(h, 320);
}

NXTEST(display_aspect_follows_the_resolution_unless_forced) {
    play::DisplaySettings d;
    CHECK(close_to(play::display_aspect(d, 3440, 1440), 3440.0 / 1440.0, 1e-3));
    d.aspect = kAspect16x9;
    CHECK(close_to(play::display_aspect(d, 3440, 1440), kAspect16x9));          // forced 16:9 wins
    d.aspect = 9.0;
    CHECK(close_to(play::display_aspect(d, 3440, 1440), 5.0));                  // clamped to the top
    d.aspect = 0.2;
    CHECK(close_to(play::display_aspect(d, 3440, 1440), 1.0));                  // 0.2 > 0 is forced, then clamped to the bottom
    d.aspect = 0;
    CHECK(close_to(play::display_aspect(d, 0, 1440), kAspect16x9));             // no usable size: 16:9
    CHECK(close_to(play::display_aspect(d, 3440, 0), kAspect16x9));
    CHECK(close_to(play::display_aspect(d, -5, 1440), kAspect16x9));
}

NXTEST(hud_perspective_widens_only_the_16_9_camera) {
    float a = 1.7778f;
    CHECK(play::hud_perspective(&a, 2.3889));
    CHECK(close_to(a, 2.3889));
    float four_three = 1.3333f;                                                 // a 4:3 camera is not a HUD camera
    CHECK(!play::hud_perspective(&four_three, 2.3889));
    CHECK(four_three == 1.3333f);
    float already = 2.3889f;                                                    // already the display aspect
    CHECK(play::hud_perspective(&already, 2.3889));
    CHECK(close_to(already, 2.3889));
    CHECK(!play::hud_perspective(nullptr, 2.3889));
}

NXTEST(hud_perspective_narrow_display_grows_the_vertical_angle_once) {
    // 16:10: the horizontal reach of the frame stays what it was drawn for, so the vertical angle grows: tan(v'/2) = tan(65/2) * (16/9) / 1.6
    float aspect = 1.7778f, fov = 65.0f;
    CHECK(play::hud_perspective(&aspect, 1.6, &fov));
    CHECK(close_to(aspect, 1.6));
    const double want = 2.0 * std::atan(std::tan(65.0 * 3.14159265358979 / 360.0) * (16.0 / 9.0) / 1.6) * 180.0 / 3.14159265358979;
    CHECK(close_to(fov, want));
    const float once = fov;
    CHECK(play::hud_perspective(&aspect, 1.6, &fov));            // called again for the same camera (every frame): no change
    CHECK(close_to(fov, once));
    float wide_aspect = 1.7778f, wide_fov = 65.0f;                // a display wider than 16:9 never changes the angle
    CHECK(play::hud_perspective(&wide_aspect, 2.3889, &wide_fov));
    CHECK(close_to(wide_fov, 65.0));
    float no_fov_aspect = 1.7778f;                                // without a fov pointer only the aspect changes (old behaviour)
    CHECK(play::hud_perspective(&no_fov_aspect, 1.6));
    CHECK(close_to(no_fov_aspect, 1.6));
}

NXTEST(hud_ortho_narrow_display_keeps_the_width_and_grows_the_height) {
    float l = -8, r = 8, b = -4.5f, t = 4.5f;
    CHECK(play::hud_ortho(&l, &r, &b, &t, 1.6));
    CHECK(close_to(l, -8) && close_to(r, 8));
    CHECK(close_to(t, 5.0) && close_to(b, -5.0));                // 16 / 1.6 = 10 units tall around the centre
    CHECK(play::hud_ortho(&l, &r, &b, &t, 1.6));                  // again: the same
    CHECK(close_to(t, 5.0) && close_to(b, -5.0));
    float l2 = 8, r2 = -8, b2 = -4.5f, t2 = 4.5f;                 // the retail ball HUD (reversed left and right)
    CHECK(play::hud_ortho(&l2, &r2, &b2, &t2, 4.0 / 3.0));
    CHECK(close_to(l2, 8) && close_to(r2, -8));
    CHECK(close_to(t2, 6.0) && close_to(b2, -6.0));
}

NXTEST(hud_ortho_widens_the_16_9_extents_around_the_centre) {
    float l = -8, r = 8, b = -4.5f, t = 4.5f;
    CHECK(play::hud_ortho(&l, &r, &b, &t, 32.0 / 9.0));
    CHECK(close_to(l, -16));
    CHECK(close_to(r, 16));
    CHECK(b == -4.5f);
    CHECK(t == 4.5f);
    // bottom above top: the same widening
    l = -8;
    r = 8;
    b = 4.5f;
    t = -4.5f;
    CHECK(play::hud_ortho(&l, &r, &b, &t, 32.0 / 9.0));
    CHECK(close_to(l, -16));
    CHECK(close_to(r, 16));
    CHECK(b == 4.5f);
    CHECK(t == -4.5f);
}

NXTEST(hud_ortho_leaves_other_extents_alone) {
    float l = -4, r = 4, b = -3, t = 3;                                         // 4:3
    CHECK(!play::hud_ortho(&l, &r, &b, &t, 32.0 / 9.0));
    CHECK(l == -4 && r == 4 && b == -3 && t == 3);
    l = -8;
    r = 8;
    b = 1;
    t = 1;                                                                      // zero height
    CHECK(!play::hud_ortho(&l, &r, &b, &t, 32.0 / 9.0));
    CHECK(l == -8 && r == 8);
}

NXTEST(hud_ortho_keeps_an_off_centre_camera_centred) {
    float l = 0, r = 16, b = 0, t = 9;
    CHECK(play::hud_ortho(&l, &r, &b, &t, kAspect21x9));
    CHECK(close_to((static_cast<double>(l) + r) * 0.5, 8.0));                   // centre stays on x = 8
    CHECK(close_to(static_cast<double>(r) - l, 21.0));                          // width = 21 for a height of 9
    CHECK(close_to(static_cast<double>(t) - b, 9.0));
}

NXTEST(hud_ortho_keeps_the_reversed_order_of_the_retail_ball_hud) {
    // FRME_BallHud stores left = 8, right = -8, top = 4.5, bottom = -4.5 (read from the game's own file)
    float l = 8, r = -8, b = -4.5f, t = 4.5f;
    CHECK(play::hud_ortho(&l, &r, &b, &t, kAspect21x9));
    CHECK(close_to(l, 10.5, 1e-4) && close_to(r, -10.5, 1e-4));                 // wider, still left > right
    CHECK(close_to(b, -4.5) && close_to(t, 4.5));
    float l2 = 8, r2 = -8, b2 = -4.5f, t2 = 4.5f;
    CHECK(play::hud_ortho(&l2, &r2, &b2, &t2, 16.0 / 9.0));                     // same aspect: unchanged
    CHECK(close_to(l2, 8.0, 1e-4) && close_to(r2, -8.0, 1e-4));
}

NXTEST(fov_offset_is_added_in_degrees_and_clamped) {
    CHECK(close_to(play::fov_with_offset(1.0f, 10.0), 1.0 + 10.0 * kPi / 180.0, 1e-5));
    CHECK(close_to(play::fov_with_offset(1.0f, -300.0), 10.0 * kPi / 180.0, 1e-5));
    CHECK(close_to(play::fov_with_offset(1.0f, 300.0), 170.0 * kPi / 180.0, 1e-5));
}

// ---- hooks ----------------------------------------------------------------------------------------------------------------------

NXTEST(hook_resolution_reports_the_internal_size) {
    play::DisplaySettings d;
    d.render_scale = 200;
    install(d, 1280, 720);
    ::Ctx w{};
    CHECK_EQ(call(kGetScreenWidth, &w), 1);
    CHECK_EQ(w.x[0], 2560u);
    ::Ctx h{};
    CHECK_EQ(call(kGetScreenHeight, &h), 1);
    CHECK_EQ(h.x[0], 1440u);
    const play::DisplayLive live = play::display_live();
    CHECK_EQ(live.render_w, 2560);
    CHECK_EQ(live.render_h, 1440);
    play::display_remove();
}

NXTEST(hook_dynamic_resolution_is_off_unless_the_game_controller_is_allowed) {
    play::DisplaySettings d;
    d.dynamic_resolution = false;
    install(d);
    ::Ctx on{};
    on.v[0].f32[0] = 0.5f;
    CHECK_EQ(call(kResolutionScale, &on), 1);
    CHECK(on.v[0].f32[0] == 1.0f);                                              // scale 1.0: dynamic resolution off
    play::DisplaySettings live = d;
    live.dynamic_resolution = true;
    play::display_apply(live);
    ::Ctx game{};
    game.v[0].f32[0] = 0.5f;
    CHECK_EQ(call(kResolutionScale, &game), 0);                                 // the game's own controller runs
    CHECK(game.v[0].f32[0] == 0.5f);
    play::display_remove();
}

NXTEST(hook_camera_nodes_write_the_display_aspect) {
    std::vector<uint8_t> mgr(32, 0), proxy(160, 0);
    const uint64_t proxy_addr = reinterpret_cast<uint64_t>(proxy.data());
    std::memcpy(mgr.data() + 16, &proxy_addr, sizeof proxy_addr);              // [manager + 16] = the scene node proxy
    play::DisplaySettings d;
    d.aspect = kAspect21x9;
    install(d);
    play::display_apply(d);
    ::Ctx c{};
    c.x[0] = reinterpret_cast<uint64_t>(mgr.data());
    CHECK_EQ(call(kCameraNodes, &c), 0);                                        // not handled: the game's function runs with the aspect in place
    CHECK(close_to(read_f(proxy.data() + 152), kAspect21x9));
    CHECK(close_to(*reinterpret_cast<const float*>(kBase + kDefaultAspect), kAspect21x9));
    // automatic aspect at a fixed 1920x1080 internal size: 16:9
    play::display_remove();
    play::DisplaySettings a;
    a.render_width = 1920;
    a.render_height = 1080;
    install(a);
    play::display_apply(a);
    std::memset(proxy.data(), 0, proxy.size());
    ::Ctx c2{};
    c2.x[0] = reinterpret_cast<uint64_t>(mgr.data());
    CHECK_EQ(call(kCameraNodes, &c2), 0);
    CHECK(close_to(read_f(proxy.data() + 152), kAspect16x9));
    play::display_remove();
}

NXTEST(hook_gui_perspective_camera_takes_the_display_aspect) {
    std::vector<uint8_t> cam(0x200, 0);
    *reinterpret_cast<int*>(cam.data() + 264) = 0;                              // perspective
    write_f(cam.data() + 268, 60.0f);
    write_f(cam.data() + 272, 1.7778f);
    write_f(cam.data() + 276, 0.1f);
    write_f(cam.data() + 280, 1000.0f);
    play::DisplaySettings d;
    d.aspect = kAspect21x9;
    install(d);
    play::display_apply(d);
    ::Ctx c{};
    c.x[0] = reinterpret_cast<uint64_t>(cam.data());
    CHECK_EQ(call(kGuiProjection, &c), 0);
    CHECK(close_to(read_f(cam.data() + 272), kAspect21x9));                     // 16:9 widened to the display aspect
    CHECK(close_to(read_f(cam.data() + 268), 60.0));
    CHECK(close_to(read_f(cam.data() + 276), 0.1));
    CHECK(close_to(read_f(cam.data() + 280), 1000.0));
    play::display_remove();
}

NXTEST(hook_gui_orthographic_camera_widens_the_horizontal_extents) {
    std::vector<uint8_t> cam(0x200, 0);
    *reinterpret_cast<int*>(cam.data() + 264) = 1;                              // orthographic
    write_f(cam.data() + 268, -8.0f);
    write_f(cam.data() + 272, 8.0f);
    write_f(cam.data() + 276, -4.5f);
    write_f(cam.data() + 280, 4.5f);
    play::DisplaySettings d;
    d.aspect = kAspect21x9;
    install(d);
    play::display_apply(d);
    ::Ctx c{};
    c.x[0] = reinterpret_cast<uint64_t>(cam.data());
    CHECK_EQ(call(kGuiProjection, &c), 0);
    // width / height = 21 / 9 with a height of 9, centred on 0: -10.5 .. 10.5
    CHECK(close_to(read_f(cam.data() + 268), -10.5));
    CHECK(close_to(read_f(cam.data() + 272), 10.5));
    CHECK(close_to(read_f(cam.data() + 276), -4.5));
    CHECK(close_to(read_f(cam.data() + 280), 4.5));
    play::display_remove();
}

NXTEST(hook_vertical_fov_adds_the_offset) {
    std::vector<uint8_t> cam(0x500, 0);
    write_f(cam.data() + 1120, 1.0f);
    play::DisplaySettings d;
    d.fov_offset = 0;
    install(d);
    ::Ctx none{};
    none.x[0] = reinterpret_cast<uint64_t>(cam.data());
    CHECK_EQ(call(kVerticalFov, &none), 0);                                     // no offset: the game's own value
    play::DisplaySettings wide = d;
    wide.fov_offset = 10;
    play::display_apply(wide);
    ::Ctx c{};
    c.x[0] = reinterpret_cast<uint64_t>(cam.data());
    CHECK_EQ(call(kVerticalFov, &c), 1);
    CHECK(close_to(c.v[0].f32[0], 1.0 + 10.0 * kPi / 180.0, 1e-5));
    write_f(cam.data() + 1120, 100.0f);                                         // not an angle: left alone
    ::Ctx odd{};
    odd.x[0] = reinterpret_cast<uint64_t>(cam.data());
    CHECK_EQ(call(kVerticalFov, &odd), 0);
    play::display_remove();
}

NXTEST(hook_bloom_is_skipped_when_off) {
    play::DisplaySettings d;
    d.bloom = true;
    install(d);
    ::Ctx on{};
    on.x[0] = 5;
    CHECK_EQ(call(kBloom, &on), 0);                                             // on: the game's pass runs
    play::DisplaySettings off = d;
    off.bloom = false;
    play::display_apply(off);
    ::Ctx skip{};
    skip.x[0] = 7;
    CHECK_EQ(call(kBloom, &skip), 1);
    CHECK_EQ(skip.x[0], 0u);
    play::display_remove();
}

NXTEST(hook_chromatic_aberration_zeroes_its_strength_when_off) {
    play::DisplaySettings d;
    d.chromatic_aberration = true;
    install(d);
    float params[4] = {5.0f, 3.0f, 2.0f, 0.0f};
    ::Ctx on{};
    on.x[2] = reinterpret_cast<uint64_t>(params);
    CHECK_EQ(call(kChromatic, &on), 0);
    CHECK(params[0] == 5.0f && params[1] == 3.0f && params[2] == 2.0f && params[3] == 0.0f);
    play::DisplaySettings off = d;
    off.chromatic_aberration = false;
    play::display_apply(off);
    ::Ctx skip{};
    skip.x[2] = reinterpret_cast<uint64_t>(params);
    CHECK_EQ(call(kChromatic, &skip), 0);                                       // the game's pass still runs
    CHECK(params[0] == 0.0f && params[1] == 0.0f);
    CHECK(params[2] == 2.0f && params[3] == 0.0f);
    play::display_remove();
}

NXTEST(display_tuning_sets_the_texture_filtering) {
    play::DisplaySettings d;
    d.anisotropy = 8;
    d.lod_bias = -0.5;
    play::display_tuning(d);
    CHECK_EQ(nxrt::gfx::tuning().anisotropy.load(), 8);
    CHECK(close_to(nxrt::gfx::tuning().lod_bias.load(), -0.5));
    d.lod_bias = 5.0;                                                           // clamped to the top of -2 .. 2
    play::display_tuning(d);
    CHECK(close_to(nxrt::gfx::tuning().lod_bias.load(), 2.0));
    // put the process-wide filtering back to the defaults for the other cases
    d.anisotropy = 0;
    d.lod_bias = 0;
    play::display_tuning(d);
}
