// SPDX-License-Identifier: GPL-3.0-or-later
// Test tooling core (S5): a toy game registered through tt::GameProbes (a world id, a position, hp and play time in host memory, one game field and
// one game step), driven frame by frame through tt::frame_boundary. Covers scripted runs (pass, wait timeout, assert), the virtual clock and RNG,
// memory-region hashing, @shot, key steps, and the game side of the interface: its state fields in the hash and the trace, its steps and state keys.
// The tables of a real game are tested in that game's folder (games/<id>/runtime/tests).
#include "test_framework.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/dispatch.h"
#include "nxrt/kernel.h"
#include "nxrt/game.h"
#include "nxrt/tt.h"
#include "fixture_game.h"

NXTEST_GROUP(tt_core)

namespace {

using namespace nxrt::tt;

const char* kWorld = "4b5e8635-0f43-4ffb-9427-2e62186e517d";
const char* kAreaA = "4ca69978-0000-4000-8000-000000000001";
const char* kAreaB = "4ca69978-0000-4000-8000-000000000002";

// A toy game: its state lives in this struct (the probes read it), its simulation is step(): UP walks +y, the left stick strafes +x, A raises z, igt counts.
// Registers itself as the game while it lives (the base tests run with the neutral fixture game otherwise).
struct Rig {
    uint8_t world[16] = {};
    uint8_t area[16] = {};
    float pos[3] = {0, 0, 0};
    float hp = 99.0f;
    double igt = 0;
    int32_t mode = 0;               // a game field: what the step @bump changes
    int bumps = 0;
    std::string out;
    int exit_code_seen = -999;
    const nxrt::game::Info* prev = nullptr;
    nxrt::game::Info info;
    static Rig* cur;

    explicit Rig(const char* name);
    ~Rig() {
        nxrt::game::set_current(prev);
        cur = nullptr;
    }
    // one frame of the toy game, using the pad of the frame that just began
    void step(const PadFrame& p) {
        if (p.buttons & (1ull << 13)) pos[1] += 0.5f;
        pos[0] += static_cast<float>(p.lx) / 32767.0f * 0.25f;
        if (p.buttons & 1ull) pos[2] += 0.125f;
        igt += 1.0 / 60.0;
    }
};
Rig* Rig::cur = nullptr;

bool toy_read_state(GameState* g, ::Ctx*) {
    Rig* r = Rig::cur;
    if (!r) return false;
    std::memcpy(g->world, r->world, 16);
    std::memcpy(g->area, r->area, 16);
    g->have_world = g->have_player = g->have_igt = g->have_hp = true;
    for (int i = 0; i < 3; i++) g->pos[i] = r->pos[i];
    g->hp = r->hp;
    g->igt = r->igt;
    g->add_int("mode", r->mode);
    g->add_float("fuel", 0.5 * r->mode, r->mode != 0, StateField::Hash::None);       // printed only once the mode is set; not hashed
    g->add_int("lives", 3, true, StateField::Hash::High);                              // shares the word of "mode"
    return true;
}

// @bump <n>: adds n to the game's mode (parse: a count; run: at once). @lift: waits until z >= 0.25, fails after the timeout.
bool toy_parse_bump(const StepLine& l, Step* s, std::string* err) {
    if (l.t.size() != 2) return l.usage("@bump <n>", err);
    return l.count(l.t[1], &s->count, err);
}
StepStatus toy_run_bump(StepRun& r) {
    Rig::cur->mode += static_cast<int32_t>(r.step.count);
    Rig::cur->bumps++;
    return StepStatus::Done;
}
bool toy_parse_lift(const StepLine&, Step*, std::string*) { return true; }
StepStatus toy_run_lift(StepRun& r) {
    if (Rig::cur->pos[2] >= 0.25f) return StepStatus::Done;
    if (r.timed_out()) { *r.error = "never lifted"; return StepStatus::Fail; }
    return StepStatus::Wait;
}
const GameStepDef kToySteps[] = {{"@bump", toy_parse_bump, toy_run_bump}, {"@lift", toy_parse_lift, toy_run_lift}};
const char* const kToyKeys[] = {"mode", "fuel", nullptr};
const GameProbes kToyProbes = [] {
    GameProbes p;
    p.read_state = toy_read_state;
    p.state_keys = kToyKeys;
    p.steps = kToySteps;
    p.step_count = 2;
    return p;
}();

Rig::Rig(const char* name) {
    cur = this;
    out = std::string("tt_test_out\\") + name;
    tt_guid_to_bytes(kWorld, world);
    tt_guid_to_bytes(kAreaA, area);
    info = nxtest::fixture_game();
    info.probes = &kToyProbes;
    prev = nxrt::game::current();
    nxrt::game::set_current(&info);
}
void on_exit_code(int code) { if (Rig::cur) Rig::cur->exit_code_seen = code; }

std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
void spit(const std::string& p, const std::string& t) { std::ofstream f(p, std::ios::binary); f << t; }

std::string scriptfile(const char* name, const std::string& text) {
    CreateDirectoryA("tt_test_out", nullptr);
    const std::string p = std::string("tt_test_out\\") + name;
    spit(p, text);
    return p;
}

void begin(Rig& r, std::initializer_list<std::pair<const char*, std::string>> kv) {
    nxrt::config_clear();
    reset_for_tests();
    set_exit_handler(on_exit_code);
    nxrt::config_set("test", "out", r.out.c_str());
    nxrt::config_set("test", "frame_source", "manual");
    for (auto& p : kv) nxrt::config_set("test", p.first, p.second.c_str());
    r.exit_code_seen = -999;
    init();
}

void run_frames(Rig& r, int max_frames) {
    ::Ctx ctx{};
    for (int i = 0; i < max_frames && !finished(); i++) {
        frame_boundary(&ctx);
        if (finished()) break;
        r.step(current_pad());
    }
}

}  // namespace

