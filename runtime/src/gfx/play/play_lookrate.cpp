// SPDX-License-Identifier: GPL-3.0-or-later
#include "play_lookrate.h"

#include <algorithm>
#include <cmath>

namespace nxrt::play {

int look_steps(double* acc, double dt, int max_calls) {
    if (!(dt > 0.0) || !std::isfinite(dt)) return 0;
    *acc += dt;
    int n = static_cast<int>(std::floor(*acc / kLookStep + kLookTolerance));
    if (n < 0) n = 0;
    if (n > max_calls) {
        n = max_calls;
        *acc = 0.0;
        return n;
    }
    *acc -= n * kLookStep;
    if (*acc < -kLookStep) *acc = -kLookStep;      // cannot happen with the rounding above; keeps a bad value from sticking
    return n;
}

double look_carry_target(double pprev, double prev, double cur, double phase, double limit) {
    if (!std::isfinite(cur) || !std::isfinite(phase)) return cur;
    const double v1 = cur - prev, v0 = prev - pprev;      // the speeds (per call) of the last call and of the one before
    if (!std::isfinite(v0) || !std::isfinite(v1) || !(v0 * v1 > 0.0)) return cur;      // different directions, or one of them at rest
    const double ratio = v1 / v0;
    if (!(ratio >= kCarryRatioMin && ratio <= kCarryRatioMax)) return cur;
    double out = cur + v1 * std::clamp(phase, 0.0, 1.0);
    if (limit > 0.0) out = v1 > 0.0 ? std::min(out, std::max(limit, cur)) : std::max(out, std::min(-limit, cur));
    return out;
}

double look_rate_scale(double dt) {
    if (!(dt > 0.0) || !std::isfinite(dt)) return 1.0;
    return std::clamp(dt * 60.0, 0.05, 4.0);
}

}  // namespace nxrt::play
