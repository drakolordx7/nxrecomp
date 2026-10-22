// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode pad mapping (src/gfx/play/pad_map.cpp): SDL gamepad state to the Switch pad. Pure functions, no SDL calls.
#include "test_framework.h"

#include <cmath>
#include <cstdint>
#include <string>

#include "play/pad_map.h"
#include "nxrt/input_source.h"
#include "nxrt/platform.h"

NXTEST_GROUP(play_pad)

namespace {

namespace hid = nxrt::hid;
namespace platform = nxrt::platform;
namespace play = nxrt::play;

// SDL_GamepadButton bit numbers.
enum : uint32_t {
    kSouth = 0, kEast, kWest, kNorth, kBack, kGuide, kStart, kLeftStick, kRightStick,
    kLeftShoulder, kRightShoulder, kDpadUp, kDpadDown, kDpadLeft, kDpadRight, kMisc1,
};

// Labels as the Xbox and PlayStation pads report them (position SOUTH, EAST, WEST, NORTH), and as a Nintendo pad does.
const uint8_t kXboxLabels[4] = {0, 1, 2, 3};     // A B X Y; PlayStation cross circle square triangle are the same
const uint8_t kNintendoLabels[4] = {1, 0, 3, 2}; // B A Y X

// Maps one raw state with the given settings.
hid::NpadInput run(const platform::GamepadRaw& raw, const play::PadSettings& cfg) {
    hid::NpadInput out;
    play::map_pad(raw, cfg, &out);
    return out;
}

// A pad with only the given SDL button bits down, default labels and axes.
platform::GamepadRaw pad_buttons(uint32_t buttons) {
    platform::GamepadRaw raw;
    raw.buttons = buttons;
    return raw;
}

// Switch buttons for one face button press (position SOUTH..NORTH) on a pad with the given printed labels.
uint64_t face(uint32_t pos, const uint8_t labels[4], bool label_mode) {
    platform::GamepadRaw raw;
    raw.buttons = 1u << pos;
    for (int i = 0; i < 4; ++i) raw.face_label[i] = labels[i];
    play::PadSettings cfg;
    cfg.label_abxy = label_mode;
    return run(raw, cfg).buttons;
}

// Left / right stick of one raw axis pair with default settings (dead zone 0.10).
hid::StickValue left_stick(int16_t x, int16_t y) {
    platform::GamepadRaw raw;
    raw.axes[0] = x;
    raw.axes[1] = y;
    return run(raw, play::PadSettings{}).left;
}
hid::StickValue right_stick(int16_t x, int16_t y) {
    platform::GamepadRaw raw;
    raw.axes[2] = x;
    raw.axes[3] = y;
    return run(raw, play::PadSettings{}).right;
}

bool approx(double a, double b, double eps = 1e-5) { return std::fabs(a - b) <= eps; }

struct ButtonCase {
    uint32_t sdl;
    uint64_t expect;
};

}  // namespace

NXTEST(every_sdl_button_maps) {
    // Default settings: label mode, labels {0,1,2,3}, so SOUTH is Switch A, EAST B, WEST X, NORTH Y.
    const ButtonCase cases[] = {
        {kSouth, hid::btn(hid::kBtnA)},        {kEast, hid::btn(hid::kBtnB)},        {kWest, hid::btn(hid::kBtnX)},
        {kNorth, hid::btn(hid::kBtnY)},        {kBack, hid::btn(hid::kBtnMinus)},    {kGuide, 0},
        {kStart, hid::btn(hid::kBtnPlus)},     {kLeftStick, hid::btn(hid::kBtnStickL)},
        {kRightStick, hid::btn(hid::kBtnStickR)}, {kLeftShoulder, hid::btn(hid::kBtnL)},
        {kRightShoulder, hid::btn(hid::kBtnR)}, {kDpadUp, hid::btn(hid::kBtnUp)},
        {kDpadDown, hid::btn(hid::kBtnDown)},  {kDpadLeft, hid::btn(hid::kBtnLeft)},
        {kDpadRight, hid::btn(hid::kBtnRight)}, {kMisc1, 0},
    };
    for (const ButtonCase& c : cases) {
        const uint64_t got = run(pad_buttons(1u << c.sdl), play::PadSettings{}).buttons;
        CHECK_MSG(got == c.expect, "sdl button " + std::to_string(c.sdl) + " gave " + std::to_string(got));
    }
}

