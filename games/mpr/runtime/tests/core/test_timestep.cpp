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
#include "nxrt/memory.h"
#include "nxrt/timestep.h"
#include "mpr_sites.h"
#include "mpr_timestep.h"     // the sites the registered test game (Metroid Prime Remastered) lists

NXTEST_GROUP(timestep)

namespace ts = nxrt::timestep;
namespace mpr_site = nxrt::games::mpr::site;

#define CHECK_NEAR(actual, expected, tol)                                                                                    \
    do {                                                                                                                     \
        const double nx_a = static_cast<double>(actual);                                                                     \
        const double nx_e = static_cast<double>(expected);                                                                   \
        if (!(std::fabs(nx_a - nx_e) <= (tol)))                                                                              \
            ::nxtest::fail(__FILE__, __LINE__, #actual " ~= " #expected,                                                     \
                           "expected " + num(nx_e) + ", actual " + num(nx_a) + ", tolerance " + num(tol));                   \
    } while (0)

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

constexpr uint64_t kBase = nxrt::mem::kMainBase;

ts::Config base_cfg(ts::Mode m) {
    ts::Config c;
    c.mode = m;
    c.retail_step = mpr_site::kRetailStep;
    return c;
}

// Installs with the game's render counter at buf instead of its address in the guest image.
void install_with_counter(const ts::Config& c, uint8_t* buf) {
    ts::install(c);
    nxrt::games::mpr::timestep_set_render_counter_for_tests(buf);
}

// Leaves the module as it was found, also when a CHECK fails: hooks removed, test clock and deterministic mode reset.
struct Guard {
    Guard() { reset(); }
    ~Guard() { reset(); }
    static void reset() {
        ts::remove();
        nxrt::config_clear();
        ts::set_clock_for_tests(nullptr);
        ts::set_deterministic_for_tests(0);
    }
};

}

NXTEST(scale60_exact) {
    CHECK_EXACT(ts::step_scale(mpr_site::kRetailStep), 1.0f);
    CHECK_EXACT(ts::step_scale(mpr_site::kRetailStep / 2), 0.5f);
    CHECK_EXACT(ts::step_scale(mpr_site::kRetailStep / 4), 0.25f);
}

NXTEST(stepper_off_and_first_frame) {
    ts::Stepper off(base_cfg(ts::Mode::Off));
    CHECK_EXACT(off.step(0.001), static_cast<double>(mpr_site::kRetailStep));
    CHECK_EXACT(off.min_period(), 0.0);

    ts::Stepper real(base_cfg(ts::Mode::Real));
    CHECK_EXACT(real.step(-1.0), static_cast<double>(mpr_site::kRetailStep));
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
    CHECK(d.movie_real);
    CHECK_EXACT(d.retail_step, mpr_site::kRetailStep);

    nxrt::config_load_string(
        "[time]\nmode = fixed\nrate = 120\nsmooth = 8\npace = 0\nmax_rate = 240\nmin_rate = 20\n");
    ts::Config f = ts::load_config();
    CHECK(f.mode == ts::Mode::Fixed);
    CHECK_EXACT(f.rate, 120.0);
    CHECK_EQ(f.smooth, 8);
    CHECK(!f.pace);
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
    const uint64_t b = kBase;
    ::Ctx ctx{};

    g_now = 100.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kUpdate), 0);
    CHECK_EXACT(ts::current_dt(), mpr_site::kRetailStep);

    g_now = 100.0 + 1.0 / 120.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kUpdate), 0);
    CHECK_NEAR(ts::current_dt(), 1.0 / 120.0, 1e-7);
    const float dt = ts::current_dt();

    ctx.x[27] = 0xdeadbeefcafeull;
    CHECK_EQ(nx_site_midhook(&ctx, b + mpr_site::kDtLoop), 0);
    CHECK_EQ(lo32(ctx.x[27]), bits(dt));
    CHECK_EQ(ctx.x[27] >> 32, 0ull);

    ctx.x[8] = 0xdeadbeefcafeull;
    CHECK_EQ(nx_site_midhook(&ctx, b + mpr_site::kDtClamp), 0);
    CHECK_EQ(lo32(ctx.x[8]), bits(dt));
    CHECK_EQ(ctx.x[8] >> 32, 0ull);

    ctx.x[8] = 0xdeadbeefcafeull;
    CHECK_EQ(nx_site_midhook(&ctx, b + mpr_site::kDtFloor), 0);
    CHECK_EQ(lo32(ctx.x[8]), bits(-dt));
    CHECK_EQ(ctx.x[8] >> 32, 0ull);

    const uint64_t budgets[] = {mpr_site::kBudget0, mpr_site::kBudget1, mpr_site::kBudget2, mpr_site::kBudget3};
    for (uint64_t off : budgets) {
        ctx.x[8] = 0xdeadbeefcafeull;
        CHECK_EQ(nx_site_midhook(&ctx, b + off), 0);
        CHECK_NEAR(lo_float(ctx.x[8]), 0.014f * ts::step_scale(dt), 1e-7);
    }

    g_now += 1.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kUpdate), 0);
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
    const uint64_t b = kBase;
    ::Ctx ctx{};

    for (int i = 0; i < 3; i++) {
        CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kUpdate), 0);
        CHECK_NEAR(ts::current_dt(), 1.0 / 120.0, 1e-7);
    }
    CHECK_EQ(ts::stats().waits, 0ull);

    ctx.x[27] = 0;
    CHECK_EQ(nx_site_midhook(&ctx, b + mpr_site::kDtLoop), 0);
    CHECK_EQ(lo32(ctx.x[27]), bits(1.0f / 120.0f));
}