NXTEST(wait_timeout_fails_with_the_script_line) {
    Rig r("timeout");
    const std::string s = scriptfile("timeout.nxs", "3\n@area " + std::string(kAreaB) + " timeout=12\n@pass\n");
    begin(r, {{"script", s}});
    run_frames(r, 100);
    CHECK_EQ(r.exit_code_seen, 1);
    const std::string res = slurp(r.out + "\\result.json");
    CHECK(res.find("\"verdict\":\"fail\"") != std::string::npos);
    CHECK(res.find("timeout.nxs:2:") != std::string::npos);
    CHECK(res.find("\"fail_line\":2") != std::string::npos);
    shutdown();
}

NXTEST(assert_failure_ends_the_run) {
    Rig r("assert");
    const std::string s = scriptfile("assert.nxs", "2\n@assert health < 50\n@pass\n");
    begin(r, {{"script", s}});
    run_frames(r, 50);
    CHECK_EQ(r.exit_code_seen, 1);
    CHECK(slurp(r.out + "\\result.json").find("assertion failed") != std::string::npos);
    shutdown();
}

NXTEST(parse_error_is_exit_code_2) {
    Rig r("parse");
    const std::string s = scriptfile("bad.nxs", "5\nfrobnicate 3\n");
    begin(r, {{"script", s}});
    CHECK_EQ(r.exit_code_seen, 2);
    CHECK(slurp(r.out + "\\result.json").find("bad.nxs:2") != std::string::npos);
    shutdown();
}

NXTEST(a_different_input_gives_a_different_hash) {
    uint64_t h[2];
    const char* scripts[2] = {"5\nhold UP\n10\n@pass\n", "5\nhold UP\n11\n@pass\n"};
    for (int i = 0; i < 2; i++) {
        Rig r("diff");
        begin(r, {{"script", scriptfile("diff.nxs", scripts[i])}, {"deterministic", "1"}});
        run_frames(r, 200);
        h[i] = chain_hash();
        shutdown();
    }
    CHECK(h[0] != h[1]);
}