NXTEST(xbox_and_playstation_labels) {
    // Label mode: the printed label names the Switch button. Position mode: SOUTH B, EAST A, WEST Y, NORTH X.
    const uint64_t by_label[4] = {hid::btn(hid::kBtnA), hid::btn(hid::kBtnB), hid::btn(hid::kBtnX), hid::btn(hid::kBtnY)};
    const uint64_t by_pos[4] = {hid::btn(hid::kBtnB), hid::btn(hid::kBtnA), hid::btn(hid::kBtnY), hid::btn(hid::kBtnX)};
    for (uint32_t p = 0; p < 4; ++p) {
        CHECK_MSG(face(p, kXboxLabels, true) == by_label[p], "label mode, position " + std::to_string(p));
        CHECK_MSG(face(p, kXboxLabels, false) == by_pos[p], "position mode, position " + std::to_string(p));
    }
    // Xbox bottom button in the two modes, explicitly.
    CHECK_EQ(face(kSouth, kXboxLabels, true), hid::btn(hid::kBtnA));
    CHECK_EQ(face(kSouth, kXboxLabels, false), hid::btn(hid::kBtnB));
}

NXTEST(nintendo_labels_same_in_both_modes) {
    // A Nintendo pad prints B, A, Y, X on SOUTH, EAST, WEST, NORTH. Both modes must give the same Switch buttons.
    const uint64_t expect[4] = {hid::btn(hid::kBtnB), hid::btn(hid::kBtnA), hid::btn(hid::kBtnY), hid::btn(hid::kBtnX)};
    for (uint32_t p = 0; p < 4; ++p) {
        CHECK_MSG(face(p, kNintendoLabels, true) == expect[p], "label mode, position " + std::to_string(p));
        CHECK_MSG(face(p, kNintendoLabels, false) == expect[p], "position mode, position " + std::to_string(p));
    }
}

NXTEST(stick_center_is_zero) {
    const hid::StickValue l = left_stick(0, 0);
    const hid::StickValue r = right_stick(0, 0);
    CHECK_EQ(l.x, 0);
    CHECK_EQ(l.y, 0);
    CHECK_EQ(r.x, 0);
    CHECK_EQ(r.y, 0);
}

NXTEST(stick_full_deflection_each_direction) {
    // 32767 is 0.99997 after normalising, so the positive extreme comes out at 32766; -32768 is exactly -1.
    const hid::StickValue right = left_stick(32767, 0);
    CHECK_MSG(right.x >= 32765 && right.x <= 32767, "right x " + std::to_string(right.x));
    CHECK_EQ(right.y, 0);
    CHECK_EQ(left_stick(-32768, 0).x, -32767);
    // SDL y is down = +: pushing up (negative) gives hid y up = +.
    const hid::StickValue up = left_stick(0, -32768);
    CHECK_EQ(up.y, 32767);
    CHECK_EQ(up.x, 0);
    const hid::StickValue down = left_stick(0, 32767);
    CHECK_MSG(down.y >= -32767 && down.y <= -32765, "down y " + std::to_string(down.y));
    CHECK_EQ(down.x, 0);
}

NXTEST(stick_dead_zone_edge) {
    // Default dead zone 0.10: 3000 / 32768 = 0.0916 is inside, 3300 / 32768 = 0.1007 is just outside.
    const hid::StickValue inside = left_stick(3000, 0);
    CHECK_EQ(inside.x, 0);
    CHECK_EQ(inside.y, 0);
    const hid::StickValue outside = left_stick(3300, 0);
    CHECK_MSG(outside.x > 0 && outside.x < 1000, "just outside gave " + std::to_string(outside.x));
    CHECK_EQ(outside.y, 0);
}

