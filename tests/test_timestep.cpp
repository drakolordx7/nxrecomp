// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the frame-time module (include/nxrt/timestep.h, src/timestep/timestep.cpp): the pure Stepper, the [time] config keys and the
// guest hooks (Update entry, the mid hooks of Update, friction, render timings, streaming, movie clock). The hooks are called directly
// with a fake ::Ctx. A test clock replaces the host clock (which also disables the pacing sleep); deterministic mode is used where the
// step must be exact.
#include "test_framework.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "nxrt/config.h"
#include "nxrt/hooks.h"
#include "nxrt/timestep.h"

NXTEST_GROUP(timestep)

namespace ts = nxrt::timestep;

// Tolerance checks that print the expected and the actual value on failure.
#define CHECK_NEAR(actual, expected, tol)                                                                                    \
    do {                                                                                                                     \
        const double nx_a = static_cast<double>(actual);                                                                     \
        const double nx_e = static_cast<double>(expected);                                                                   \
        if (!(std::fabs(nx_a - nx_e) <= (tol)))                                                                              \
            ::nxtest::fail(__FILE__, __LINE__, #actual " ~= " #expected,                                                     \
                           "expected " + num(nx_e) + ", actual " + num(nx_a) + ", tolerance " + num(tol));                   \
    } while (0)

// Exact checks (floats compared as values, which is bit-exact for the cases used here).
#define CHECK_EXACT(actual, expected)                                                                                        \
    do {                                                                                                                     \
        const double nx_a = static_cast<double>(actual);                                                                     \
        const double nx_e = static_cast<double>(expected);                                                                   \
        if (!(nx_a == nx_e))                                                                                                 \
            ::nxtest::fail(__FILE__, __LINE__, #actual " == " #expected,                                                     \
                           "expected " + num(nx_e) + ", actual " + num(nx_a));                                               \
    } while (0)

namespace {

// The test clock (seconds). The cases that run the Update hook set it.
static double g_now = 0.0;
double fake_clock() { return g_now; }

std::string num(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.10g", v);
    return b;
}

uint32_t bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

uint32_t lo32(uint64_t r) { return static_cast<uint32_t>(r); }

// The low word of a guest x register read as a float (the lifted code reads the float from there).
float lo_float(uint64_t r) {
    const uint32_t w = static_cast<uint32_t>(r);
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}

uint32_t load_u32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

float load_f32(const uint8_t* p) {
    float f;
    std::memcpy(&f, p, 4);
    return f;
}

void store_u32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }

// Config of a test: the default fields with the given mode and the default game base.
ts::Config base_cfg(ts::Mode m) {
    ts::Config c;
    c.mode = m;
    c.game_base = 0x7100000000ull;
    return c;
}

// Config whose render counter sits at buf: game_base + kRenderTimingCounter == buf (unsigned wrap-around is intended).
ts::Config render_cfg(const uint8_t* buf) {
    ts::Config c = base_cfg(ts::Mode::Real);
    c.game_base = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(buf)) - ts::site::kRenderTimingCounter;
    return c;
}

// Leaves the module as it was found, also when a CHECK fails: hooks removed, test clock and deterministic mode reset.
struct Guard {
    Guard() { reset(); }
    ~Guard() { reset(); }
    static void reset() {
        ts::remove();
        ts::set_clock_for_tests(nullptr);
        ts::set_deterministic_for_tests(0);
    }
};

}  // namespace

NXTEST(scale60_exact) {
    CHECK_EXACT(ts::scale60(ts::kGameStep), 1.0f);
    CHECK_EXACT(ts::scale60(ts::kGameStep / 2), 0.5f);
    CHECK_EXACT(ts::scale60(ts::kGameStep / 4), 0.25f);
}

NXTEST(stepper_off_and_first_frame) {
    ts::Stepper off(base_cfg(ts::Mode::Off));
    CHECK_EXACT(off.step(0.001), static_cast<double>(ts::kGameStep));
    CHECK_EXACT(off.min_period(), 0.0);

    ts::Stepper real(base_cfg(ts::Mode::Real));
    CHECK_EXACT(real.step(-1.0), static_cast<double>(ts::kGameStep));
}