NXTEST(update_hook_fixed) {
    Guard guard;
    ts::set_clock_for_tests(&fake_clock);
    ts::Config c = base_cfg(ts::Mode::Fixed);
    c.rate = 144.0;
    ts::install(c);
    const uint64_t b = kBase;
    ::Ctx ctx{};

    // The clock moves 1 ms per call, less than the 1/144 s period, so the pacing holds the frame.
    g_now = 50.0;
    for (int i = 0; i < 3; i++) {
        CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kUpdate), 0);
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

    const uint64_t b = kBase;
    ::Ctx ctx{};
    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kUpdate), 0);
    ctx.x[27] = 0x1234ull;
    CHECK_EQ(nx_site_midhook(&ctx, b + mpr_site::kDtLoop), 0);
    CHECK_EQ(ctx.x[27], 0x1234ull);
    CHECK(!ts::active());
}

NXTEST(friction_hook) {
    Guard guard;
    ts::set_clock_for_tests(&fake_clock);
    ts::Config c = base_cfg(ts::Mode::Real);
    c.smooth = 1;
    ts::install(c);
    const uint64_t b = kBase;
    ::Ctx ctx{};

    g_now = 300.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kUpdate), 0);
    g_now += 1.0 / 120.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kUpdate), 0);

    ctx.v[1].f32[0] = 3.5f;
    CHECK_EQ(nx_site_midhook(&ctx, b + mpr_site::kFriction), 0);
    CHECK_NEAR(ctx.v[1].f32[0], 3.5f * ts::step_scale(ts::current_dt()), 1e-6);

    ts::remove();
    nxrt::config_load_string("[time]\nfix_friction = 0\n");
    ts::install(c);
    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kUpdate), 0);
    g_now += 1.0 / 120.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kUpdate), 0);
    ctx.v[1].f32[0] = 3.5f;
    CHECK_EQ(nx_site_midhook(&ctx, b + mpr_site::kFriction), 0);
    CHECK_EXACT(ctx.v[1].f32[0], 3.5f);
}

NXTEST(streaming_hooks) {
    Guard guard;
    ts::set_clock_for_tests(&fake_clock);
    ts::Config c = base_cfg(ts::Mode::Real);
    ts::install(c);
    const uint64_t b = kBase;
    ::Ctx ctx{};

    g_now = 400.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kUpdate), 0);
    g_now += 1.0 / 240.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kUpdate), 0);
    const float dt = ts::current_dt();
    CHECK_NEAR(dt, 1.0 / 240.0, 1e-7);

    ctx.x[8] = 0;
    CHECK_EQ(nx_site_midhook(&ctx, b + mpr_site::kStreamA0), 0);
    CHECK_EQ(lo32(ctx.x[8]), bits(dt));

    ctx.x[8] = 0;
    CHECK_EQ(nx_site_midhook(&ctx, b + mpr_site::kStreamA1), 0);
    CHECK_EQ(lo32(ctx.x[8]), bits(2.0f * dt));

    ctx.x[9] = 0;
    CHECK_EQ(nx_site_midhook(&ctx, b + mpr_site::kStreamB0), 0);
    CHECK_EQ(lo32(ctx.x[9]), bits(dt));

    ctx.x[9] = 0;
    CHECK_EQ(nx_site_midhook(&ctx, b + mpr_site::kStreamB1), 0);
    CHECK_EQ(lo32(ctx.x[9]), bits(2.0f * dt));

    ts::remove();
    nxrt::config_load_string("[time]\nfix_stream_step = 0\n");
    ts::install(c);
    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kUpdate), 0);
    g_now += 1.0 / 240.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kUpdate), 0);
    ctx.x[8] = 0x1111222233334444ull;
    ctx.x[9] = 0x5555666677778888ull;
    CHECK_EQ(nx_site_midhook(&ctx, b + mpr_site::kStreamA0), 0);
    CHECK_EQ(nx_site_midhook(&ctx, b + mpr_site::kStreamA1), 0);
    CHECK_EQ(nx_site_midhook(&ctx, b + mpr_site::kStreamB0), 0);
    CHECK_EQ(nx_site_midhook(&ctx, b + mpr_site::kStreamB1), 0);
    CHECK_EQ(ctx.x[8], 0x1111222233334444ull);
    CHECK_EQ(ctx.x[9], 0x5555666677778888ull);
}