NXTEST(stick_diagonal_within_unit_circle) {
    // Corner of the square: the radial length is clamped to full deflection, so the result stays inside the circle.
    const hid::StickValue d = left_stick(32767, 32767);
    const double len = std::sqrt(static_cast<double>(d.x) * d.x + static_cast<double>(d.y) * d.y);
    CHECK_MSG(len <= 32767 * 1.0001, "diagonal length " + std::to_string(len));
    CHECK_MSG(len >= 32767 * 0.999, "diagonal length " + std::to_string(len));
    CHECK(d.x > 0);
    CHECK(d.y < 0);   // SDL y down = + : pushing down-right gives hid y negative
}

NXTEST(sticks_are_independent) {
    platform::GamepadRaw raw;
    raw.axes[0] = -32768;   // left stick full left
    raw.axes[1] = 0;
    raw.axes[2] = 0;        // right stick straight up
    raw.axes[3] = -32768;
    const hid::NpadInput out = run(raw, play::PadSettings{});
    CHECK_EQ(out.left.x, -32767);
    CHECK_EQ(out.left.y, 0);
    CHECK_EQ(out.right.x, 0);
    CHECK_EQ(out.right.y, 32767);
}

NXTEST(triggers_threshold) {
    play::PadSettings cfg;   // threshold 0.35: 11000 / 32767 = 0.336 is below, 20000 / 32767 = 0.61 is above
    platform::GamepadRaw raw;
    raw.axes[4] = 11000;
    raw.axes[5] = 11000;
    hid::NpadInput out = run(raw, cfg);
    CHECK_EQ(out.buttons & hid::btn(hid::kBtnZL), 0u);
    CHECK_EQ(out.buttons & hid::btn(hid::kBtnZR), 0u);

    raw.axes[4] = 20000;
    out = run(raw, cfg);
    CHECK(out.buttons & hid::btn(hid::kBtnZL));
    CHECK_EQ(out.buttons & hid::btn(hid::kBtnZR), 0u);

    raw.axes[4] = 0;
    raw.axes[5] = 32767;
    out = run(raw, cfg);
    CHECK_EQ(out.buttons & hid::btn(hid::kBtnZL), 0u);
    CHECK(out.buttons & hid::btn(hid::kBtnZR));
}

NXTEST(map_pad_fills_buttons_and_keeps_other_fields) {
    hid::NpadInput out;
    out.connected = true;
    out.style = hid::NpadStyle::JoyDual;
    out.buttons = 0xFFFF;   // stale value: map_pad replaces it
    out.six_axis.accel[0] = 0.5f;
    out.color_body = 0x12345678;
    out.color_buttons = 0x9ABCDEF0;

    play::map_pad(pad_buttons(0), play::PadSettings{}, &out);
    CHECK_EQ(out.buttons, 0ull);
    CHECK(out.connected);
    CHECK(out.style == hid::NpadStyle::JoyDual);
    CHECK(approx(out.six_axis.accel[0], 0.5f));
    CHECK_EQ(out.color_body, 0x12345678u);
    CHECK_EQ(out.color_buttons, 0x9ABCDEF0u);
}

NXTEST(sensors_rest_pose) {
    const float accel[3] = {0.0f, 0.0f, 9.80665f};   // face up, at rest
    const float gyro[3] = {0.0f, 0.0f, 0.0f};
    hid::SixAxisSample s;
    play::map_sensors(accel, gyro, play::PadSettings{}, &s);
    CHECK(approx(s.accel[0], 0.0));
    CHECK(approx(s.accel[1], 0.0));
    CHECK(approx(s.accel[2], -1.0));
    CHECK(approx(s.gyro[0], 0.0));
    CHECK(approx(s.gyro[1], 0.0));
    CHECK(approx(s.gyro[2], 0.0));
    CHECK(s.at_rest);
}

