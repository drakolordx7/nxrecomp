// SPDX-License-Identifier: GPL-3.0-or-later
#include "hud_math.h"

#include <cmath>

namespace nxrt::play {

namespace {
bool near_ratio(double a, double b) { return std::fabs(a - b) < 0.02; }
}  // namespace

bool hud_perspective(float* aspect, double display_aspect, float* fov_degrees) {
    if (!aspect || !(display_aspect > 0)) return false;
    const double a = *aspect;
    if (!near_ratio(a, kHudAspect) && !near_ratio(a, display_aspect)) return false;
    // A display narrower than 16:9 would cut the sides off a frame drawn for 16:9. Keep the 16:9 width instead and show more above and below:
    // the vertical angle grows so that the horizontal reach stays what the frame was drawn for. Only the stored 16:9 camera is changed (the
    // aspect of an already adjusted camera is the display's), so calling this every frame does not compound.
    if (fov_degrees && display_aspect < kHudAspect - 0.02 && near_ratio(a, kHudAspect) && *fov_degrees > 1.0f && *fov_degrees < 170.0f) {
        constexpr double kPi = 3.14159265358979323846;
        const double half = *fov_degrees * kPi / 360.0;
        const double wide = std::atan(std::tan(half) * (kHudAspect / display_aspect));
        *fov_degrees = static_cast<float>(wide * 360.0 / kPi);
    }
    *aspect = static_cast<float>(display_aspect);
    return true;
}

bool hud_ortho(float* left, float* right, float* bottom, float* top, double display_aspect) {
    if (!left || !right || !bottom || !top || !(display_aspect > 0)) return false;
    // The retail ball HUD stores left = 8 and right = -8 (reversed): the width is the absolute difference and the order is kept.
    const double span = static_cast<double>(*right) - *left;
    const double w = std::fabs(span);
    const double h = std::fabs(static_cast<double>(*top) - *bottom);
    if (!(w > 0) || !(h > 0)) return false;
    const double own = w / h;
    if (!near_ratio(own, kHudAspect) && !near_ratio(own, display_aspect)) return false;
    const double cx = (static_cast<double>(*left) + *right) * 0.5;
    const double dir = span >= 0 ? 1.0 : -1.0;
    if (display_aspect < kHudAspect - 0.02) {
        const double cy = (static_cast<double>(*top) + *bottom) * 0.5;
        const double nh = w / display_aspect;
        const double vdir = (static_cast<double>(*top) - *bottom) >= 0 ? 1.0 : -1.0;
        *bottom = static_cast<float>(cy - vdir * nh * 0.5);
        *top = static_cast<float>(cy + vdir * nh * 0.5);
        return true;
    }
    const double nw = h * display_aspect;
    *left = static_cast<float>(cx - dir * nw * 0.5);
    *right = static_cast<float>(cx + dir * nw * 0.5);
    return true;
}

}  // namespace nxrt::play