NXTEST(virtual_clock_steps_per_frame) {
    Rig r("clock");
    begin(r, {{"deterministic", "1"}, {"trace", "none"}});
    uint64_t t0 = 0, t1 = 0, t2 = 0;
    REQUIRE(virtual_tick(&t0));
    REQUIRE(virtual_tick(&t1));
    CHECK_EQ(t1, t0 + 1);                                  // every read advances time by one tick
    ::Ctx ctx{};
    for (int i = 0; i < 4; i++) frame_boundary(&ctx);      // frames 0..3
    REQUIRE(virtual_tick(&t2));
    CHECK(t2 >= t0 + 3 * 320000);                          // three full steps since frame 0
    CHECK(t2 < t0 + 4 * 320000);
    const uint64_t before = t2;
    virtual_sleep(1000000);                                // 1 ms = 19200 ticks, no real wait
    uint64_t t3 = 0;
    virtual_tick(&t3);
    CHECK(t3 >= before + 19200);
    // A sleep on another thread does not move the clock: it waits for the frames (here: none come) for at most its length of host time.
    uint64_t t4 = 0, t5 = 0;
    virtual_tick(&t4);
    std::thread other([] { for (int i = 0; i < 20; i++) virtual_sleep(1000000); });
    other.join();
    virtual_tick(&t5);
    CHECK(t5 < t4 + 19200);                                // 20 sleeps of 1 ms on the other thread: less than 1 ms of virtual time
    shutdown();
    CHECK(!virtual_tick(&t3));                             // off after shutdown / when not enabled
}

NXTEST(rng_is_a_seeded_stream_in_deterministic_mode) {
    uint8_t a[24], b[24], c[24];
    Rig r("rng");
    begin(r, {{"deterministic", "1"}, {"seed", "42"}, {"trace", "none"}});
    rng_fill(a, 24);
    const uint64_t n1 = rng_next();
    shutdown();
    begin(r, {{"deterministic", "1"}, {"seed", "42"}, {"trace", "none"}});
    rng_fill(b, 24);
    CHECK(std::memcmp(a, b, 24) == 0);
    CHECK_EQ(n1, rng_next());
    shutdown();
    begin(r, {{"deterministic", "1"}, {"seed", "43"}, {"trace", "none"}});
    rng_fill(c, 24);
    CHECK(std::memcmp(a, c, 24) != 0);
    CHECK_EQ(wall_filetime(), wall_filetime() - 0 + 0);    // fixed epoch + virtual time: reads do not move wall time
    shutdown();
}

NXTEST(hash_regions_fold_memory_into_the_frame_hash) {
    static uint8_t buf[256];
    std::memset(buf, 7, sizeof buf);
    char spec[64];
    std::snprintf(spec, sizeof spec, "0x%llx+0x100", static_cast<unsigned long long>(reinterpret_cast<uint64_t>(buf)));
    uint64_t h[3];
    for (int i = 0; i < 3; i++) {
        Rig r("regions");
        begin(r, {{"deterministic", "1"}, {"hash_regions", spec}, {"trace", "none"}});
        if (i == 2) buf[100] ^= 1;
        ::Ctx ctx{};
        frame_boundary(&ctx);
        h[i] = last_hash();
        shutdown();
    }
    CHECK_EQ(h[0], h[1]);
    CHECK(h[0] != h[2]);
}

namespace {
bool shot_ok(const char* path, void*) { spit(path, "PNG?"); return true; }
}

NXTEST(shots_go_through_the_provider) {
    Rig r("shot");
    const std::string s = scriptfile("shot.nxs", "2\n@shot first\n1\n@shot second\n@pass\n");
    begin(r, {{"script", s}});
    set_screenshot_provider(shot_ok, nullptr);
    run_frames(r, 50);
    CHECK_EQ(r.exit_code_seen, 0);
    const std::string res = slurp(r.out + "\\result.json");
    CHECK(res.find("\"first.png\"") != std::string::npos && res.find("\"second.png\"") != std::string::npos);
    CHECK_EQ(slurp(r.out + "\\shots\\first.png"), std::string("PNG?"));
    shutdown();
    set_screenshot_provider(nullptr, nullptr);
}

