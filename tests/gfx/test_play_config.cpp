// SPDX-License-Identifier: GPL-3.0-or-later
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
NXTEST_GROUP(play_config)

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
          ma.menu_touch == mb.menu_touch && ma.wheel_beams == mb.wheel_beams))
        return false;
    const PadSettings &pa = a.pad, &pb = b.pad;
    if (!(pa.label_abxy == pb.label_abxy && pa.stick_deadzone == pb.stick_deadzone && pa.trigger_threshold == pb.trigger_threshold &&
          pa.gyro == pb.gyro && pa.gyro_scale == pb.gyro_scale && pa.rumble == pb.rumble && pa.rumble_scale == pb.rumble_scale))
        return false;
    const TimingSettings &ta = a.timing, &tb = b.timing;
    if (!(ta.tap_ms == tb.tap_ms && ta.hold_ms == tb.hold_ms && ta.double_ms == tb.double_ms && ta.pulse_ms == tb.pulse_ms)) return false;
    const DisplaySettings &da = a.display, &db = b.display;
    if (!(da.window_mode == db.window_mode && da.vsync == db.vsync && da.fps_cap == db.fps_cap && da.width == db.width &&
          da.height == db.height && da.keep_aspect == db.keep_aspect))
        return false;
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

NXTEST(act_table_is_complete) {
    std::set<std::string> names;
    for (int i = 0; i < kActCount; ++i) {
        const ActInfo& info = act_info(static_cast<Act>(i));
        CHECK_EQ((int)info.act, i);
        CHECK(names.insert(info.name).second);   // unique
        Act back = Act::kCount;
        CHECK(parse_act(info.name, &back));
        CHECK_EQ((int)back, i);
    }
    CHECK_EQ(std::string(act_info(Act::None).name), std::string("none"));
    CHECK_EQ(std::string(act_info(Act::MoveForward).name), std::string("move_forward"));
    CHECK_EQ(std::string(act_info(Act::BtnRStick).name), std::string("btn_rstick"));
}

NXTEST(parse_act_is_lenient) {
    Act a = Act::None;
    CHECK(parse_act("FIRE", &a));
    CHECK(a == Act::Fire);
    CHECK(parse_act("move-forward", &a));
    CHECK(a == Act::MoveForward);
    CHECK(parse_act("Dpad-Up", &a));
    CHECK(a == Act::DpadUp);
    CHECK(parse_act("  btn-zl ", &a));
    CHECK(a == Act::BtnZL);
    CHECK(!parse_act("nope", &a));
    CHECK(!parse_act("", &a));
}

NXTEST(ctx_and_trig_names) {
    for (int c = 0; c < kCtxCount; ++c) {
        Ctx back = Ctx::All;
        CHECK(parse_ctx(ctx_name(static_cast<Ctx>(c)), &back));
        CHECK_EQ((int)back, c);
    }
    Ctx m = Ctx::All;
    CHECK(parse_ctx("MENU", &m));
    CHECK(m == Ctx::Menu);
    CHECK(!parse_ctx("lobby", &m));

    Trig t = Trig::Press;
    CHECK(parse_trig("Double_Tap", &t));
    CHECK(t == Trig::DoubleTap);
    CHECK(parse_trig("doubletap", &t));
    CHECK(t == Trig::DoubleTap);
    CHECK(parse_trig("HOLD", &t));
    CHECK(t == Trig::Hold);
    CHECK(!parse_trig("sideways", &t));
    CHECK_EQ(std::string(trig_name(Trig::Toggle)), std::string("toggle"));
}

NXTEST(default_outputs_all_parse) {
    for (int i = 0; i < kActCount; ++i) {
        Output o;
        std::string err;
        CHECK_MSG(parse_output(act_info(static_cast<Act>(i)).default_out, &o, &err), err);
    }
}

// ---- outputs -----------------------------------------------------------------------------------------------------------------