NXTEST(stepper_real_raw) {
    ts::Config c = base_cfg(ts::Mode::Real);
    c.smooth = 1;
    c.max_rate = 480.0;
    c.min_rate = 30.0;
    ts::Stepper s(c);

    CHECK_EXACT(s.step(0.008), 0.008);
    CHECK_EXACT(s.step(0.0001), 1.0 / 480.0);
    CHECK_EQ(s.clamped(), 1ull);
    CHECK_EXACT(s.step(0.2), 1.0 / 30.0);
    CHECK_EQ(s.clamped(), 2ull);
}

NXTEST(stepper_real_smooth) {
    ts::Config c = base_cfg(ts::Mode::Real);
    c.smooth = 4;
    ts::Stepper s(c);

    s.step(0.004);
    s.step(0.0042);
    s.step(0.0038);
    s.step(0.004);
    CHECK_NEAR(s.dt(), 0.004, 1e-9);

    // The window slides: the last four frame times are 0.0042, 0.0038, 0.004, 0.0041.
    s.step(0.0041);
    CHECK_NEAR(s.dt(), (0.0042 + 0.0038 + 0.004 + 0.0041) / 4.0, 1e-9);
    CHECK_EQ(s.restarts(), 0ull);

    // A jump of more than 50 % restarts the window: the step is the new frame time alone.
    s.step(0.02);
    CHECK_NEAR(s.dt(), 0.02, 1e-12);
    CHECK_EQ(s.restarts(), 1ull);
}

NXTEST(stepper_fixed) {
    ts::Config c = base_cfg(ts::Mode::Fixed);
    c.rate = 120.0;
    ts::Stepper s(c);
    CHECK_EXACT(s.step(-1.0), 1.0 / 120.0);
    CHECK_EXACT(s.step(0.5), 1.0 / 120.0);
    CHECK_EXACT(s.min_period(), 1.0 / 120.0);
}

NXTEST(config_defaults_and_keys) {
    nxrt::config_load_string("");
    ts::Config d = ts::load_config();
    CHECK(d.mode == ts::Mode::Real);
    CHECK_EQ(d.smooth, 4);
    CHECK_EXACT(d.max_rate, 480.0);
    CHECK_EXACT(d.min_rate, 30.0);
    CHECK(d.pace);
    CHECK(d.fix_friction);
    CHECK(d.fix_render_timings);
    CHECK(d.fix_streaming);
    CHECK(d.fix_budget);
    CHECK(d.movie_real);

    nxrt::config_load_string(
        "[time]\nmode = fixed\nrate = 120\nsmooth = 8\npace = 0\nfix_friction = 0\nmax_rate = 240\nmin_rate = 20\n");
    ts::Config f = ts::load_config();
    CHECK(f.mode == ts::Mode::Fixed);
    CHECK_EXACT(f.rate, 120.0);
    CHECK_EQ(f.smooth, 8);
    CHECK(!f.pace);
    CHECK(!f.fix_friction);
    CHECK(f.fix_render_timings);
    CHECK(f.fix_streaming);
    CHECK(f.fix_budget);
    CHECK(f.movie_real);
    CHECK_EXACT(f.max_rate, 240.0);
    CHECK_EXACT(f.min_rate, 20.0);

    nxrt::config_load_string("[time]\nmode = off\n");
    CHECK(ts::load_config().mode == ts::Mode::Off);

    nxrt::config_clear();
}

