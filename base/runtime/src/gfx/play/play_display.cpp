// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/game/play_display.h"

#include <algorithm>
#include <cmath>

#include "nxrt/gfx_tuning.h"

namespace nxrt::play {

namespace {

int clamp_dim(double v) {
    const long r = std::lround(v);
    const long even = r & ~1L;
    return static_cast<int>(std::clamp<long>(even, kMinRenderDim, kMaxRenderDim));
}

}  // namespace

void internal_size(const DisplaySettings& d, int window_w, int window_h, int* out_w, int* out_h) {
    double w, h;
    if (d.render_width > 0 && d.render_height > 0) {
        w = d.render_width;
        h = d.render_height;
    } else {
        const double s = std::clamp(d.render_scale, 25, 400) / 100.0;
        w = std::max(window_w, 1) * s;
        h = std::max(window_h, 1) * s;
    }
    constexpr double kMaxArea = 8192.0 * 8192.0 * 2.0;
    if (w * h > kMaxArea) {
        const double f = std::sqrt(kMaxArea / (w * h));
        w *= f;
        h *= f;
    }
    *out_w = clamp_dim(w);
    *out_h = clamp_dim(h);
}

double display_aspect(const DisplaySettings& d, int w, int h) {
    if (d.aspect > 0) return std::clamp(d.aspect, kMinAspect, kMaxAspect);
    if (w <= 0 || h <= 0) return kHudAspect;
    return std::clamp(static_cast<double>(w) / h, kMinAspect, kMaxAspect);
}

float fov_with_offset(float base_radians, double offset_degrees) {
    constexpr double kDeg = 3.14159265358979323846 / 180.0;
    const double v = static_cast<double>(base_radians) + offset_degrees * kDeg;
    return static_cast<float>(std::clamp(v, 10.0 * kDeg, 170.0 * kDeg));
}

void display_tuning(const DisplaySettings& d) {
    gfx::tuning().anisotropy.store(d.anisotropy);
    gfx::tuning().lod_bias.store(static_cast<float>(std::clamp(d.lod_bias, -2.0, 2.0)));
}

}  // namespace nxrt::play
