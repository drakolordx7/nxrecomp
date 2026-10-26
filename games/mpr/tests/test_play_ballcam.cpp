// SPDX-License-Identifier: GPL-3.0-or-later
// S48: the morph ball camera turned by the mouse and the right stick (games/mpr/play_ballcam.cpp): the turn collector, the free-camera gate, the
// camera's heading and elevation, the pitch range, and the turn about the ball that the Think hook hands to the game's TeleportCamera.
#include "test_framework.h"

#include <cmath>
#include <string>

#include "mpr/play_ballcam.h"
#include "play/play_camlook.h"

NXTEST_GROUP(play_ballcam)

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

// A camera at `pos` looking at `target` (Y up, forward = column 2, right = column 0, up = column 1; row-major 3x4).
void look_at(const double pos[3], const double target[3], float xf[12]) {
    double f[3] = {target[0] - pos[0], target[1] - pos[1], target[2] - pos[2]};
    const double fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (double& v : f) v /= fl;
    // right = up x forward (Y up, +X left at yaw 0 means right is -X when looking along +Z: up x forward = (0,1,0) x (0,0,1) = (1,0,0))
    double r[3] = {f[2], 0.0, -f[0]};
    const double rl = std::sqrt(r[0] * r[0] + r[2] * r[2]);
    r[0] /= rl;
    r[2] /= rl;
    const double u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
    for (int i = 0; i < 3; ++i) {
        xf[i * 4 + 0] = static_cast<float>(r[i]);
        xf[i * 4 + 1] = static_cast<float>(u[i]);
        xf[i * 4 + 2] = static_cast<float>(f[i]);
        xf[i * 4 + 3] = static_cast<float>(pos[i]);
    }
}