NXTEST(update_hook_real_mode) {
    Guard guard;
    ts::set_clock_for_tests(&fake_clock);
    ts::Config c = base_cfg(ts::Mode::Real);
    c.smooth = 1;
    c.pace = true;
    ts::install(c);
    const uint64_t b = c.game_base;
    ::Ctx ctx{};

    // First frame: the game's own step.
    g_now = 100.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kUpdate), 0);
    CHECK_EXACT(ts::current_dt(), ts::kGameStep);

    // Second frame 1/120 s later: the measured step.
    g_now = 100.0 + 1.0 / 120.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kUpdate), 0);
    CHECK_NEAR(ts::current_dt(), 1.0 / 120.0, 1e-7);
    const float dt = ts::current_dt();

    ctx.x[27] = 0xdeadbeefcafeull;
    CHECK_EQ(nx_site_midhook(&ctx, b + ts::site::kDtLoop), 0);
    CHECK_EQ(lo32(ctx.x[27]), bits(dt));
    CHECK_EQ(ctx.x[27] >> 32, 0ull);

    ctx.x[8] = 0xdeadbeefcafeull;
    CHECK_EQ(nx_site_midhook(&ctx, b + ts::site::kDtClamp), 0);
    CHECK_EQ(lo32(ctx.x[8]), bits(dt));
    CHECK_EQ(ctx.x[8] >> 32, 0ull);

    ctx.x[8] = 0xdeadbeefcafeull;
    CHECK_EQ(nx_site_midhook(&ctx, b + ts::site::kDtFloor), 0);
    CHECK_EQ(lo32(ctx.x[8]), bits(-dt));
    CHECK_EQ(ctx.x[8] >> 32, 0ull);

    const uint64_t budgets[] = {ts::site::kBudget0, ts::site::kBudget1, ts::site::kBudget2, ts::site::kBudget3};
    for (uint64_t off : budgets) {
        ctx.x[8] = 0xdeadbeefcafeull;
        CHECK_EQ(nx_site_midhook(&ctx, b + off), 0);
        CHECK_NEAR(lo_float(ctx.x[8]), 0.014f * ts::scale60(dt), 1e-7);
    }

    // A hitch of one second: the step is clamped to the slowest allowed frame, 1/30 s.
    g_now += 1.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kUpdate), 0);
    CHECK_NEAR(ts::current_dt(), 1.0 / 30.0, 1e-7);
    CHECK_EQ(ts::stats().frames, 3ull);
    CHECK(ts::active());
}

NXTEST(update_hook_deterministic) {
    Guard guard;
    ts::set_clock_for_tests(&fake_clock);
    g_now = 200.0;
    ts::set_deterministic_for_tests(1.0 / 120.0);
    ts::Config c = base_cfg(ts::Mode::Real);
    ts::install(c);
    const uint64_t b = c.game_base;
    ::Ctx ctx{};

    // The clock does not move, yet every frame gets the deterministic step and nothing waits.
    for (int i = 0; i < 3; i++) {
        CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kUpdate), 0);
        CHECK_NEAR(ts::current_dt(), 1.0 / 120.0, 1e-7);
    }
    CHECK_EQ(ts::stats().waits, 0ull);

    ctx.x[27] = 0;
    CHECK_EQ(nx_site_midhook(&ctx, b + ts::site::kDtLoop), 0);
    CHECK_EQ(lo32(ctx.x[27]), bits(1.0f / 120.0f));
}

NXTEST(update_hook_fixed) {
    Guard guard;
    ts::set_clock_for_tests(&fake_clock);
    ts::Config c = base_cfg(ts::Mode::Fixed);
    c.rate = 144.0;
    ts::install(c);
    const uint64_t b = c.game_base;
    ::Ctx ctx{};

    // The clock moves 1 ms per call, less than the 1/144 s period, so the pacing holds the frame.
    g_now = 50.0;
    for (int i = 0; i < 3; i++) {
        CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kUpdate), 0);
        CHECK_NEAR(ts::current_dt(), 1.0 / 144.0, 1e-7);
        g_now += 0.001;
    }
    CHECK(ts::stats().waits >= 1);
}

NXTEST(update_hook_off) {
    Guard guard;
    ts::set_clock_for_tests(&fake_clock);
    g_now = 1.0;
    ts::Config c = base_cfg(ts::Mode::Off);
    const size_t before = nxrt::hook_count();
    ts::install(c);
    CHECK_EQ(nxrt::hook_count(), before);

    const uint64_t b = c.game_base;
    ::Ctx ctx{};
    CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kUpdate), 0);
    ctx.x[27] = 0x1234ull;
    CHECK_EQ(nx_site_midhook(&ctx, b + ts::site::kDtLoop), 0);
    CHECK_EQ(ctx.x[27], 0x1234ull);
    CHECK(!ts::active());
}