NXTEST(missing_provider_is_a_note_unless_required) {
    for (int req = 0; req < 2; req++) {
        Rig r("noshot");
        const std::string s = scriptfile("noshot.nxs", "2\n@shot x\n@pass\n");
        begin(r, {{"script", s}, {"require_shots", req ? "1" : "0"}});
        run_frames(r, 50);
        CHECK_EQ(r.exit_code_seen, req ? 1 : 0);
        if (!req) CHECK(slurp(r.out + "\\result.json").find("\"missing_shots\":[\"x\"]") != std::string::npos);
        shutdown();
    }
}

NXTEST(max_frames_budget_fails_the_run) {
    Rig r("budget");
    const std::string s = scriptfile("budget.nxs", "100000\n");
    begin(r, {{"script", s}, {"max_frames", "30"}});
    run_frames(r, 500);
    CHECK_EQ(r.exit_code_seen, 1);
    CHECK(slurp(r.out + "\\result.json").find("max_frames") != std::string::npos);
    shutdown();
}

NXTEST(watchdog_freeze_ends_a_scripted_run_with_code_5) {
    Rig r("freeze");
    const std::string s = scriptfile("freeze.nxs", "100000\n");
    nxrt::config_clear();
    nxrt::config_set("diag", "watchdog_seconds", "1");
    reset_for_tests();
    set_exit_handler(on_exit_code);
    nxrt::config_set("test", "out", r.out.c_str());
    nxrt::config_set("test", "frame_source", "manual");
    nxrt::config_set("test", "script", s.c_str());
    nxrt::config_set("test", "freeze_exit", "1");
    r.exit_code_seen = -999;
    init();
    ::Ctx ctx{};
    frame_boundary(&ctx);                       // one frame, then the "game" hangs
    for (int i = 0; i < 60 && r.exit_code_seen == -999; i++) Sleep(100);
    CHECK_EQ(r.exit_code_seen, 5);
    CHECK(slurp(r.out + "\\result.json").find("\"verdict\":\"freeze\"") != std::string::npos);
    shutdown();
    nxrt::watchdog_stop();
}

// ---- scripted key steps (set_key_sink) ----------------------------------------------------------------------------------------
namespace {
struct KeyEvent {
    std::string name;
    bool down;
    uint64_t frame;      // the frame boundary (0-based) the sink was called at
};
std::vector<KeyEvent> g_key_events;

// the sink: refuses "Nope", records everything else with the frame it was called at
bool record_key(const char* name, bool down) {
    if (std::string(name) == "Nope") return false;
    g_key_events.push_back({name, down, frame() - 1});
    return true;
}
}  // namespace

NXTEST(key_steps_reach_the_key_sink_on_their_frames) {
    Rig r("keys");
    const std::string s = scriptfile("keys.nxs", "key W for 3\nkeydown Left Ctrl\nkeyup Left Ctrl\n@pass\n");
    begin(r, {{"script", s}, {"deterministic", "1"}});
    g_key_events.clear();
    set_key_sink(record_key);
    run_frames(r, 50);
    CHECK(finished());
    CHECK_EQ(r.exit_code_seen, 0);
    // "key W for 3" is down for frames 0, 1, 2 and released at frame 3, where the next two steps run in the same frame
    REQUIRE(g_key_events.size() == 4);
    CHECK(g_key_events[0].name == "W" && g_key_events[0].down && g_key_events[0].frame == 0);
    CHECK(g_key_events[1].name == "W" && !g_key_events[1].down && g_key_events[1].frame == 3);
    CHECK(g_key_events[2].name == "Left Ctrl" && g_key_events[2].down && g_key_events[2].frame == 3);
    CHECK(g_key_events[3].name == "Left Ctrl" && !g_key_events[3].down && g_key_events[3].frame == 3);
    set_key_sink(nullptr);
    shutdown();
}

