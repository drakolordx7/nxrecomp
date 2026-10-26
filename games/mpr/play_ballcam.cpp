// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode (S48): the morph ball camera turned by the mouse and the right stick, see play_ballcam.h.
#include "play_ballcam.h"

#include <algorithm>
#include <cmath>

namespace nxrt::play {

namespace {
constexpr double kTwoPi = 6.28318530717958647692;
}

bool ballcam_free(uint32_t state, uint32_t behaviour) {
    const bool state_ok = state == 0 || state == 2 || state == 3;   // default, chase, boost (Think runs the collider update for these)
    const bool behaviour_ok = behaviour == 0 || behaviour == 1;     // default, freeze look (hints, fixed, path and spindle cameras are the room's)
    return state_ok && behaviour_ok;
}

void camera_heading_pitch(const float xf[12], double* heading, double* pitch) {
    const double fx = xf[2], fy = xf[6], fz = xf[10];
    *heading = std::atan2(fx, fz);
    const double len = std::sqrt(fx * fx + fy * fy + fz * fz);
    *pitch = len > 1e-9 ? std::asin(std::clamp(fy / len, -1.0, 1.0)) : 0.0;
}

double camera_elevation(const float xf[12], const float pivot[3]) {
    const double dx = xf[3] - pivot[0], dy = xf[7] - pivot[1], dz = xf[11] - pivot[2];
    return std::atan2(dy, std::sqrt(dx * dx + dz * dz));
}

double clamp_ball_pitch(double want, double elev, double min_elev, double max_elev) {
    // Looking up (want > 0) lowers the camera: at most down to min_elev. Looking down raises it: at most up to max_elev. A camera the game already put
    // past a limit gets no offset that way, and never one of the other sign (that would move the view against the player's input).
    if (want > 0) return std::min(want, std::max(0.0, elev - min_elev));
    if (want < 0) return std::max(want, std::min(0.0, elev - max_elev));
    return 0.0;
}

double BallOrbit::begin_think(bool free) {
    if (!free) {          // the room's camera rules: what was asked meanwhile is dropped, yaw and pitch
        reset();
        return 0.0;
    }
    return take_yaw();
}

double BallOrbit::end_think(bool free, double elev) {
    if (!free) return 0.0;
    pitch_ = clamp_ball_pitch(pitch_, elev, kBallMinElevation, kBallMaxElevation);   // a push past the limit is not kept, so the way back is immediate
    return pitch_;
}

void BallOrbit::add_mouse(double dx, double dy, const LookParams& p) {
    add_angles(dx * p.yaw_rad * (p.invert_x ? 1.0 : -1.0), dy * p.pitch_rad * (p.invert_y ? 1.0 : -1.0), p);
}

void BallOrbit::add_angles(double dyaw, double dpitch, const LookParams& p) {
    yaw_ += dyaw;
    if (std::fabs(yaw_) > kTwoPi) yaw_ = std::remainder(yaw_, kTwoPi);
    pitch_ = std::clamp(pitch_ + dpitch, -p.pitch_limit_rad, p.pitch_limit_rad);
}

}  // namespace nxrt::play