NXTEST(friction_hook) {
    Guard guard;
    ts::set_clock_for_tests(&fake_clock);
    ts::Config c = base_cfg(ts::Mode::Real);
    c.smooth = 1;
    ts::install(c);
    const uint64_t b = c.game_base;
    ::Ctx ctx{};

    g_now = 300.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kUpdate), 0);
    g_now += 1.0 / 120.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kUpdate), 0);

    ctx.v[1].f32[0] = 3.5f;
    CHECK_EQ(nx_site_midhook(&ctx, b + ts::site::kFriction), 0);
    CHECK_NEAR(ctx.v[1].f32[0], 3.5f * ts::scale60(ts::current_dt()), 1e-6);

    // With fix_friction = 0 the game's 3.5 stays.
    ts::remove();
    c.fix_friction = false;
    ts::install(c);
    CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kUpdate), 0);
    g_now += 1.0 / 120.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kUpdate), 0);
    ctx.v[1].f32[0] = 3.5f;
    CHECK_EQ(nx_site_midhook(&ctx, b + ts::site::kFriction), 0);
    CHECK_EXACT(ctx.v[1].f32[0], 3.5f);
}

NXTEST(streaming_hooks) {
    Guard guard;
    ts::set_clock_for_tests(&fake_clock);
    ts::Config c = base_cfg(ts::Mode::Real);
    ts::install(c);
    const uint64_t b = c.game_base;
    ::Ctx ctx{};

    g_now = 400.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kUpdate), 0);
    g_now += 1.0 / 240.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kUpdate), 0);
    const float dt = ts::current_dt();
    CHECK_NEAR(dt, 1.0 / 240.0, 1e-7);

    ctx.x[8] = 0;
    CHECK_EQ(nx_site_midhook(&ctx, b + ts::site::kStreamA0), 0);
    CHECK_EQ(lo32(ctx.x[8]), bits(dt));

    ctx.x[8] = 0;
    CHECK_EQ(nx_site_midhook(&ctx, b + ts::site::kStreamA1), 0);
    CHECK_EQ(lo32(ctx.x[8]), bits(2.0f * dt));

    ctx.x[9] = 0;
    CHECK_EQ(nx_site_midhook(&ctx, b + ts::site::kStreamB0), 0);
    CHECK_EQ(lo32(ctx.x[9]), bits(dt));

    ctx.x[9] = 0;
    CHECK_EQ(nx_site_midhook(&ctx, b + ts::site::kStreamB1), 0);
    CHECK_EQ(lo32(ctx.x[9]), bits(2.0f * dt));

    // With fix_streaming = 0 the registers are not touched.
    ts::remove();
    c.fix_streaming = false;
    ts::install(c);
    CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kUpdate), 0);
    g_now += 1.0 / 240.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kUpdate), 0);
    ctx.x[8] = 0x1111222233334444ull;
    ctx.x[9] = 0x5555666677778888ull;
    CHECK_EQ(nx_site_midhook(&ctx, b + ts::site::kStreamA0), 0);
    CHECK_EQ(nx_site_midhook(&ctx, b + ts::site::kStreamA1), 0);
    CHECK_EQ(nx_site_midhook(&ctx, b + ts::site::kStreamB0), 0);
    CHECK_EQ(nx_site_midhook(&ctx, b + ts::site::kStreamB1), 0);
    CHECK_EQ(ctx.x[8], 0x1111222233334444ull);
    CHECK_EQ(ctx.x[9], 0x5555666677778888ull);
}