NXTEST(sensors_gyro_and_axis_flip) {
    // SDL x is flipped: 2 pi rad/s on SDL x is -1 rev/s on hid x. Accel x is flipped too.
    const float accel[3] = {9.80665f, 0.0f, 0.0f};
    const float gyro[3] = {6.2831853f, 0.0f, 0.0f};
    hid::SixAxisSample s;
    play::map_sensors(accel, gyro, play::PadSettings{}, &s);
    CHECK(approx(s.gyro[0], -1.0, 1e-4));
    CHECK(approx(s.gyro[1], 0.0));
    CHECK(approx(s.accel[0], -1.0, 1e-4));
    CHECK(!s.at_rest);
}

NXTEST(sensors_gyro_scale_and_rest_threshold) {
    const float accel[3] = {0.0f, 0.0f, 9.80665f};
    const float gyro[3] = {6.2831853f, 0.0f, 0.0f};
    play::PadSettings cfg;
    cfg.gyro_scale = 0.5;
    hid::SixAxisSample s;
    play::map_sensors(accel, gyro, cfg, &s);
    CHECK(approx(s.gyro[0], -0.5, 1e-4));
    CHECK(!s.at_rest);

    // 0.1 rad/s is 0.0159 rev/s: moving. With gyro_scale 0 the same input is at rest.
    const float slow[3] = {0.1f, 0.0f, 0.0f};
    const float none[3] = {0.0f, 0.0f, 0.0f};
    cfg.gyro_scale = 1.0;
    play::map_sensors(accel, slow, cfg, &s);
    CHECK(!s.at_rest);
    cfg.gyro_scale = 0.0;
    play::map_sensors(accel, gyro, cfg, &s);
    CHECK(s.at_rest);
    CHECK(approx(s.gyro[0], 0.0));
    play::map_sensors(accel, none, cfg, &s);
    CHECK(s.at_rest);
}

NXTEST(sensors_gyro_off_gives_rest_sample) {
    const float accel[3] = {3.0f, -2.0f, 5.0f};
    const float gyro[3] = {6.2831853f, 6.2831853f, 6.2831853f};
    play::PadSettings cfg;
    cfg.gyro = false;
    hid::SixAxisSample s;
    play::map_sensors(accel, gyro, cfg, &s);
    CHECK(approx(s.accel[0], 0.0));
    CHECK(approx(s.accel[1], 0.0));
    CHECK(approx(s.accel[2], -1.0));
    CHECK(approx(s.gyro[0], 0.0));
    CHECK(approx(s.gyro[1], 0.0));
    CHECK(approx(s.gyro[2], 0.0));
    CHECK(s.at_rest);
}

NXTEST(rumble_scale_and_clamp) {
    uint16_t lo = 0, hi = 0;
    play::map_rumble(1.0f, 0.25f, play::PadSettings{}, &lo, &hi);
    CHECK_EQ(lo, 65535);
    CHECK_EQ(hi, 16384);   // 0.25 * 65535 = 16383.75

    play::PadSettings cfg;
    cfg.rumble_scale = 0.25;
    play::map_rumble(1.0f, 1.0f, cfg, &lo, &hi);
    CHECK_EQ(lo, 16384);
    CHECK_EQ(hi, 16384);

    // Amplitudes clamp to 0..1 first, then the scale clamps the product again.
    cfg.rumble_scale = 4.0;
    play::map_rumble(0.5f, 2.0f, cfg, &lo, &hi);
    CHECK_EQ(lo, 65535);
    CHECK_EQ(hi, 65535);
    play::map_rumble(-1.0f, 0.0f, play::PadSettings{}, &lo, &hi);
    CHECK_EQ(lo, 0);
    CHECK_EQ(hi, 0);
}

NXTEST(rumble_off_gives_zero) {
    uint16_t lo = 1234, hi = 1234;
    play::PadSettings cfg;
    cfg.rumble = false;
    play::map_rumble(1.0f, 1.0f, cfg, &lo, &hi);
    CHECK_EQ(lo, 0);
    CHECK_EQ(hi, 0);
}