double dist(const float xf[12], const float p[3]) {
    const double dx = xf[3] - p[0], dy = xf[7] - p[1], dz = xf[11] - p[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
}  // namespace

NXTEST(mouse_turn_is_linear_and_signed) {
    play::LookParams p;
    p.pitch_limit_rad = play::kBallMaxElevation;
    play::BallOrbit o;
    o.add_mouse(100, 0, p);
    CHECK_NEAR(o.pending_yaw(), -100 * p.yaw_rad, 1e-12);    // mouse right turns the view right
    o.add_mouse(0, -50, p);
    CHECK_NEAR(o.pitch(), 50 * p.pitch_rad, 1e-12);           // mouse up looks up
    p.invert_x = p.invert_y = true;
    o.reset();
    o.add_mouse(10, 10, p);
    CHECK_NEAR(o.pending_yaw(), 10 * p.yaw_rad, 1e-12);
    CHECK_NEAR(o.pitch(), 10 * p.pitch_rad, 1e-12);
}

NXTEST(turn_does_not_depend_on_how_the_counts_are_split) {
    // the same counts over 1, 10 or 240 frames give the same turn: no smoothing, no acceleration, no frame-rate term
    play::LookParams p;
    p.pitch_limit_rad = play::kBallMaxElevation;
    play::BallOrbit one, ten, many;
    one.add_mouse(1200, -240, p);
    for (int i = 0; i < 10; ++i) ten.add_mouse(120, -24, p);
    for (int i = 0; i < 240; ++i) many.add_mouse(5, -1, p);
    CHECK_NEAR(one.pending_yaw(), ten.pending_yaw(), 1e-12);
    CHECK_NEAR(one.pending_yaw(), many.pending_yaw(), 1e-12);
    CHECK_NEAR(one.pitch(), many.pitch(), 1e-12);
}

NXTEST(take_yaw_hands_over_once) {
    play::LookParams p;
    play::BallOrbit o;
    o.add_mouse(-300, 0, p);
    const double y = o.take_yaw();
    CHECK_NEAR(y, 300 * p.yaw_rad, 1e-12);
    CHECK(o.take_yaw() == 0.0);       // the next Think gets nothing more
    o.add_angles(0.5, 0.0, p);        // a stick turn: radians straight in
    CHECK_NEAR(o.take_yaw(), 0.5, 1e-12);
}

NXTEST(stick_turn_is_rate_times_step) {
    // a full stick at 2 rad/s for one second turns 2 radians whatever the step
    play::LookParams p;
    play::BallOrbit a, b;
    for (int i = 0; i < 60; ++i) a.add_angles(-2.0 / 60.0, 0, p);
    for (int i = 0; i < 240; ++i) b.add_angles(-2.0 / 240.0, 0, p);
    CHECK_NEAR(a.take_yaw(), -2.0, 1e-9);
    CHECK_NEAR(b.take_yaw(), -2.0, 1e-9);
}

NXTEST(pitch_is_held_within_its_limit) {
    play::LookParams p;
    p.pitch_limit_rad = 0.5;
    play::BallOrbit o;
    o.add_mouse(0, -100000, p);
    CHECK_NEAR(o.pitch(), 0.5, 1e-12);
    o.add_mouse(0, 200000, p);
    CHECK_NEAR(o.pitch(), -0.5, 1e-12);
}

NXTEST(free_camera_gate) {
    CHECK(play::ballcam_free(0, 0));     // default
    CHECK(play::ballcam_free(2, 0));     // chase
    CHECK(play::ballcam_free(3, 1));     // boost, freeze look
    CHECK(!play::ballcam_free(1, 0));    // state "one"
    CHECK(!play::ballcam_free(4, 0));    // to the ball
    CHECK(!play::ballcam_free(5, 0));    // from the ball
    for (uint32_t b = 2; b <= 8; ++b) CHECK(!play::ballcam_free(0, b));   // hints, fixed, path and spindle cameras belong to the room
}

NXTEST(heading_and_elevation_of_a_camera) {
    const float ball[3] = {10.f, 1.f, -5.f};
    const double pos[3] = {10.0, 3.0, -9.0};          // 4 behind the ball (on -Z), 2 above it
    const double tgt[3] = {10.0, 1.0, -5.0};
    float xf[12];
    look_at(pos, tgt, xf);
    double h = 0, pt = 0;
    play::camera_heading_pitch(xf, &h, &pt);
    CHECK_NEAR(h, 0.0, 1e-6);                         // looking along +Z
    CHECK_NEAR(pt, -std::atan2(2.0, 4.0), 1e-6);      // looking down at the ball
    CHECK_NEAR(play::camera_elevation(xf, ball), std::atan2(2.0, 4.0), 1e-6);
}

NXTEST(pitch_range_follows_the_camera_height) {
    const double lo = play::kBallMinElevation, hi = play::kBallMaxElevation;
    const double elev = 30.0 * kPi / 180.0;
    CHECK_NEAR(play::clamp_ball_pitch(0.1, elev, lo, hi), 0.1, 1e-12);                 // inside: as asked
    CHECK_NEAR(play::clamp_ball_pitch(1.0, elev, lo, hi), elev - lo, 1e-12);           // looking up lowers the camera: stops 5 degrees above the ball
    CHECK_NEAR(play::clamp_ball_pitch(-2.0, elev, lo, hi), elev - hi, 1e-12);          // looking down raises it: stops at 80 degrees
    CHECK_NEAR(play::clamp_ball_pitch(0.0, 2.0, lo, hi), 2.0 - hi, 1e-12);             // a camera the game put too high is brought into range
}

NXTEST(yaw_turn_moves_the_camera_around_the_ball) {
    // what the Think hook hands to TeleportCamera: the camera keeps its distance and height and still looks at the ball, from another side
    const float ball[3] = {0.f, 0.7f, 0.f};
    const double pos[3] = {0.0, 2.7, -4.5};
    const double tgt[3] = {0.0, 0.7, 0.0};
    float xf[12], out[12];
    look_at(pos, tgt, xf);
    const double turn = 0.5 * kPi;                    // a quarter turn to the left
    play::orbit_transform(xf, ball, turn, 0.0, out);
    CHECK_NEAR(dist(out, ball), dist(xf, ball), 1e-5);
    CHECK_NEAR(out[7], xf[7], 1e-5);                  // same height
    CHECK_NEAR(play::camera_elevation(out, ball), play::camera_elevation(xf, ball), 1e-5);
    double h0 = 0, p0 = 0, h1 = 0, p1 = 0;
    play::camera_heading_pitch(xf, &h0, &p0);
    play::camera_heading_pitch(out, &h1, &p1);
    CHECK_NEAR(std::remainder(h1 - h0 - turn, 2 * kPi), 0.0, 1e-5);   // the view turned left by the turn
    CHECK_NEAR(p1, p0, 1e-5);
    // still looking at the ball: forward points from the camera to the ball
    const double to[3] = {ball[0] - out[3], ball[1] - out[7], ball[2] - out[11]};
    const double tl = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
    CHECK_NEAR(out[2] * to[0] / tl + out[6] * to[1] / tl + out[10] * to[2] / tl, 1.0, 1e-5);
    // the camera's flat forward (what the ball's controls follow) turned by the same angle: stick up rolls toward the new view
    CHECK_NEAR(std::atan2(out[2], out[10]) - std::atan2(xf[2], xf[10]), turn, 1e-5);
}

NXTEST(pitch_offset_changes_the_elevation_by_the_offset) {
    const float ball[3] = {0.f, 0.7f, 0.f};
    const double pos[3] = {3.0, 2.7, -3.0};
    const double tgt[3] = {0.0, 0.7, 0.0};
    float xf[12], out[12];
    look_at(pos, tgt, xf);
    const double e0 = play::camera_elevation(xf, ball);
    const double p = play::clamp_ball_pitch(0.2, e0, play::kBallMinElevation, play::kBallMaxElevation);
    play::orbit_transform(xf, ball, 0.0, p, out);
    CHECK_NEAR(play::camera_elevation(out, ball), e0 - p, 1e-5);   // looking up by p lowers the camera by p around the ball
    CHECK_NEAR(dist(out, ball), dist(xf, ball), 1e-5);
    double h0 = 0, p0 = 0, h1 = 0, p1 = 0;
    play::camera_heading_pitch(xf, &h0, &p0);
    play::camera_heading_pitch(out, &h1, &p1);
    CHECK_NEAR(h1, h0, 1e-5);                         // the heading (and so the ball's controls) is unchanged
    CHECK_NEAR(p1, p0 + p, 1e-5);
}