NXTEST(key_without_for_is_four_frames) {
    Rig r("keys4");
    const std::string s = scriptfile("keys4.nxs", "key Mouse1\n@pass\n");
    begin(r, {{"script", s}});
    g_key_events.clear();
    set_key_sink(record_key);
    run_frames(r, 50);
    REQUIRE(g_key_events.size() == 2);
    CHECK(g_key_events[0].name == "Mouse1" && g_key_events[0].down && g_key_events[0].frame == 0);
    CHECK(g_key_events[1].name == "Mouse1" && !g_key_events[1].down && g_key_events[1].frame == 4);
    set_key_sink(nullptr);
    shutdown();
}

NXTEST(unknown_key_name_fails_the_run_and_releases_held_keys) {
    Rig r("keyunknown");
    // keydown W holds W; the next line names a key the sink refuses, so the run fails there and W must be released
    const std::string s = scriptfile("keys_unknown.nxs", "keydown W\nkey Nope\n@pass\n");
    begin(r, {{"script", s}});
    g_key_events.clear();
    set_key_sink(record_key);
    run_frames(r, 50);
    CHECK_EQ(r.exit_code_seen, 1);
    const std::string res = slurp(r.out + "/result.json");
    CHECK(res.find("\"verdict\":\"fail\"") != std::string::npos);
    CHECK(res.find("keys_unknown.nxs:2:") != std::string::npos);
    CHECK(res.find("unknown input 'Nope'") != std::string::npos);
    REQUIRE(g_key_events.size() == 2);
    CHECK(g_key_events[0].name == "W" && g_key_events[0].down);
    CHECK(g_key_events[1].name == "W" && !g_key_events[1].down);
    set_key_sink(nullptr);
    shutdown();
}

NXTEST(keys_still_down_are_released_when_the_script_passes) {
    Rig r("keyhold");
    const std::string s = scriptfile("keyhold.nxs", "keydown Left Ctrl\n2\n@pass\n");
    begin(r, {{"script", s}});
    g_key_events.clear();
    set_key_sink(record_key);
    run_frames(r, 50);
    CHECK_EQ(r.exit_code_seen, 0);
    REQUIRE(g_key_events.size() == 2);
    CHECK(g_key_events[0].name == "Left Ctrl" && g_key_events[0].down && g_key_events[0].frame == 0);
    CHECK(g_key_events[1].name == "Left Ctrl" && !g_key_events[1].down && g_key_events[1].frame == 2);
    set_key_sink(nullptr);
    shutdown();
}

// ---- the game side of the interface -----------------------------------------------------------------------------------------------

NXTEST(game_fields_are_in_the_hash_and_the_trace) {
    uint64_t h[3];
    std::string trace;
    for (int i = 0; i < 3; i++) {
        Rig r("fields");
        r.mode = i == 2 ? 5 : 1;
        begin(r, {{"deterministic", "1"}});
        ::Ctx ctx{};
        frame_boundary(&ctx);
        h[i] = last_hash();
        shutdown();
        if (i == 0) trace = slurp(r.out + "/trace.jsonl");
    }
    CHECK_EQ(h[0], h[1]);                    // the same state, the same hash
    CHECK(h[0] != h[2]);                     // a game field changed
    // printed in the order the game added them, before hp and igt; "fuel" is shown because the mode is set
    CHECK(trace.find("\"mode\":1,\"fuel\":0.5,\"lives\":3,\"hp\":99") != std::string::npos);
}

NXTEST(a_game_field_that_is_not_shown_is_left_out_of_the_trace) {
    Rig r("hidden");
    r.mode = 0;
    begin(r, {{"deterministic", "1"}});
    ::Ctx ctx{};
    frame_boundary(&ctx);
    shutdown();
    const std::string trace = slurp(r.out + "/trace.jsonl");
    CHECK(trace.find("\"mode\":0,\"lives\":3") != std::string::npos);    // fuel is not shown at mode 0
    CHECK(trace.find("fuel") == std::string::npos);
}

