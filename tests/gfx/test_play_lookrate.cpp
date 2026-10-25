// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode look clock (src/gfx/play/play_lookrate.cpp): how many 1/60 s calls of the game's pitch code are due for a frame time, and the gyro rate scale.
#include "test_framework.h"

#include <cmath>
#include <limits>
#include <string>

#include "play/play_lookrate.h"

NXTEST_GROUP(play_lookrate)

namespace {
namespace play = nxrt::play;

#define CHECK_NEAR(got, want, eps)                                                                                  \
    do {                                                                                                            \
        const double nx_g = static_cast<double>(got), nx_w = static_cast<double>(want);                             \
        if (!(std::fabs(nx_g - nx_w) <= (eps)))                                                                     \
            ::nxtest::fail(__FILE__, __LINE__, #got " ~ " #want,                                                    \
                           "got " + std::to_string(nx_g) + ", want " + std::to_string(nx_w));                       \
    } while (0)
}  // namespace

NXTEST(steps_at_60) {
    // From an empty clock (what the hook starts with) every tick is exactly one call, with the double 1/60 and with the float-rounded 1/60 the frame time arrives as.
    const double dt = 1.0 / 60.0;
    double acc0 = 0.0;
    int total = 0;
    for (int i = 0; i < 600; ++i) {
        const int n = play::look_steps(&acc0, dt);
        CHECK_EQ(n, 1);
        total += n;
    }
    CHECK_EQ(total, 600);
    double accf = 0.0;
    const double dtf = static_cast<double>(1.0f / 60.0f);
    for (int i = 0; i < 600; ++i) CHECK_EQ(play::look_steps(&accf, dtf), 1);
}

NXTEST(steps_at_120) {
    const double dt = 1.0 / 120.0;
    double acc = 0.0;
    int total = 0;
    for (int i = 1; i <= 1200; ++i) {
        const int n = play::look_steps(&acc, dt);
        CHECK_EQ(n, (i % 2 == 0) ? 1 : 0);                            // 0, 1, 0, 1: every second tick calls
        total += n;
    }
    CHECK_EQ(total, 600);
}

NXTEST(steps_at_30) {
    const double dt = 1.0 / 30.0;
    double acc = 0.0;
    int total = 0;
    for (int i = 1; i <= 300; ++i) {
        const int n = play::look_steps(&acc, dt);
        CHECK_EQ(n, 2);
        total += n;
    }
    CHECK_EQ(total, 600);
}

NXTEST(steps_total_is_rate_independent) {
    const int rates[] = {30, 45, 60, 90, 120, 144, 240, 360};
    for (int rate : rates) {
        double acc = 0.0;
        long long total = 0;
        const double dt = 1.0 / rate;
        for (int i = 0; i < rate * 10; ++i) total += play::look_steps(&acc, dt);
        const long long diff = total - 600;
        CHECK_MSG(diff >= -1 && diff <= 1, "rate " + std::to_string(rate) + ": calls " + std::to_string(total) + ", want 600 +-1");
    }
}

NXTEST(steps_hitch_is_capped) {
    // A 0.5 s frame is 30 steps: 8 calls (the default cap), the rest dropped, the clock empty.
    double acc = 0.0;
    CHECK_EQ(play::look_steps(&acc, 0.5), 8);
    CHECK_NEAR(acc, 0.0, 1e-15);
    CHECK_EQ(play::look_steps(&acc, 1.0 / 60.0), 1);

    // A 0.2 s frame is 12 steps: 3 calls with max_calls = 3.
    double acc3 = 0.0;
    CHECK_EQ(play::look_steps(&acc3, 0.2, 3), 3);
    CHECK_NEAR(acc3, 0.0, 1e-15);
}

NXTEST(steps_bad_dt) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const double bad[] = {0.0, -1.0 / 60.0, nan, inf, -inf};
    for (double dt : bad) {
        double acc = 0.5 * play::kLookStep;
        const double before = acc;
        CHECK_EQ(play::look_steps(&acc, dt), 0);
        CHECK_EQ(acc, before);
    }
}

NXTEST(rate_scale_values) {
    CHECK_NEAR(play::look_rate_scale(1.0 / 60.0), 1.0, 1e-9);
    CHECK_NEAR(play::look_rate_scale(1.0 / 120.0), 0.5, 1e-9);
    CHECK_NEAR(play::look_rate_scale(1.0 / 240.0), 0.25, 1e-9);
    CHECK_NEAR(play::look_rate_scale(1.0 / 30.0), 2.0, 1e-9);
    CHECK_NEAR(play::look_rate_scale(0.001), 0.06, 1e-9);     // 0.06 is inside [0.05, 4.0], so no clamp here
    CHECK_NEAR(play::look_rate_scale(0.0001), 0.05, 1e-9);    // 0.006 clamps up to the floor
    CHECK_NEAR(play::look_rate_scale(1.0), 4.0, 1e-9);        // 60 clamps down to the ceiling
    CHECK_NEAR(play::look_rate_scale(0.0), 1.0, 1e-9);
    CHECK_NEAR(play::look_rate_scale(-1.0), 1.0, 1e-9);
    CHECK_NEAR(play::look_rate_scale(std::numeric_limits<double>::quiet_NaN()), 1.0, 1e-9);
}