NXTEST(render_timings_hook) {
    alignas(16) static uint8_t buf[16];
    Guard guard;
    ts::set_clock_for_tests(&fake_clock);
    g_now = 600.0;
    ::Ctx ctx{};

    std::memset(buf, 0, sizeof buf);
    ts::set_deterministic_for_tests(1.0 / 60.0);
    ts::Config c = base_cfg(ts::Mode::Real);
    install_with_counter(c, buf);
    CHECK_EQ(nx_hook_entry(&ctx, kBase + mpr_site::kUpdate), 0);
    for (int i = 0; i < 3; i++) CHECK_EQ(nx_hook_entry(&ctx, kBase + mpr_site::kTickRenderTimings), 1);
    CHECK_EQ(load_u32(buf), 3u);
    CHECK_EXACT(load_f32(buf + 4), 3.0f / 60.0f);
    CHECK_EXACT(ctx.v[0].f32[0], 3.0f / 60.0f);

    ts::remove();
    std::memset(buf, 0, sizeof buf);
    ts::set_deterministic_for_tests(1.0 / 240.0);
    install_with_counter(c, buf);
    CHECK_EQ(nx_hook_entry(&ctx, kBase + mpr_site::kUpdate), 0);
    for (int i = 0; i < 4; i++) CHECK_EQ(nx_hook_entry(&ctx, kBase + mpr_site::kTickRenderTimings), 1);
    CHECK_EQ(load_u32(buf), 1u);
    CHECK_NEAR(load_f32(buf + 4), 1.0f / 60.0f, 1e-6);

    ts::remove();
    std::memset(buf, 0, sizeof buf);
    store_u32(buf, 52999);
    ts::set_deterministic_for_tests(1.0 / 60.0);
    install_with_counter(c, buf);
    CHECK_EQ(nx_hook_entry(&ctx, kBase + mpr_site::kUpdate), 0);
    CHECK_EQ(nx_hook_entry(&ctx, kBase + mpr_site::kTickRenderTimings), 1);
    CHECK_EQ(load_u32(buf), 0u);
    CHECK_EXACT(load_f32(buf + 4), 0.0f);

    ts::remove();
    std::memset(buf, 0xAB, sizeof buf);
    uint8_t snap[16];
    std::memcpy(snap, buf, sizeof buf);
    ts::set_deterministic_for_tests(1.0 / 60.0);
    nxrt::config_load_string("[time]\nfix_render_timings = 0\n");
    install_with_counter(c, buf);
    CHECK_EQ(nx_hook_entry(&ctx, kBase + mpr_site::kUpdate), 0);
    CHECK_EQ(nx_hook_entry(&ctx, kBase + mpr_site::kTickRenderTimings), 0);
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
    const uint64_t b = kBase;
    ::Ctx ctx{};

    g_now = 50.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kUpdate), 0);

    ctx.v[0].f32[0] = 0.0166f;
    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kMovieUpdate), 0);
    CHECK_EXACT(ctx.v[0].f32[0], 0.0166f);

    g_now = 50.0083;
    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kMovieUpdate), 0);
    CHECK_NEAR(ctx.v[0].f32[0], 0.0083f, 1e-6);

    g_now = 52.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kMovieUpdate), 0);
    CHECK_EXACT(ctx.v[0].f32[0], 0.5f);

    const ts::Stats st = ts::stats();
    CHECK_EQ(st.movie_calls, 3ull);
    CHECK_NEAR(st.movie_total, 0.0166 + 0.0083 + 0.5, 1e-5);

    ts::remove();
    c.movie_real = false;
    ts::install(c);
    g_now = 70.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kUpdate), 0);
    ctx.v[0].f32[0] = 0.25f;
    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kMovieUpdate), 0);
    g_now = 71.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kMovieUpdate), 0);
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
    const uint64_t b = kBase;
    ::Ctx ctx{};

    g_now = 10.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kUpdate), 0);
    g_now = 10.0005;
    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kUpdate), 0);
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
    const uint64_t b = kBase;
    ::Ctx ctx{};

    // The Update hook runs (frames are counted and paced) but the game keeps its own 1/60: active() is false and no register is touched.
    g_now = 5.0;
    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kUpdate), 0);
    g_now = 5.002;
    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kUpdate), 0);
    CHECK(!ts::active());
    CHECK_EQ(ts::stats().frames, 2ull);
    CHECK_EQ(ts::stats().waits, 1ull);                         // 2 ms after the first frame is shorter than the 10 ms period
    ctx.x[27] = 0x1234;
    CHECK_EQ(nx_site_midhook(&ctx, b + mpr_site::kDtLoop), 0);
    CHECK_EQ(ctx.x[27], 0x1234ull);
    ctx.v[0].f32[0] = 0.0166f;
    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kMovieUpdate), 0);
    CHECK_EXACT(ctx.v[0].f32[0], 0.0166f);
    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kTickRenderTimings), 0);
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
        const uint64_t b = kBase;
        ::Ctx ctx{};
        double elapsed_in_game = 0, accum = 0;     // Update's accumulator (this+0xa0)
        int ticks = 0;
        g_now = 1000.0;
        nx_hook_entry(&ctx, b + mpr_site::kUpdate);
        for (int frame = 0; frame < 50; frame++) {
            g_now += 1.0 / hz;
            nx_hook_entry(&ctx, b + mpr_site::kUpdate);
            nx_site_midhook(&ctx, b + mpr_site::kDtLoop);
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

// The stuck tracker takes one sample per 1/60 s of game time: the entry hook returns nonzero ("handled", the call is skipped) for the calls
// in between and 0 for the calls that take a sample.
NXTEST(stuck_tracker_samples_once_per_tick_at_240hz) {
    Guard guard;
    ts::set_clock_for_tests(&fake_clock);
    ts::set_deterministic_for_tests(1.0 / 240.0);
    ts::Config c = base_cfg(ts::Mode::Real);
    ts::install(c);
    const uint64_t b = kBase;
    ::Ctx ctx{};

    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kUpdate), 0);
    CHECK_NEAR(ts::current_dt(), 1.0 / 240.0, 1e-7);

    int zeros = 0;
    for (int i = 1; i <= 12; i++) {
        const int r = nx_hook_entry(&ctx, b + mpr_site::kStuckAddState);
        if (r == 0) zeros++;
        if (i % 4 == 0) CHECK_EQ(r, 0);
        else CHECK(r != 0);
    }
    CHECK_EQ(zeros, 3);
}

