// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode camera look (src/gfx/play/play_camlook.cpp): the offset accumulator and the orbit maths.
#include "test_framework.h"

#include <cmath>
#include <string>

#include "mpr/play_aim.h"
#include "play/play_camlook.h"

NXTEST_GROUP(play_camlook)

namespace {
namespace play = nxrt::play;
constexpr double kPi = 3.14159265358979323846;

#define CHECK_NEAR(got, want, eps)                                                                                  \
    do {                                                                                                            \
        const double nx_g = static_cast<double>(got), nx_w = static_cast<double>(want);                             \
        if (!(std::fabs(nx_g - nx_w) <= (eps)))                                                                     \
            ::nxtest::fail(__FILE__, __LINE__, #got " ~ " #want,                                                    \
                           "got " + std::to_string(nx_g) + ", want " + std::to_string(nx_w));                       \
    } while (0)

// forward of a transform (column vector convention): +Z of the local frame in world space = column 2
void forward(const float xf[12], double f[3]) { f[0] = xf[2]; f[1] = xf[6]; f[2] = xf[10]; }
}  // namespace

NXTEST(offsets_are_linear_and_signed) {
    play::LookOffsets o;
    play::LookParams p;
    o.add(100, 0, p);
    CHECK_NEAR(o.yaw(), -100 * p.yaw_rad, 1e-12);          // mouse right turns right
    o.add(-300, 0, p);
    CHECK_NEAR(o.yaw(), 200 * p.yaw_rad, 1e-12);
    o.add(0, -50, p);
    CHECK_NEAR(o.pitch(), 50 * p.pitch_rad, 1e-12);        // mouse up looks up
    p.invert_x = true;
    p.invert_y = true;
    o.reset();
    o.add(10, 10, p);
    CHECK_NEAR(o.yaw(), 10 * p.yaw_rad, 1e-12);
    CHECK_NEAR(o.pitch(), 10 * p.pitch_rad, 1e-12);
    CHECK(o.active());
    o.reset();
    CHECK(!o.active());
}

NXTEST(offsets_split_the_same_as_one) {
    play::LookParams p;
    play::LookOffsets a, b;
    a.add(1000, -400, p);
    for (int i = 0; i < 10; ++i) b.add(100, -40, p);
    CHECK_NEAR(a.yaw(), b.yaw(), 1e-12);
    CHECK_NEAR(a.pitch(), b.pitch(), 1e-12);
}

NXTEST(pitch_is_clamped) {
    play::LookParams p;
    play::LookOffsets o;
    o.add(0, -1e6, p);
    CHECK_NEAR(o.pitch(), p.pitch_limit_rad, 1e-12);
    o.add(0, 1e7, p);
    CHECK_NEAR(o.pitch(), -p.pitch_limit_rad, 1e-12);
}

NXTEST(orbit_yaw_matches_the_aim_yaw) {
    const float in[12] = {1, 0, 0, 5, 0, 1, 0, 6, 0, 0, 1, 7};
    const float pivot[3] = {5, 6, 7};
    float out[12], aim[12];
    play::orbit_transform(in, pivot, 0.4, 0, out);
    play::rotate_transform_yaw(in, 0.4, aim);
    for (int i = 0; i < 12; ++i) CHECK_NEAR(out[i], aim[i], 1e-6);
    CHECK_NEAR(play::yaw_of_transform(out), 0.4, 1e-6);
}

NXTEST(orbit_about_a_pivot_moves_the_position) {
    // camera 10 behind a ball at the origin (behind = -Z, the camera looks along +Z), turned 90 degrees to the left about up
    const float in[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, -10};
    const float pivot[3] = {0, 0, 0};
    float out[12];
    play::orbit_transform(in, pivot, kPi / 2, 0, out);
    CHECK_NEAR(out[3], -10, 1e-5);    // Ry(+90): (0,0,-10) -> (-10,0,0): the camera is now to the right of the ball, looking along +X (left of the old view)
    CHECK_NEAR(out[11], 0, 1e-5);
    double f[3];
    forward(out, f);
    CHECK_NEAR(f[0], 1, 1e-5);
    CHECK_NEAR(f[2], 0, 1e-5);
    // the camera still looks at the pivot: pivot - position is parallel to the forward vector
    CHECK_NEAR(-out[3] * f[0] / 10.0, 1.0, 1e-5);
}

NXTEST(orbit_pitch_up_looks_up_and_moves_the_camera_below) {
    const float in[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, -10};
    const float pivot[3] = {0, 0, 0};
    float out[12];
    play::orbit_transform(in, pivot, 0, 0.5, out);
    double f[3];
    forward(out, f);
    CHECK_NEAR(f[1], std::sin(0.5), 1e-5);              // looks up by 0.5 rad
    CHECK(out[7] < 0);                                   // the camera sits below the ball
    CHECK_NEAR(std::sqrt(double(out[3]) * out[3] + double(out[7]) * out[7] + double(out[11]) * out[11]), 10.0, 1e-4);   // same distance
}

NXTEST(free_look_about_the_eye_keeps_the_position) {
    const float in[12] = {1, 0, 0, 3, 0, 1, 0, 4, 0, 0, 1, 5};
    const float pivot[3] = {3, 4, 5};
    float out[12];
    play::orbit_transform(in, pivot, -0.3, 0.2, out);
    CHECK_NEAR(out[3], 3, 1e-6);
    CHECK_NEAR(out[7], 4, 1e-6);
    CHECK_NEAR(out[11], 5, 1e-6);
    double f[3];
    forward(out, f);
    CHECK_NEAR(std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]), 1.0, 1e-5);
}

NXTEST(add_angles_signed_and_linear) {
    play::LookParams p;
    play::LookOffsets o;
    o.add_angles(0.3, 0, p);
    o.add_angles(-0.1, 0, p);
    CHECK_NEAR(o.yaw(), 0.2, 1e-12);
    o.reset();
    o.add_angles(0, 0.2, p);
    CHECK_NEAR(o.pitch(), 0.2, 1e-12);
    CHECK_NEAR(o.yaw(), 0.0, 1e-12);
}

NXTEST(add_angles_pitch_clamped) {
    play::LookParams p;
    play::LookOffsets up;
    up.add_angles(0, 5.0, p);
    CHECK_EQ(up.pitch(), p.pitch_limit_rad);
    play::LookOffsets down;
    down.add_angles(0, -10.0, p);
    CHECK_EQ(down.pitch(), -p.pitch_limit_rad);
}

NXTEST(add_angles_yaw_wraps) {
    play::LookParams p;
    play::LookOffsets o;
    o.add_angles(7.0, 0, p);   // 7 - 2 pi = 0.71681...
    CHECK(std::fabs(o.yaw()) <= 2 * kPi);
    CHECK_NEAR(o.yaw(), std::remainder(7.0, 2 * kPi), 1e-12);
}

NXTEST(add_angles_mixes_with_mouse) {
    play::LookParams p;
    play::LookOffsets o;
    o.add(100, 0, p);
    o.add_angles(0.5, 0, p);
    CHECK_NEAR(o.yaw(), -100 * p.yaw_rad + 0.5, 1e-12);
}
