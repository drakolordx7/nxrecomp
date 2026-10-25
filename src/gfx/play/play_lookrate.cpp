// SPDX-License-Identifier: GPL-3.0-or-later
#include "play_lookrate.h"

#include <algorithm>
#include <cmath>

namespace nxrt::play {

int look_steps(double* acc, double dt, int max_calls) {
    if (!(dt > 0.0) || !std::isfinite(dt)) return 0;
    *acc += dt;
    int n = static_cast<int>(std::floor(*acc / kLookStep + 0.03));
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

double look_rate_scale(double dt) {
    if (!(dt > 0.0) || !std::isfinite(dt)) return 1.0;
    return std::clamp(dt * 60.0, 0.05, 4.0);
}

}  // namespace nxrt::play