NXTEST(render_timings_hook) {
    alignas(16) static uint8_t buf[16];
    Guard guard;
    ts::set_clock_for_tests(&fake_clock);
    g_now = 600.0;
    ::Ctx ctx{};

    // Deterministic 1/60 s: three calls after one frame give counter 3 and 3/60 s.
    std::memset(buf, 0, sizeof buf);
    ts::set_deterministic_for_tests(1.0 / 60.0);
    ts::Config c = render_cfg(buf);
    ts::install(c);
    CHECK_EQ(nx_hook_entry(&ctx, c.game_base + ts::site::kUpdate), 0);
    for (int i = 0; i < 3; i++) CHECK_EQ(nx_hook_entry(&ctx, c.game_base + ts::site::kTickRenderTimings), 1);
    CHECK_EQ(load_u32(buf), 3u);
    CHECK_EXACT(load_f32(buf + 4), 3.0f / 60.0f);
    CHECK_EXACT(ctx.v[0].f32[0], 3.0f / 60.0f);

    // Deterministic 1/240 s: four calls advance the counter by one frame of 1/60 s.
    ts::remove();
    std::memset(buf, 0, sizeof buf);
    ts::set_deterministic_for_tests(1.0 / 240.0);
    c = render_cfg(buf);
    ts::install(c);
    CHECK_EQ(nx_hook_entry(&ctx, c.game_base + ts::site::kUpdate), 0);
    for (int i = 0; i < 4; i++) CHECK_EQ(nx_hook_entry(&ctx, c.game_base + ts::site::kTickRenderTimings), 1);
    CHECK_EQ(load_u32(buf), 1u);
    CHECK_NEAR(load_f32(buf + 4), 1.0f / 60.0f, 1e-6);

    // The counter wraps at 53000 frames.
    ts::remove();
    std::memset(buf, 0, sizeof buf);
    store_u32(buf, 52999);
    ts::set_deterministic_for_tests(1.0 / 60.0);
    c = render_cfg(buf);
    ts::install(c);
    CHECK_EQ(nx_hook_entry(&ctx, c.game_base + ts::site::kUpdate), 0);
    CHECK_EQ(nx_hook_entry(&ctx, c.game_base + ts::site::kTickRenderTimings), 1);
    CHECK_EQ(load_u32(buf), 0u);
    CHECK_EXACT(load_f32(buf + 4), 0.0f);

    // fix_render_timings = 0: the hook hands the call back and leaves the buffer alone.
    ts::remove();
    std::memset(buf, 0xAB, sizeof buf);
    uint8_t snap[16];
    std::memcpy(snap, buf, sizeof buf);
    ts::set_deterministic_for_tests(1.0 / 60.0);
    c = render_cfg(buf);
    c.fix_render_timings = false;
    ts::install(c);
    CHECK_EQ(nx_hook_entry(&ctx, c.game_base + ts::site::kUpdate), 0);
    CHECK_EQ(nx_hook_entry(&ctx, c.game_base + ts::site::kTickRenderTimings), 0);
    CHECK(std::memcmp(buf, snap, sizeof buf) == 0);
}

NXTEST(movie_hook) {
    Guard guard;
    ts::set_clock_for_tests(&fake_clock);
    ts::Config c = base_cfg(ts::Mode::Real);
    c.smooth = 1;
    c.movie_real = true;
    c.movie_max = 0.5;
    ts::install(c);
    const uint64_t b = c.game_base;
    ::Ctx ctx{};

    g_now = 50.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kUpdate), 0);

    // First movie call: no previous call, so the incoming value stays.
    ctx.v[0].f32[0] = 0.0166f;
    CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kMovieUpdate), 0);
    CHECK_EXACT(ctx.v[0].f32[0], 0.0166f);

    // Real elapsed time since the last call: 8.3 ms.
    g_now = 50.0083;
    CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kMovieUpdate), 0);
    CHECK_NEAR(ctx.v[0].f32[0], 0.0083f, 1e-6);

    // Two seconds later: clamped to movie_max.
    g_now = 52.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kMovieUpdate), 0);
    CHECK_EXACT(ctx.v[0].f32[0], 0.5f);

    const ts::Stats st = ts::stats();
    CHECK_EQ(st.movie_calls, 3ull);
    CHECK_NEAR(st.movie_total, 0.0166 + 0.0083 + 0.5, 1e-5);

    // movie_real = 0: the incoming value is never touched and nothing is counted.
    ts::remove();
    c.movie_real = false;
    ts::install(c);
    g_now = 70.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kUpdate), 0);
    ctx.v[0].f32[0] = 0.25f;
    CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kMovieUpdate), 0);
    g_now = 71.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kMovieUpdate), 0);
    CHECK_EXACT(ctx.v[0].f32[0], 0.25f);
    CHECK_EQ(ts::stats().movie_calls, 0ull);
}