NXTEST(a_high_field_shares_the_word_of_the_field_before_it) {
    // mode (Word) and lives (High) are one 64-bit word: the hash must equal the hand-folded one
    Rig r("pack");
    begin(r, {{"deterministic", "1"}, {"trace", "none"}});
    ::Ctx ctx{};
    frame_boundary(&ctx);
    const uint64_t got = last_hash();
    shutdown();
    auto mix = [](uint64_t h, uint64_t w) { h = (h ^ w) * 0x100000001b3ull; return h ^ (h >> 29); };
    auto bits = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return static_cast<uint64_t>(u); };
    uint64_t h = 0xcbf29ce484222325ull;
    h = mix(h, 0);
    h = mix(h, 1u | 2u | 4u | 8u | 16u);
    for (int i = 0; i < 16; i += 8) { uint64_t w; std::memcpy(&w, r.world + i, 8); h = mix(h, w); }
    for (int i = 0; i < 16; i += 8) { uint64_t w; std::memcpy(&w, r.area + i, 8); h = mix(h, w); }
    for (int i = 0; i < 3; i++) { h = mix(h, bits(r.pos[i])); h = mix(h, 0); }
    h = mix(h, 0);                                       // yaw
    h = mix(h, bits(r.hp));
    uint64_t ig; std::memcpy(&ig, &r.igt, 8);
    h = mix(h, ig);
    h = mix(h, 0u | (3ull << 32));                       // mode | lives << 32
    CHECK_EQ(got, h);
}

NXTEST(game_steps_parse_and_run_in_the_frame) {
    Rig r("gamestep");
    const std::string s = scriptfile("gamestep.nxs", "2\n@bump 3\n@BUMP 4\n1\n@assert mode == 7\n@assert FUEL > 3\n@mashuntil A every 5 until mode >= 7 timeout=20\n@pass\n");
    begin(r, {{"script", s}});
    run_frames(r, 50);
    CHECK_EQ(r.exit_code_seen, 0);
    CHECK_EQ(r.bumps, 2);
    CHECK_EQ(r.mode, 7);
    shutdown();
}

NXTEST(a_game_step_that_waits_blocks_until_done) {
    Rig r("lift_ok");
    begin(r, {{"script", scriptfile("lift_ok.nxs", "@lift\n@assert pz >= 0.25\n@pass\n")}});
    run_frames(r, 5);
    CHECK(!finished());                          // the step keeps waiting
    r.pos[2] = 0.3f;
    run_frames(r, 5);
    CHECK_EQ(r.exit_code_seen, 0);
    shutdown();
}

NXTEST(a_game_step_fails_the_run_with_its_message_after_the_timeout) {
    Rig r("lift_fail");
    begin(r, {{"script", scriptfile("lift_fail.nxs", "@timeout 5\n2\n@lift\n@pass\n")}});
    run_frames(r, 30);
    CHECK_EQ(r.exit_code_seen, 1);
    const std::string res = slurp(r.out + "/result.json");
    CHECK(res.find("lift_fail.nxs:3:") != std::string::npos);
    CHECK(res.find("never lifted") != std::string::npos);
    shutdown();
}

NXTEST(game_step_errors_name_the_script_line) {
    Rig r("gameerr");
    begin(r, {{"script", scriptfile("gameerr.nxs", "2\n@bump\n")}});
    CHECK_EQ(r.exit_code_seen, 2);
    const std::string res = slurp(r.out + "/result.json");
    CHECK(res.find("gameerr.nxs:2:") != std::string::npos);
    CHECK(res.find("@bump <n>") != std::string::npos);
    shutdown();
}

NXTEST(a_game_step_is_unknown_to_a_game_without_it) {
    Rig r("nostep");
    r.info.probes = nullptr;                         // the game registers no steps and no state keys
    begin(r, {{"script", scriptfile("nostep.nxs", "@bump 1\n")}});
    CHECK_EQ(r.exit_code_seen, 2);
    CHECK(slurp(r.out + "/result.json").find("unknown keyword '@bump'") != std::string::npos);
    shutdown();
    Rig q("nokey");
    q.info.probes = nullptr;
    begin(q, {{"script", scriptfile("nokey.nxs", "@assert mode == 1\n")}});
    CHECK_EQ(q.exit_code_seen, 2);
    shutdown();
}
