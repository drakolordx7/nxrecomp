// SPDX-License-Identifier: GPL-3.0-or-later
// Moved from the base's runtime/tests (gfx/test_play_config.cpp) in the untangle: these cases check Metroid Prime Remastered's tables (title defaults,
// play-mode vocabulary and factory settings, test-tooling probes), so they run with the mpr module registered.
// Play mode: names and parsers (play_types.cpp), factory settings, the settings file reader and writer, load and save (play_config.cpp).
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "nxrt/input_source.h"
#include "play/play_types.h"
#include "test_framework.h"
NXTEST_GROUP(mpr_play_config)

// A REQUIRE with a message (test_framework.h has REQUIRE only).
#define REQUIRE_MSG(cond, msg)                                                        \
    do {                                                                              \
        if (!(cond)) {                                                                \
            ::nxtest::fail(__FILE__, __LINE__, #cond, (msg));                         \
            return;                                                                   \
        }                                                                             \
    } while (0)

namespace {

using namespace nxrt::play;
using nxrt::hid::btn;
using nxrt::hid::kBtnA;
using nxrt::hid::kBtnB;
using nxrt::hid::kBtnDown;
using nxrt::hid::kBtnL;
using nxrt::hid::kBtnLeft;
using nxrt::hid::kBtnMinus;
using nxrt::hid::kBtnPlus;
using nxrt::hid::kBtnR;
using nxrt::hid::kBtnRight;
using nxrt::hid::kBtnStickL;
using nxrt::hid::kBtnStickR;
using nxrt::hid::kBtnUp;
using nxrt::hid::kBtnX;
using nxrt::hid::kBtnY;
using nxrt::hid::kBtnZL;
using nxrt::hid::kBtnZR;

Output make_out(uint64_t buttons, float lx = 0, float ly = 0, float rx = 0, float ry = 0) {
    Output o;
    o.buttons = buttons;
    o.lx = lx;
    o.ly = ly;
    o.rx = rx;
    o.ry = ry;
    return o;
}

InputId key_id(int scancode) {
    InputId id;
    id.kind = InKind::Key;
    id.code = scancode;
    return id;
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-12; }

bool starts_with(const std::string& s, const std::string& prefix) { return s.compare(0, prefix.size(), prefix) == 0; }

// The action bound to `input` in `c` (first match), or Act::kCount when there is no such binding.
Act act_at(const Settings& s, const char* input, Ctx c) {
    InputId id;
    if (!parse_input(input, &id)) return Act::kCount;
    for (const Binding& b : s.bindings) {
        if (b.ctx == c && b.in == id) return b.act;
    }
    return Act::kCount;
}

std::vector<Binding> bindings_in(const Settings& s, int ctx) {
    std::vector<Binding> v;
    for (const Binding& b : s.bindings) {
        if (static_cast<int>(b.ctx) == ctx) v.push_back(b);
    }
    return v;
}

// Field-by-field equality. Bindings are compared per context, because format_settings writes the contexts in a fixed order.
bool same_settings(const Settings& a, const Settings& b) {
    const MouseSettings &ma = a.mouse, &mb = b.mouse;
    if (!(ma.yaw_rad == mb.yaw_rad && ma.pitch_rad == mb.pitch_rad && ma.invert_y == mb.invert_y && ma.invert_x == mb.invert_x &&
          ma.menu_cursor == mb.menu_cursor && ma.wheel_cycles == mb.wheel_cycles))
        return false;
    if (a.aim.lock_mode != b.aim.lock_mode || a.aim.free_lock_margin != b.aim.free_lock_margin || a.camera.gun_drives_view != b.camera.gun_drives_view)
        return false;
    const PadSettings &pa = a.pad, &pb = b.pad;
    if (!(pa.label_abxy == pb.label_abxy && pa.stick_deadzone == pb.stick_deadzone && pa.trigger_threshold == pb.trigger_threshold &&
          pa.gyro == pb.gyro && pa.gyro_scale == pb.gyro_scale && pa.rumble == pb.rumble && pa.rumble_scale == pb.rumble_scale &&
          pa.right_deadzone == pb.right_deadzone && pa.invert_y == pb.invert_y && pa.look_sens == pb.look_sens && pa.alt_orbit == pb.alt_orbit))
        return false;
    const TimingSettings &ta = a.timing, &tb = b.timing;
    if (!(ta.tap_ms == tb.tap_ms && ta.hold_ms == tb.hold_ms && ta.double_ms == tb.double_ms && ta.pulse_ms == tb.pulse_ms)) return false;
    const DisplaySettings &da = a.display, &db = b.display;
    if (!(da.window_mode == db.window_mode && da.vsync == db.vsync && da.fps_cap == db.fps_cap && da.width == db.width &&
          da.height == db.height && da.keep_aspect == db.keep_aspect))
        return false;
    if (!(da.render_scale == db.render_scale && da.render_width == db.render_width && da.render_height == db.render_height &&
          da.aspect == db.aspect && da.fov_offset == db.fov_offset && da.dynamic_resolution == db.dynamic_resolution &&
          da.anisotropy == db.anisotropy && da.lod_bias == db.lod_bias && da.bloom == db.bloom &&
          da.chromatic_aberration == db.chromatic_aberration && da.fast_gpu_memory == db.fast_gpu_memory &&
          da.fewer_gpu_waits == db.fewer_gpu_waits))
        return false;
    const nxrt::extras::Settings &ea = a.extras, &eb = b.extras;
    if (!(ea.hdr.mode == eb.hdr.mode && ea.hdr.format == eb.hdr.format && ea.hdr.peak_nits == eb.hdr.peak_nits &&
          ea.hdr.paper_white_nits == eb.hdr.paper_white_nits && ea.hdr.highlight_boost == eb.hdr.highlight_boost))
        return false;
    if (!(ea.upscale.mode == eb.upscale.mode && ea.upscale.sharpness == eb.upscale.sharpness && ea.upscale.scene_scale == eb.upscale.scene_scale))
        return false;
    if (!(ea.latency.mode == eb.latency.mode && ea.latency.log_timings == eb.latency.log_timings && ea.latency.frames_ahead == eb.latency.frames_ahead)) return false;
    if (a.prompts.style != b.prompts.style) return false;
    const FpsSettings &fa = a.fps, &fb = b.fps;
    if (!(fa.show == fb.show && fa.key == fb.key && fa.corner == fb.corner && fa.detail == fb.detail && fa.in_screenshots == fb.in_screenshots))
        return false;
    for (int i = 0; i < kPadSrcCount; ++i) {
        if (a.pad.remap[i] != b.pad.remap[i]) return false;
    }
    for (int i = 0; i < kActCount; ++i) {
        if (!(a.out[i] == b.out[i])) return false;
    }
    for (int c = 0; c < kCtxCount; ++c) {
        const std::vector<Binding> va = bindings_in(a, c), vb = bindings_in(b, c);
        if (va.size() != vb.size()) return false;
        for (size_t i = 0; i < va.size(); ++i) {
            if (!(va[i].in == vb[i].in && va[i].act == vb[i].act && va[i].trig == vb[i].trig && va[i].ms == vb[i].ms)) return false;
        }
    }
    return true;
}

std::filesystem::path fresh_test_dir(const char* name) {
    const std::filesystem::path p = std::filesystem::temp_directory_path() / "nxrt_play_config_test" / name;
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

void write_file(const std::filesystem::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

}  // namespace

// ---- names -------------------------------------------------------------------------------------------------------------------

NXTEST(parse_act_is_lenient) {
    Act a = Act::None;
    CHECK(parse_act("FIRE", &a));
    CHECK(a == Act::Primary);
    CHECK(parse_act("move-forward", &a));
    CHECK(a == Act::MoveForward);
    CHECK(parse_act("Dpad-Up", &a));
    CHECK(a == Act::DpadUp);
    CHECK(parse_act("  btn-zl ", &a));
    CHECK(a == Act::BtnZL);
    CHECK(!parse_act("nope", &a));
    CHECK(!parse_act("", &a));
}

NXTEST(output_parse_terms) {
    Output o;
    REQUIRE(parse_output("ZR", &o));
    CHECK(o == make_out(btn(kBtnZR)));

    REQUIRE(parse_output("X+UP", &o));
    CHECK(o == make_out(btn(kBtnX) | btn(kBtnUp)));
    Output beam1;   // the Beam1 default must be the same output
    REQUIRE(parse_output(act_info(Act::SetA1).default_out, &beam1));
    CHECK(o == beam1);

    REQUIRE(parse_output(" x + up ", &o));   // case and spaces
    CHECK(o == make_out(btn(kBtnX) | btn(kBtnUp)));

    REQUIRE(parse_output("LS_UP", &o));
    CHECK(o == make_out(0, 0, 1, 0, 0));
    REQUIRE(parse_output("RS_LEFT+RS_DOWN", &o));
    CHECK(o == make_out(0, 0, 0, -1, -1));
    REQUIRE(parse_output("LSTICK+RSTICK+PLUS+MINUS", &o));
    CHECK(o == make_out(btn(kBtnStickL) | btn(kBtnStickR) | btn(kBtnPlus) | btn(kBtnMinus)));
    REQUIRE(parse_output("A+B+L+R+ZL+DOWN+LEFT+RIGHT", &o));
    CHECK(o == make_out(btn(kBtnA) | btn(kBtnB) | btn(kBtnL) | btn(kBtnR) | btn(kBtnZL) | btn(kBtnDown) | btn(kBtnLeft) | btn(kBtnRight)));
    REQUIRE(parse_output("LS_LEFT+RS_RIGHT+Y", &o));
    CHECK(o == make_out(btn(nxrt::hid::kBtnY), -1, 0, 1, 0));
}

NXTEST(default_settings_bindings_and_outputs) {
    const Settings s = default_settings();
    CHECK(act_at(s, "W", Ctx::All) == Act::MoveForward);
    CHECK(act_at(s, "Mouse1", Ctx::All) == Act::Primary);
    CHECK(act_at(s, "Mouse1", Ctx::Menu) == Act::Confirm);
    CHECK(act_at(s, "WheelUp", Ctx::All) == Act::SetAPrev);
    CHECK(act_at(s, "Return", Ctx::Menu) == Act::Confirm);
    CHECK(act_at(s, "Left Ctrl", Ctx::All) == Act::AltForm);
    CHECK(act_at(s, "W", Ctx::Menu) == Act::kCount);   // menus have no move binding of their own

    CHECK(s.out[(int)Act::Primary] == make_out(btn(kBtnZR)));
    CHECK(s.out[(int)Act::SetA1] == make_out(btn(kBtnX) | btn(kBtnUp)));
    CHECK(s.out[(int)Act::Confirm] == make_out(btn(kBtnA)));

    CHECK_EQ(s.timing.hold_ms, 250);
    CHECK_EQ(s.display.window_mode, std::string("windowed"));
    CHECK(near(s.mouse.yaw_rad, 0.000767945));
    CHECK(near(s.pad.stick_deadzone, 0.10));
}

NXTEST(file_with_only_mouse_keeps_default_bindings) {
    Settings s;
    std::string err;
    REQUIRE(parse_settings("[mouse]\ninvert_y = 1\n", &s, &err));
    CHECK(s.mouse.invert_y);
    CHECK_EQ(s.bindings.size(), default_settings().bindings.size());
    CHECK(act_at(s, "W", Ctx::All) == Act::MoveForward);
}

NXTEST(old_factory_lock_mode_moves_to_free_once) {
    // A rev 2 file still on the old factory value takes the new one; a player's own choice in a current file stays.
    Settings s;
    std::string err;
    REQUIRE(parse_settings("[aim]\nlock_mode = classic\n[bindings.all]\nW = fire\n[meta]\ndefaults_rev = 2\n", &s, &err));
    CHECK(s.aim.lock_mode == LockMode::Free);
    CHECK(s.migrated);
    CHECK(s.migration_added.find("lock_mode = free") != std::string::npos);
    Settings k;
    REQUIRE(parse_settings("[aim]\nlock_mode = classic\n[bindings.all]\nW = fire\n[meta]\ndefaults_rev = 3\n", &k, &err));
    CHECK(k.aim.lock_mode == LockMode::Classic);
}

NXTEST(binding_trigger_and_ms_parse) {
    Settings s;
    std::string err;
    REQUIRE(parse_settings("[bindings.gameplay]\nLeft Ctrl = morph hold 250\nSpace = jump toggle\nMouse1 = fire tap\n[meta]\ndefaults_rev = 4\n", &s, &err));
    REQUIRE_MSG(s.bindings.size() == 3u, "three bindings");
    CHECK(s.bindings[0].in == key_id(SDL_SCANCODE_LCTRL));
    CHECK(s.bindings[0].ctx == Ctx::Play);
    CHECK(s.bindings[0].act == Act::AltForm);
    CHECK(s.bindings[0].trig == Trig::Hold);
    CHECK_EQ(s.bindings[0].ms, 250);
    CHECK(s.bindings[1].trig == Trig::Toggle);
    CHECK_EQ(s.bindings[1].ms, 0);
    CHECK(s.bindings[2].trig == Trig::Tap);
}

NXTEST(inline_comments_and_case) {
    Settings s;
    std::string err;
    REQUIRE(parse_settings("[mouse] # the mouse\ninvert_y = 1 ; yes\nwheel_beams = off # no\n", &s, &err));
    CHECK(s.mouse.invert_y);
    CHECK(!s.mouse.wheel_cycles);

    REQUIRE(parse_settings("[MOUSE]\nINVERT_X = TRUE\n[Display]\nWindow_Mode = Borderless\n", &s, &err));
    CHECK(s.mouse.invert_x);
    CHECK_EQ(s.display.window_mode, std::string("borderless"));
}

NXTEST(camera_and_aim_defaults_are_free_and_on) {
    const Settings d = default_settings();
    CHECK(d.aim.lock_mode == LockMode::Free);
    CHECK(d.aim.free_lock_margin == 0.15);
    CHECK(d.camera.gun_drives_view);
}

NXTEST(parse_errors_name_the_line) {
    struct Case {
        const char* text;
        const char* expect;   // the start of err
    };
    const Case cases[] = {
        {"[mouse]\n\n[bindings.all]\nFoo = fire\n", "line 4: unknown input 'Foo'"},
        {"[timing]\ntap_ms = 0\n", "line 2: tap_ms must be 1..5000"},
        {"[bindings.all]\nW = fly\n", "line 2: unknown action 'fly'"},
        {"[bindings.all]\nW = fire sideways\n", "line 2: unknown trigger 'sideways'"},
        {"[bindings.all]\nW = fire hold 0\n", "line 2: ms must be 1..5000"},
        {"[display]\nwindow_mode = fullscreen\n", "line 2: window_mode must be windowed, borderless or exclusive"},
        {"[pad]\ndeadzone = 0.7\n", "line 2: deadzone must be 0..0.5"},
        {"[mouse]\nyaw = nope\n", "line 2: yaw must be a number"},
        {"[switch]\nfire = X+FOO\n", "line 2: unknown output term 'FOO'"},
        {"[bindings.all\nW = fire\n", "line 1: section header without ']'"},
        {"[mouse]\njust words\n", "line 2: expected 'key = value'"},
        {"[display]\nfps_cap = 5000\n", "line 2: fps_cap must be 0..1000"},
    };
    for (const Case& c : cases) {
        Settings s;
        std::string err;
        CHECK(!parse_settings(c.text, &s, &err));
        CHECK_MSG(starts_with(err, c.expect), err);
    }
}

NXTEST(unknown_keys_and_sections_warn) {
    Settings s;
    std::string err;
    std::vector<std::string> w;
    REQUIRE(parse_settings("[mouse]\nbogus = 1\n[nosuch]\nx = 1\n[bindings.all]\nW = fire\n[meta]\ndefaults_rev = 4\n", &s, &err, &w));
    REQUIRE_MSG(w.size() == 2u, "two warnings");
    CHECK_EQ(w[0], std::string("line 2: unknown key 'bogus' in [mouse]"));
    CHECK_EQ(w[1], std::string("line 3: unknown section '[nosuch]'"));
    CHECK(s.bindings.size() == 1u);   // the unknown section's keys did not add anything
}

NXTEST(format_has_sections_in_order) {
    const std::string text = format_settings(default_settings());
    const char* order[] = {"[mouse]", "[pad]", "[timing]", "[display]", "[hdr]", "[upscale]", "[latency]", "[prompts]", "[switch]", "[bindings.all]", "[bindings.gameplay]",
                           "[bindings.morph]", "[bindings.map]", "[bindings.menu]", "[bindings.scan]"};
    size_t at = 0;
    for (const char* h : order) {
        const size_t p = text.find(h, at);
        CHECK_MSG(p != std::string::npos, h);
        if (p == std::string::npos) return;
        at = p;
    }
}

NXTEST(default_settings_text_has_display_keys) {
    const std::string text = default_settings_text();
    const char* lines[] = {"render_scale = 100", "render_width = 0",  "render_height = 0", "aspect = 0",        "fov_offset = 0",
                           "dynamic_resolution = 0", "anisotropy = 0", "lod_bias = 0",    "bloom = 1",       "chromatic_aberration = 1",
                           "fast_gpu_memory = 1",    "fewer_gpu_waits = 1"};
    for (const char* l : lines) CHECK_MSG(text.find(std::string("\n") + l + "\n") != std::string::npos, l);
    // The four that only apply at the next start say so next to their key.
    size_t restart_notes = 0;
    for (size_t p = text.find("applies at the next start"); p != std::string::npos; p = text.find("applies at the next start", p + 1))
        ++restart_notes;
    CHECK(restart_notes >= 4u);
}

NXTEST(default_settings_text_has_prompts_section) {
    const std::string text = default_settings_text();
    const size_t at = text.find("[prompts]");
    REQUIRE_MSG(at != std::string::npos, "[prompts] section");
    CHECK(text.find("style = auto\n", at) != std::string::npos);
}

NXTEST(old_file_gets_new_factory_binding_on_map) {
    Settings s;
    std::string err;
    REQUIRE_MSG(parse_settings("[bindings.map]\nReturn = confirm\n", &s, &err), err);
    CHECK(s.migrated);
    CHECK_EQ(s.defaults_rev, 4);
    CHECK(act_at(s, "Left Shift", Ctx::All) == Act::AltForm);   // rev 4: morph ball on Shift
    CHECK(act_at(s, "X", Ctx::Map) == Act::BtnX);
    CHECK(act_at(s, "Return", Ctx::Map) == Act::Confirm);   // the file's own binding is kept
    CHECK_EQ(static_cast<int>(bindings_in(s, static_cast<int>(Ctx::Map)).size()), 2);
}

NXTEST(old_file_keeps_users_own_key_on_map) {
    Settings s;
    std::string err;
    REQUIRE_MSG(parse_settings("[bindings.map]\nX = back\n", &s, &err), err);
    CHECK(s.migrated);
    CHECK(act_at(s, "X", Ctx::Map) == Act::Back);
    CHECK_EQ(static_cast<int>(bindings_in(s, static_cast<int>(Ctx::Map)).size()), 1);
}

NXTEST(format_round_trip_keeps_defaults_rev) {
    const Settings s = default_settings();
    const std::string text = format_settings(s);
    CHECK(text.find("[meta]\ndefaults_rev = 4\n") != std::string::npos);
    Settings back;
    std::string err;
    REQUIRE_MSG(parse_settings(text, &back, &err), err);
    CHECK_EQ(back.defaults_rev, 4);
    CHECK(!back.migrated);
    CHECK(same_settings(s, back));
}

NXTEST(factory_text_is_at_current_revision) {
    Settings s;
    std::string err;
    REQUIRE_MSG(parse_settings(default_settings_text(), &s, &err), err);
    CHECK_EQ(s.defaults_rev, 4);
    CHECK(!s.migrated);
    CHECK_EQ(default_settings().defaults_rev, 4);
    CHECK(act_at(default_settings(), "Left Shift", Ctx::All) == Act::AltForm);
    CHECK(act_at(default_settings(), "X", Ctx::Map) == Act::BtnX);
}

NXTEST(meta_defaults_rev_is_known_and_unknown_meta_key_warns) {
    Settings s;
    std::string err;
    std::vector<std::string> w;
    REQUIRE_MSG(parse_settings("[meta]\ndefaults_rev = 3\n[bindings.all]\nW = fire\n", &s, &err, &w), err);
    CHECK(w.empty());
    std::vector<std::string> w2;
    REQUIRE_MSG(parse_settings("[meta]\nfoo = 1\n", &s, &err, &w2), err);
    REQUIRE_MSG(w2.size() == 1u, "one warning");
    CHECK_EQ(w2[0], std::string("line 2: unknown key 'foo' in [meta]"));
}

NXTEST(file_without_bindings_gets_factory_bindings_at_current_revision) {
    Settings s;
    std::string err;
    REQUIRE_MSG(parse_settings("[mouse]\nyaw = 0.001\n", &s, &err), err);
    CHECK(!s.migrated);
    CHECK_EQ(s.defaults_rev, 4);
    CHECK(act_at(s, "X", Ctx::Map) == Act::BtnX);
}

NXTEST(pad_stick_keys_range_errors) {
    const struct {
        const char* text;
        const char* expect;
    } cases[] = {
        {"[pad]\nright_deadzone = 0.6\n", "line 2: right_deadzone must be 0..0.5, got 0.6"},
        {"[pad]\nright_deadzone = -0.1\n", "line 2: right_deadzone must be 0..0.5, got -0.1"},
        {"[pad]\nright_deadzone = abc\n", "line 2: right_deadzone must be a number"},
        {"[pad]\nlook_sens = 0.2\n", "line 2: look_sens must be 0.25..3, got 0.2"},
        {"[pad]\nlook_sens = 3.5\n", "line 2: look_sens must be 0.25..3, got 3.5"},
        {"[pad]\nlook_sens = fast\n", "line 2: look_sens must be a number"},
        {"[pad]\ninvert_y = maybe\n", "line 2: invert_y must be 1/0"},
        {"[pad]\nball_orbit = -1\n", "line 2: ball_orbit must be 0..6, got -1"},
        {"[pad]\nball_orbit = 7\n", "line 2: ball_orbit must be 0..6, got 7"},
    };
    for (const auto& c : cases) {
        Settings s;
        std::string err;
        CHECK(!parse_settings(c.text, &s, &err));
        CHECK_MSG(starts_with(err, c.expect), err);
    }
}

NXTEST(ball_camera_keys_with_the_mpr_vocabulary) {
    // the test executable registers Metroid Prime Remastered: its words for the alternate body's camera keys
    REQUIRE(std::string(key_alt_camera()) == "ball_camera");
    REQUIRE(std::string(key_alt_sens()) == "ball_sensitivity");
    REQUIRE(std::string(key_alt_yaw()) == "morph_yaw");
    REQUIRE(std::string(key_alt_pitch()) == "morph_pitch");
    Settings s;
    std::string err;
    REQUIRE_MSG(parse_settings("[mouse]\nball_camera = 0\nball_sensitivity = 4\n", &s, &err), err);
    CHECK(!s.mouse.alt_camera);
    CHECK(near(s.mouse.alt_yaw_rad, rad_per_count(4.0)) && s.mouse.alt_pitch_rad == s.mouse.alt_yaw_rad);
    const std::string text = format_settings(s);
    CHECK(text.find("\nball_camera = 0\nball_sensitivity = 4\n") != std::string::npos);
    CHECK(text.find("morph_yaw") == std::string::npos && text.find("morph_pitch") == std::string::npos);
    // older files: morph_yaw and morph_pitch in radians per count
    REQUIRE_MSG(parse_settings("[mouse]\nmorph_yaw = 0.002\nmorph_pitch = 0.001\n", &s, &err), err);
    CHECK(s.mouse.alt_yaw_rad == 0.002 && s.mouse.alt_pitch_rad == 0.001);
    CHECK(s.mouse.alt_camera);
    // morph_pitch before ball_sensitivity: the later key wins, so both take the sensitivity
    REQUIRE_MSG(parse_settings("[mouse]\nmorph_pitch = 0.001\nball_sensitivity = 3\n", &s, &err), err);
    CHECK(near(s.mouse.alt_pitch_rad, rad_per_count(3.0)) && s.mouse.alt_yaw_rad == s.mouse.alt_pitch_rad);
    // after it: the vertical keeps its own
    REQUIRE_MSG(parse_settings("[mouse]\nball_sensitivity = 3\nmorph_pitch = 0.001\n", &s, &err), err);
    CHECK(near(s.mouse.alt_yaw_rad, rad_per_count(3.0)) && s.mouse.alt_pitch_rad == 0.001);
}

NXTEST(ball_sensitivity_range) {
    Settings s;
    std::string err;
    for (const char* bad : {"0", "-1", "0.05", "25", "1e9"})
        CHECK(!parse_settings(std::string("[mouse]\nball_sensitivity = ") + bad + "\n", &s, &err));
    for (const char* bad : {"0", "-0.001", "1"}) {
        CHECK(!parse_settings(std::string("[mouse]\nmorph_yaw = ") + bad + "\n", &s, &err));
        CHECK(!parse_settings(std::string("[mouse]\nmorph_pitch = ") + bad + "\n", &s, &err));
    }
    for (const char* ok : {"0.1", "2", "20"}) REQUIRE_MSG(parse_settings(std::string("[mouse]\nball_sensitivity = ") + ok + "\n", &s, &err), err);
}