NXTEST(pacing_real_mode_waits) {
    Guard guard;
    ts::set_clock_for_tests(&fake_clock);
    ts::Config c = base_cfg(ts::Mode::Real);
    c.smooth = 1;
    c.pace = true;
    c.max_rate = 480.0;
    ts::install(c);
    const uint64_t b = c.game_base;
    ::Ctx ctx{};

    // The second frame comes 0.5 ms after the first, shorter than the 1/480 s minimum: the frame is held until the minimum has passed.
    g_now = 10.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kUpdate), 0);
    g_now = 10.0005;
    CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kUpdate), 0);
    CHECK_NEAR(ts::current_dt(), 1.0 / 480.0, 1e-7);
    CHECK_EQ(ts::stats().waits, 1ull);
}

NXTEST(retail_off_rate_paces_without_touching_the_step) {
    Guard guard;
    ts::set_clock_for_tests(&fake_clock);
    ts::Config c = base_cfg(ts::Mode::Off);
    c.off_rate = 100.0;          // 10 ms per frame
    c.pace = true;
    ts::install(c);
    const uint64_t b = c.game_base;
    ::Ctx ctx{};

    // The Update hook runs (frames are counted and paced) but the game keeps its own 1/60: active() is false and no register is touched.
    g_now = 5.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kUpdate), 0);
    g_now = 5.002;
    CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kUpdate), 0);
    CHECK(!ts::active());
    CHECK_EQ(ts::stats().frames, 2ull);
    CHECK_EQ(ts::stats().waits, 1ull);                         // 2 ms after the first frame is shorter than the 10 ms period
    ctx.x[27] = 0x1234;
    CHECK_EQ(nx_site_midhook(&ctx, b + ts::site::kDtLoop), 0);
    CHECK_EQ(ctx.x[27], 0x1234ull);
    ctx.v[0].f32[0] = 0.0166f;
    CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kMovieUpdate), 0);
    CHECK_EXACT(ctx.v[0].f32[0], 0.0166f);
    CHECK_EQ(nx_hook_entry(&ctx, b + ts::site::kTickRenderTimings), 0);
}

NXTEST(update_loop_runs_one_tick_per_frame) {
    // One tick per Update call at every rate means one ProcessInput per rendered frame, so the mouse aim (applied by the first ProcessInput after the
    // frame boundary) is applied every frame. Update's accumulator floor and loop compare use the step the mid hooks load, so the do-while body runs once.
    Guard guard;
    ts::set_clock_for_tests(&fake_clock);
    for (double hz : {60.0, 120.0, 144.0, 240.0, 480.0}) {
        ts::remove();
        ts::Config c = base_cfg(ts::Mode::Real);
        c.smooth = 1;
        c.pace = false;
        ts::install(c);
        const uint64_t b = c.game_base;
        ::Ctx ctx{};
        double elapsed_in_game = 0, accum = 0;     // Update's accumulator (this+0xa0)
        int ticks = 0;
        g_now = 1000.0;
        nx_hook_entry(&ctx, b + ts::site::kUpdate);
        for (int frame = 0; frame < 50; frame++) {
            g_now += 1.0 / hz;
            nx_hook_entry(&ctx, b + ts::site::kUpdate);
            nx_site_midhook(&ctx, b + ts::site::kDtLoop);
            float step;
            const uint32_t w = lo32(ctx.x[27]);
            std::memcpy(&step, &w, 4);
            // retail Update: accum += min(elapsed, step); do { v16 = max(accum - step, -step); tick(step); accum = v16 } while (v16 >= step)
            accum += std::min(1.0 / hz, static_cast<double>(step));
            int n = 0;
            double v16;
            do {
                v16 = accum - step;
                if (accum - step + step < 0) v16 = -step;
                n++;
                elapsed_in_game += step;
                accum = v16;
            } while (v16 >= step);
            ticks += n;
            CHECK_EQ(n, 1);
        }
        CHECK_EQ(ticks, 50);
        CHECK_NEAR(elapsed_in_game, 50.0 / hz, 50.0 / hz * 0.01);
    }
}