NXTEST(output_parse_terms) {
    Output o;
    REQUIRE(parse_output("ZR", &o));
    CHECK(o == make_out(btn(kBtnZR)));

    REQUIRE(parse_output("X+UP", &o));
    CHECK(o == make_out(btn(kBtnX) | btn(kBtnUp)));
    Output beam1;   // the Beam1 default must be the same output
    REQUIRE(parse_output(act_info(Act::Beam1).default_out, &beam1));
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

NXTEST(output_none_and_cancelation) {
    Output o = make_out(btn(kBtnA));
    CHECK(parse_output("", &o));
    CHECK(o.empty());
    o = make_out(btn(kBtnA));
    CHECK(parse_output("none", &o));
    CHECK(o.empty());
    CHECK(parse_output("  NONE ", &o));
    CHECK(o.empty());

    CHECK(parse_output("LS_UP+LS_DOWN", &o));   // opposite directions cancel
    CHECK(o.empty());
    REQUIRE(parse_output("LS_LEFT+LS_RIGHT+ZL", &o));
    CHECK(o == make_out(btn(kBtnZL)));
    REQUIRE(parse_output("LS_UP+LS_UP", &o));   // a repeated term does not stack
    CHECK(o == make_out(0, 0, 1, 0, 0));
}

NXTEST(output_errors) {
    Output o = make_out(7);
    std::string err;
    CHECK(!parse_output("X+FOO", &o, &err));
    CHECK_EQ(err, std::string("unknown output term 'FOO'"));
    CHECK(o == make_out(7));   // *out is written only on success

    err.clear();
    CHECK(!parse_output("none+X", &o, &err));
    CHECK_EQ(err, std::string("unknown output term 'none'"));
    CHECK(!parse_output("X+", &o, &err));
    CHECK(starts_with(err, "empty term"));
    CHECK(!parse_output("+", &o, nullptr));
    CHECK(!parse_output("LS_UP_", &o, nullptr));   // no null err pointer crash
}

NXTEST(output_text_canonical_and_round_trip) {
    Output o;
    REQUIRE(parse_output("RS_RIGHT+ZR+LS_DOWN", &o));
    CHECK_EQ(output_text(o), std::string("ZR+LS_DOWN+RS_RIGHT"));
    CHECK_EQ(output_text(Output{}), std::string("none"));
    REQUIRE(parse_output("UP+X", &o));
    CHECK_EQ(output_text(o), std::string("X+UP"));

    for (int i = 0; i < kActCount; ++i) {   // every default survives parse -> text -> parse
        Output a, b;
        const char* text = act_info(static_cast<Act>(i)).default_out;
        REQUIRE(parse_output(text, &a));
        REQUIRE(parse_output(output_text(a), &b));
        CHECK(a == b);
    }
}

// ---- inputs ------------------------------------------------------------------------------------------------------------------

NXTEST(input_names_parse_and_round_trip) {
    struct Case {
        const char* text;
        int scancode;
        const char* canonical;
    };
    const Case cases[] = {
        {"w", SDL_SCANCODE_W, "W"},
        {"W", SDL_SCANCODE_W, "W"},
        {"left ctrl", SDL_SCANCODE_LCTRL, "Left Ctrl"},
        {"Left Ctrl", SDL_SCANCODE_LCTRL, "Left Ctrl"},
        {"Space", SDL_SCANCODE_SPACE, "Space"},
        {"Page Up", SDL_SCANCODE_PAGEUP, "PageUp"},   // SDL's own name is "PageUp"; the factory text writes "Page Up"
        {"pageup", SDL_SCANCODE_PAGEUP, "PageUp"},
        {"Keypad Enter", SDL_SCANCODE_KP_ENTER, "Keypad Enter"},
        {"Up", SDL_SCANCODE_UP, "Up"},
        {"1", SDL_SCANCODE_1, "1"},
        {"  f  ", SDL_SCANCODE_F, "F"},
    };
    for (const Case& c : cases) {
        InputId id;
        CHECK_MSG(parse_input(c.text, &id), c.text);
        CHECK_EQ((int)id.kind, (int)InKind::Key);
        CHECK_EQ(id.code, c.scancode);
        CHECK_EQ(input_name(id), std::string(c.canonical));
        InputId again;
        CHECK(parse_input(input_name(id), &again));
        CHECK(again == id);
    }
}

NXTEST(input_mouse_and_wheel) {
    for (int n = 1; n <= 5; ++n) {
        InputId id;
        const std::string name = "Mouse" + std::to_string(n);
        CHECK(parse_input(name, &id));
        CHECK_EQ((int)id.kind, (int)InKind::Mouse);
        CHECK_EQ(id.code, n);
        CHECK_EQ(input_name(id), name);
    }
    InputId m;
    CHECK(parse_input("mouse3", &m));
    CHECK_EQ(m.code, 3);
    InputId up, down;
    CHECK(parse_input("WheelUp", &up));
    CHECK((int)up.kind == (int)InKind::Wheel && up.code == 1);
    CHECK(parse_input("wheeldown", &down));
    CHECK((int)down.kind == (int)InKind::Wheel && down.code == -1);
    CHECK_EQ(input_name(up), std::string("WheelUp"));
    CHECK_EQ(input_name(down), std::string("WheelDown"));
}

NXTEST(input_unknown_names) {
    InputId id;
    CHECK(!parse_input("NoSuchKey", &id));
    CHECK(!parse_input("", &id));
    CHECK(!parse_input("   ", &id));
    CHECK(!parse_input("Mouse6", &id));
    CHECK(!parse_input("Mouse0", &id));
}

NXTEST(input_every_scancode_name_parses_back) {
    for (int sc = 1; sc < SDL_SCANCODE_COUNT; ++sc) {
        const char* n = SDL_GetScancodeName(static_cast<SDL_Scancode>(sc));
        if (!n || !*n) continue;
        InputId id;
        CHECK_MSG(parse_input(n, &id), n);
        // Some names are shared by two scancodes ("Return" is also the keypad key on some layouts), so compare names, not codes.
        CHECK_MSG(input_name(id) == std::string(n), n);
    }
}

// ---- factory settings --------------------------------------------------------------------------------------------------------

NXTEST(default_settings_bindings_and_outputs) {
    const Settings s = default_settings();
    CHECK(act_at(s, "W", Ctx::All) == Act::MoveForward);
    CHECK(act_at(s, "Mouse1", Ctx::All) == Act::Fire);
    CHECK(act_at(s, "Mouse1", Ctx::Menu) == Act::Confirm);
    CHECK(act_at(s, "WheelUp", Ctx::All) == Act::BeamPrev);
    CHECK(act_at(s, "Return", Ctx::Menu) == Act::Confirm);
    CHECK(act_at(s, "Left Ctrl", Ctx::All) == Act::Morph);
    CHECK(act_at(s, "W", Ctx::Menu) == Act::kCount);   // menus have no move binding of their own

    CHECK(s.out[(int)Act::Fire] == make_out(btn(kBtnZR)));
    CHECK(s.out[(int)Act::Beam1] == make_out(btn(kBtnX) | btn(kBtnUp)));
    CHECK(s.out[(int)Act::Confirm] == make_out(btn(kBtnA)));

    CHECK_EQ(s.timing.hold_ms, 250);
    CHECK_EQ(s.display.window_mode, std::string("windowed"));
    CHECK(near(s.mouse.yaw_rad, 0.000767945));
    CHECK(near(s.pad.stick_deadzone, 0.10));
}

NXTEST(default_settings_text_has_no_warnings) {
    std::vector<std::string> warnings;
    Settings s;
    std::string err;
    REQUIRE(parse_settings(default_settings_text(), &s, &err, &warnings));
    CHECK(warnings.empty());
}

// ---- reading the settings file ------------------------------------------------------------------------------------------------

NXTEST(parse_yaw_source_and_radians) {
    Settings s;
    std::string err;
    REQUIRE(parse_settings("[mouse]\nyaw_source = 2.0\npitch = 0.001\n", &s, &err));
    CHECK(near(s.mouse.yaw_rad, rad_per_count(2.0)));
    CHECK(s.mouse.pitch_rad == 0.001);

    REQUIRE(parse_settings("[mouse]\nyaw = 0.001\nyaw_source = 2\n", &s, &err));   // the last one wins
    CHECK(near(s.mouse.yaw_rad, rad_per_count(2.0)));
    REQUIRE(parse_settings("[mouse]\nyaw_source = 2\nyaw = 0.5\n", &s, &err));
    CHECK(s.mouse.yaw_rad == 0.5);
}

NXTEST(file_with_only_mouse_keeps_default_bindings) {
    Settings s;
    std::string err;
    REQUIRE(parse_settings("[mouse]\ninvert_y = 1\n", &s, &err));
    CHECK(s.mouse.invert_y);
    CHECK_EQ(s.bindings.size(), default_settings().bindings.size());
    CHECK(act_at(s, "W", Ctx::All) == Act::MoveForward);
}

NXTEST(one_bindings_section_drops_default_bindings) {
    Settings s;
    std::string err;
    REQUIRE(parse_settings("[bindings.menu]\nReturn = confirm\n", &s, &err));
    CHECK_EQ(s.bindings.size(), 1u);
    CHECK(act_at(s, "Return", Ctx::Menu) == Act::Confirm);
    CHECK(act_at(s, "W", Ctx::All) == Act::kCount);
}

NXTEST(binding_trigger_and_ms_parse) {
    Settings s;
    std::string err;
    REQUIRE(parse_settings("[bindings.gameplay]\nLeft Ctrl = morph hold 250\nSpace = jump toggle\nMouse1 = fire tap\n", &s, &err));
    REQUIRE_MSG(s.bindings.size() == 3u, "three bindings");
    CHECK(s.bindings[0].in == key_id(SDL_SCANCODE_LCTRL));
    CHECK(s.bindings[0].ctx == Ctx::Gameplay);
    CHECK(s.bindings[0].act == Act::Morph);
    CHECK(s.bindings[0].trig == Trig::Hold);
    CHECK_EQ(s.bindings[0].ms, 250);
    CHECK(s.bindings[1].trig == Trig::Toggle);
    CHECK_EQ(s.bindings[1].ms, 0);
    CHECK(s.bindings[2].trig == Trig::Tap);
}

NXTEST(binding_keys_equals_and_semicolon) {
    // The keys "=" and ";" are real SDL names; a line for each must not be cut apart by the '=' split or taken as a comment.
    const char* eq_name = SDL_GetScancodeName(SDL_SCANCODE_EQUALS);
    const char* semi_name = SDL_GetScancodeName(SDL_SCANCODE_SEMICOLON);
    Settings s;
    std::string err;
    const std::string text = std::string("[bindings.menu]\n") + eq_name + " = confirm\n" + semi_name + " = back\n";
    REQUIRE_MSG(parse_settings(text, &s, &err), err);
    CHECK(act_at(s, eq_name, Ctx::Menu) == Act::Confirm);
    CHECK(act_at(s, semi_name, Ctx::Menu) == Act::Back);
}

NXTEST(inline_comments_and_case) {
    Settings s;
    std::string err;
    REQUIRE(parse_settings("[mouse] # the mouse\ninvert_y = 1 ; yes\nwheel_beams = off # no\n", &s, &err));
    CHECK(s.mouse.invert_y);
    CHECK(!s.mouse.wheel_beams);

    REQUIRE(parse_settings("[MOUSE]\nINVERT_X = TRUE\n[Display]\nWindow_Mode = Borderless\n", &s, &err));
    CHECK(s.mouse.invert_x);
    CHECK_EQ(s.display.window_mode, std::string("borderless"));
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

NXTEST(failed_parse_does_not_touch_out) {
    Settings s;
    s.timing.tap_ms = 123;
    std::string err;
    CHECK(!parse_settings("[timing]\ntap_ms = 999999\n", &s, &err));
    CHECK_EQ(s.timing.tap_ms, 123);
}

NXTEST(unknown_keys_and_sections_warn) {
    Settings s;
    std::string err;
    std::vector<std::string> w;
    REQUIRE(parse_settings("[mouse]\nbogus = 1\n[nosuch]\nx = 1\n[bindings.all]\nW = fire\n", &s, &err, &w));
    REQUIRE_MSG(w.size() == 2u, "two warnings");
    CHECK_EQ(w[0], std::string("line 2: unknown key 'bogus' in [mouse]"));
    CHECK_EQ(w[1], std::string("line 3: unknown section '[nosuch]'"));
    CHECK(s.bindings.size() == 1u);   // the unknown section's keys did not add anything
}

// ---- writing and the round trip ----------------------------------------------------------------------------------------------

NXTEST(format_has_sections_in_order) {
    const std::string text = format_settings(default_settings());
    const char* order[] = {"[mouse]", "[pad]", "[timing]", "[display]", "[switch]", "[bindings.all]", "[bindings.gameplay]",
                           "[bindings.morph]", "[bindings.map]", "[bindings.menu]", "[bindings.scan]"};
    size_t at = 0;
    for (const char* h : order) {
        const size_t p = text.find(h, at);
        CHECK_MSG(p != std::string::npos, h);
        if (p == std::string::npos) return;
        at = p;
    }
}

NXTEST(format_parse_round_trip_defaults) {
    const Settings d = default_settings();
    const std::string text = format_settings(d);
    Settings r;
    std::string err;
    REQUIRE_MSG(parse_settings(text, &r, &err), err);
    CHECK(same_settings(d, r));
    CHECK_EQ(format_settings(r), text);
}

NXTEST(format_parse_round_trip_modified) {
    Settings s = default_settings();
    s.mouse.invert_y = true;
    s.mouse.yaw_rad = 0.00125;   // values that %.9g writes exactly
    s.mouse.wheel_beams = false;
    s.pad.stick_deadzone = 0.25;
    s.pad.trigger_threshold = 0.5;
    s.pad.gyro = false;
    s.pad.gyro_scale = 1.5;
    s.pad.rumble_scale = 0.75;
    s.timing.tap_ms = 150;
    s.timing.hold_ms = 400;
    s.display.window_mode = "exclusive";
    s.display.vsync = true;
    s.display.fps_cap = 144;
    s.display.width = 1920;
    s.display.height = 1080;
    s.display.keep_aspect = false;
    REQUIRE(parse_output("A+RS_UP", &s.out[(int)Act::Fire]));
    REQUIRE(parse_output("none", &s.out[(int)Act::Jump]));
    REQUIRE(parse_output("LS_LEFT+LS_RIGHT+X", &s.out[(int)Act::LockOn]));

    s.bindings.clear();
    Binding b;
    b.in = key_id(SDL_SCANCODE_LCTRL);
    b.ctx = Ctx::Gameplay;
    b.act = Act::Morph;
    b.trig = Trig::Hold;
    b.ms = 250;
    s.bindings.push_back(b);
    b = Binding{};
    b.in.kind = InKind::Mouse;
    b.in.code = 4;
    b.ctx = Ctx::All;
    b.act = Act::Missile;
    s.bindings.push_back(b);
    b = Binding{};
    b.in = key_id(SDL_SCANCODE_EQUALS);
    b.ctx = Ctx::Menu;
    b.act = Act::Confirm;
    s.bindings.push_back(b);
    b = Binding{};
    b.in = key_id(SDL_SCANCODE_SEMICOLON);
    b.ctx = Ctx::Menu;
    b.act = Act::Back;
    b.trig = Trig::Tap;
    s.bindings.push_back(b);
    b = Binding{};
    b.in.kind = InKind::Wheel;
    b.in.code = 1;
    b.ctx = Ctx::All;
    b.act = Act::BeamPrev;
    s.bindings.push_back(b);
    b = Binding{};
    b.in = key_id(SDL_SCANCODE_SPACE);
    b.ctx = Ctx::Map;
    b.act = Act::Map;
    b.trig = Trig::Toggle;
    s.bindings.push_back(b);
    b = Binding{};
    b.in = key_id(SDL_SCANCODE_Q);
    b.ctx = Ctx::Morph;
    b.act = Act::Jump;
    b.trig = Trig::DoubleTap;
    b.ms = 300;
    s.bindings.push_back(b);

    const std::string text = format_settings(s);
    Settings r;
    std::string err;
    REQUIRE_MSG(parse_settings(text, &r, &err), err);
    CHECK(same_settings(s, r));
    CHECK_EQ(format_settings(r), text);
}

NXTEST(empty_bindings_stay_empty_after_round_trip) {
    Settings s = default_settings();
    s.bindings.clear();
    const std::string text = format_settings(s);
    CHECK(text.find("[bindings.all]") != std::string::npos);   // the sections are written even when empty
    Settings r;
    std::string err;
    REQUIRE(parse_settings(text, &r, &err));
    CHECK(r.bindings.empty());
}

// ---- load and save -----------------------------------------------------------------------------------------------------------

NXTEST(load_missing_file_gives_defaults_and_no_error) {
    const std::filesystem::path dir = fresh_test_dir("missing");
    const std::string path = (dir / "settings.ini").string();
    Settings s;
    std::string err = "not cleared";
    CHECK(!load_settings(path, &s, &err));
    CHECK(err.empty());
    CHECK(same_settings(s, default_settings()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

NXTEST(load_invalid_file_gives_defaults_and_error) {
    const std::filesystem::path dir = fresh_test_dir("invalid");
    const std::string path = (dir / "settings.ini").string();
    write_file(dir / "settings.ini", "[mouse]\nyaw = nope\n");
    Settings s;
    s.timing.tap_ms = 7;
    std::string err;
    CHECK(!load_settings(path, &s, &err));
    CHECK(err.find(path) != std::string::npos);
    CHECK(err.find("line 2:") != std::string::npos);
    CHECK(same_settings(s, default_settings()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

NXTEST(save_then_load_round_trip) {
    const std::filesystem::path dir = fresh_test_dir("roundtrip");
    const std::string path = (dir / "sub" / "settings.ini").string();   // the folder does not exist yet: save creates it

    Settings s = default_settings();
    s.mouse.invert_x = true;
    s.display.fps_cap = 144;
    std::string err;
    REQUIRE_MSG(save_settings(path, s, &err), err);
    CHECK(!std::filesystem::exists(path + ".tmp"));   // the temp file was renamed away

    Settings r;
    REQUIRE_MSG(load_settings(path, &r, &err), err);
    CHECK(err.empty());
    CHECK(same_settings(r, s));

    // Saving again replaces the file.
    s.display.fps_cap = 30;
    s.bindings.clear();
    REQUIRE_MSG(save_settings(path, s, &err), err);
    Settings r2;
    REQUIRE_MSG(load_settings(path, &r2, &err), err);
    CHECK(same_settings(r2, s));
    CHECK(r2.bindings.empty());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