NXTEST(stuck_tracker_samples_every_call_at_60hz) {
    Guard guard;
    ts::set_clock_for_tests(&fake_clock);
    ts::set_deterministic_for_tests(1.0 / 60.0);
    ts::Config c = base_cfg(ts::Mode::Real);
    ts::install(c);
    const uint64_t b = kBase;
    ::Ctx ctx{};

    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kUpdate), 0);
    CHECK_NEAR(ts::current_dt(), 1.0 / 60.0, 1e-7);
    for (int i = 0; i < 24; i++) CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kStuckAddState), 0);
}

NXTEST(stuck_tracker_samples_60_of_144) {
    Guard guard;
    ts::set_clock_for_tests(&fake_clock);
    ts::set_deterministic_for_tests(1.0 / 144.0);
    ts::Config c = base_cfg(ts::Mode::Real);
    ts::install(c);
    const uint64_t b = kBase;
    ::Ctx ctx{};

    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kUpdate), 0);
    CHECK_NEAR(ts::current_dt(), 1.0 / 144.0, 1e-7);
    int zeros = 0;
    for (int i = 0; i < 144; i++)
        if (nx_hook_entry(&ctx, b + mpr_site::kStuckAddState) == 0) zeros++;
    CHECK_NEAR(zeros, 60, 1);
}

NXTEST(stuck_tracker_off_by_config) {
    Guard guard;
    ts::set_clock_for_tests(&fake_clock);
    nxrt::config_load_string("[time]\nfix_stuck_tracker = 0\n");
    ts::Config c = ts::load_config();
    ts::set_deterministic_for_tests(1.0 / 240.0);
    ts::install(c);
    const uint64_t b = kBase;
    ::Ctx ctx{};

    CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kUpdate), 0);
    for (int i = 0; i < 12; i++) CHECK_EQ(nx_hook_entry(&ctx, b + mpr_site::kStuckAddState), 0);
    nxrt::config_clear();
}
