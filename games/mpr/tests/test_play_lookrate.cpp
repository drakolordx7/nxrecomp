// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode look clock (src/gfx/play/play_lookrate.cpp): how many 1/60 s calls of the game's pitch code are due for a frame time, and the gyro rate scale.
#include "test_framework.h"

#include <cmath>
#include <limits>
#include <string>

#include "mpr/play_lookrate.h"

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

constexpr double kTol = 0.1;
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

NXTEST(steps_60hz_with_frame_time_jitter_is_one_call_per_frame) {
    // Real timestep, smoothing off: the frame times jitter around the 60 Hz grid. A frame of 15.2 ms followed by one of 18.1 ms used to give 0 calls and then 2.
    {
        double acc = 0.0;
        CHECK_EQ(play::look_steps(&acc, 0.0152), 1);      // 0.912 of a step: inside the 10 percent
        CHECK_EQ(play::look_steps(&acc, 0.0181), 1);
        CHECK_EQ(play::look_steps(&acc, 0.0152), 1);
    }
    // frames on a 60 Hz grid with a timestamp jitter of up to 9 percent of a step: every frame time is 1/60 +- 18 percent, every frame gets exactly one call
    double acc = 0.0, last = 0.0;
    unsigned rng = 12345u;
    long long total = 0;
    for (int i = 1; i <= 3000; ++i) {
        rng = rng * 1664525u + 1013904223u;
        const double j = (static_cast<double>((rng >> 8) & 0xffff) / 65535.0 * 2.0 - 1.0) * 0.09 * play::kLookStep;
        const double t = i * play::kLookStep + j;
        const int n = play::look_steps(&acc, t - last);
        last = t;
        CHECK_MSG(n == 1, "frame " + std::to_string(i) + ": " + std::to_string(n) + " calls");
        total += n;
    }
    CHECK_EQ(total, 3000);
}

NXTEST(steps_alternating_frame_times_keep_the_remainder) {
    // frame times of 7 percent under and over a step alternate (mean exactly 1/60): one call per frame all along, and the early time is taken off the clock
    // again, so over a long run the calls still follow the time
    double acc = 0.0;
    long long total = 0;
    double elapsed = 0.0;
    for (int i = 0; i < 6000; ++i) {
        const double dt = play::kLookStep * ((i % 2 == 0) ? 0.93 : 1.07);
        const int n = play::look_steps(&acc, dt);
        CHECK_MSG(n == 1, "frame " + std::to_string(i));
        total += n;
        elapsed += dt;
    }
    CHECK_NEAR(static_cast<double>(total), elapsed * 60.0, 1.0 + kTol);      // a tenth of a step may still be owed
}

NXTEST(steps_outside_the_tolerance_are_still_skipped) {
    // 80 percent of a step is too early for a call (the tolerance is 10 percent), 90 percent is not
    double acc = 0.0;
    CHECK_EQ(play::look_steps(&acc, 0.8 * play::kLookStep), 0);
    CHECK_EQ(play::look_steps(&acc, 0.1 * play::kLookStep), 1);      // 0.9 + a hair of float noise
    double acc2 = 0.0;
    CHECK_EQ(play::look_steps(&acc2, 0.89 * play::kLookStep), 0);
}

NXTEST(carry_follows_an_accumulating_target) {
    // the target grows by 0.02 per call: half way to the next call it is carried half a call on
    CHECK_NEAR(play::look_carry_target(0.0, 0.02, 0.04, 0.5, 1.5), 0.05, 1e-12);
    CHECK_NEAR(play::look_carry_target(0.0, 0.02, 0.04, 0.0, 1.5), 0.04, 1e-12);      // at 60 steps per second the phase is 0: nothing is added
    CHECK_NEAR(play::look_carry_target(0.0, 0.02, 0.04, 1.0, 1.5), 0.06, 1e-12);
    CHECK_NEAR(play::look_carry_target(0.0, 0.02, 0.04, 7.0, 1.5), 0.06, 1e-12);      // the phase is held to 0..1
    CHECK_NEAR(play::look_carry_target(0.0, 0.02, 0.04, -3.0, 1.5), 0.04, 1e-12);
    // downwards as well
    CHECK_NEAR(play::look_carry_target(0.0, -0.02, -0.04, 0.5, 1.5), -0.05, 1e-12);
    // a speed that changes by less than the ratio window is still carried
    CHECK_NEAR(play::look_carry_target(0.0, 0.02, 0.05, 1.0, 1.5), 0.08, 1e-12);      // 0.02 then 0.03
}

NXTEST(carry_holds_when_the_target_is_not_accumulating) {
    // a stick let go: the target stops, the last speed is zero
    CHECK_NEAR(play::look_carry_target(0.02, 0.04, 0.04, 1.0, 1.5), 0.04, 1e-12);
    // a target the game sets outright (Classic with ZR, Pointer): it jumps from rest, the speed before was zero
    CHECK_NEAR(play::look_carry_target(0.0, 0.0, 0.5, 1.0, 1.5), 0.5, 1e-12);
    // and it stays there
    CHECK_NEAR(play::look_carry_target(0.0, 0.5, 0.5, 1.0, 1.5), 0.5, 1e-12);
    // the direction turns round
    CHECK_NEAR(play::look_carry_target(0.0, 0.04, 0.02, 1.0, 1.5), 0.02, 1e-12);
    CHECK_NEAR(play::look_carry_target(0.04, 0.02, 0.04, 1.0, 1.5), 0.04, 1e-12);
    // a jump far out of the ratio window, either way
    CHECK_NEAR(play::look_carry_target(0.0, 0.01, 0.31, 1.0, 1.5), 0.31, 1e-12);      // 0.01 then 0.30
    CHECK_NEAR(play::look_carry_target(0.0, 0.30, 0.31, 1.0, 1.5), 0.31, 1e-12);      // 0.30 then 0.01
    // bad numbers
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK_NEAR(play::look_carry_target(0.0, 0.02, 0.04, nan, 1.5), 0.04, 1e-12);
    CHECK(std::isnan(play::look_carry_target(0.0, 0.02, nan, 1.0, 1.5)));      // cur itself is handed back as it is
    CHECK_NEAR(play::look_carry_target(nan, 0.02, 0.04, 1.0, 1.5), 0.04, 1e-12);
}

NXTEST(carry_never_goes_past_the_limit) {
    CHECK_NEAR(play::look_carry_target(1.46, 1.48, 1.50, 1.0, 1.5184), 1.5184, 1e-12);      // would be 1.52
    CHECK_NEAR(play::look_carry_target(-1.46, -1.48, -1.50, 1.0, 1.5184), -1.5184, 1e-12);
    CHECK_NEAR(play::look_carry_target(1.46, 1.48, 1.50, 0.5, 1.5184), 1.51, 1e-12);      // inside the limit: not touched
    // a target already past the limit is neither carried further nor pulled back
    CHECK_NEAR(play::look_carry_target(1.56, 1.58, 1.60, 1.0, 1.5184), 1.60, 1e-12);
    CHECK_NEAR(play::look_carry_target(-1.56, -1.58, -1.60, 1.0, 1.5184), -1.60, 1e-12);
    // no limit given
    CHECK_NEAR(play::look_carry_target(1.46, 1.48, 1.50, 1.0, 0.0), 1.52, 1e-12);
}
