// SPDX-License-Identifier: GPL-3.0-or-later
// Moved from the base's runtime/tests (test_tt_core.cpp) in the untangle: these cases check Metroid Prime Remastered's tables (title defaults,
// play-mode vocabulary and factory settings, test-tooling probes), so they run with the mpr module registered.
// Test tooling core (S5): a fake game whose memory has exactly the shape the registered test game's probes read (games/mpr/mpr_tt_probe.cpp), driven frame by frame
// through tt::frame_boundary. Covers scripted runs (pass, wait timeout, assert), record -> replay determinism, the virtual clock and RNG,
// memory-region hashing, the warp sequence (host functions registered at the guest addresses of the real functions) and @shot.
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
#include "nxrt/tt.h"

NXTEST_GROUP(mpr_tt_core)

namespace {

using namespace nxrt::tt;

constexpr uint64_t kBase = 0x5000000000ull;          // the fake "main module" base: only a key for fn_register / hook addresses
const char* kWorld = "4b5e8635-0f43-4ffb-9427-2e62186e517d";
const char* kAreaA = "4ca69978-0000-4000-8000-000000000001";
const char* kAreaB = "4ca69978-0000-4000-8000-000000000002";

// A fake game whose memory has the shape the probes read: gpGameState -> CGameStateMP1 -> world array; CStateManager -> CGameState (igt),
// GameLogic -> player -> transform. The toy simulation: UP walks +y, left stick strafes +x, A raises z, igt counts.
struct Rig {
    alignas(16) uint8_t gs[0x40] = {};
    alignas(16) uint8_t worlds[2 * 0x50] = {};
    alignas(16) uint8_t pstate[0x20] = {};
    uint64_t gp = 0;
    uint8_t* mgr;                // CStateManager, large enough for +0x4e0150
    uint8_t* cgs;                // CGameState, 1 MiB
    alignas(16) uint8_t cgsw[0x10] = {};
    alignas(16) uint8_t logic[0x40] = {};
    alignas(16) uint8_t player[0x400] = {};
    alignas(16) uint8_t pa[0x40] = {};
    alignas(16) uint8_t pb[0x80] = {};
    float xf[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    double igt = 0;
    int warp_calls = 0;
    uint64_t warp_world[2] = {}, warp_area[2] = {}, quit_reason = 0;
    // S42 warp gate: the game globals -> CResourceFactory -> CResourceLoaderManager chain, with the queue counts IsThreadWorkDone reads
    bool with_loader = false;
    uint64_t globals = 0;
    alignas(16) uint8_t factory[0x20] = {};
    alignas(16) uint8_t loader[0x200] = {};
    uint8_t pending_area[16] = {};
    bool pending = false;
    int pending_in = 0;
    std::string out;
    int exit_code_seen = -999;
    static Rig* cur;

    explicit Rig(const char* name) {
        cur = this;
        out = std::string("tt_test_out\\") + name;
        mgr = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x4f0000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        cgs = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x101000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        tt_guid_to_bytes(kWorld, gs);
        std::memcpy(worlds, gs, 16);
        tt_guid_to_bytes(kAreaA, worlds + 0x10);
        const int32_t count = 1;
        std::memcpy(gs + 0x10, &count, 4);
        put(gs + 0x18, worlds);
        put(gs + 0x20, pstate);
        const float hp = 99.0f;
        std::memcpy(pstate + 0xc, &hp, 4);
        gp = reinterpret_cast<uint64_t>(gs);
        put(mgr + 8, cgsw);
        put(cgsw + 8, cgs);
        put(mgr + 0x4e0150, logic);
        put(logic + 0x18, player);
        put(player + 8, pa);
        put(pa + 0x20, pb);
        put(pb + 0x40, xf);
        put(factory + 0x10, loader);
        globals = reinterpret_cast<uint64_t>(factory);
        set_igt(0);
    }
    void set_loader_queue(int32_t n) { std::memcpy(loader + 0xe8, &n, 4); }      // the load list count
    ~Rig() {
        VirtualFree(mgr, 0, MEM_RELEASE);
        VirtualFree(cgs, 0, MEM_RELEASE);
        cur = nullptr;
    }
    static void put(uint8_t* where, const void* ptr) { const uint64_t v = reinterpret_cast<uint64_t>(ptr); std::memcpy(where, &v, 8); }
    void set_igt(double v) { igt = v; std::memcpy(cgs + 0xffb08, &v, 8); }
    void layout() {
        ProbeLayout l;
        l.game_state_global = reinterpret_cast<uint64_t>(&gp);
        l.state_manager = reinterpret_cast<uint64_t>(mgr);
        if (with_loader) l.game_globals = reinterpret_cast<uint64_t>(&globals);
        set_probe_layout(l);
    }
    // one frame of the toy game, using the pad of the frame that just began
    void step(const PadFrame& p) {
        if (p.buttons & (1ull << 13)) xf[7] += 0.5f;
        xf[3] += static_cast<float>(p.lx) / 32767.0f * 0.25f;
        if (p.buttons & 1ull) xf[11] += 0.125f;
        set_igt(igt + 1.0 / 60.0);
        if (pending && --pending_in <= 0) {              // a warp "loads" for 3 frames
            std::memcpy(worlds + 0x10, pending_area, 16);
            pending = false;
        }
    }
};
Rig* Rig::cur = nullptr;

void fake_set_world(::Ctx* c) { Rig* r = Rig::cur; r->warp_calls++; r->warp_world[0] = c->x[1]; r->warp_world[1] = c->x[2]; std::memcpy(r->gs, &c->x[1], 8); std::memcpy(r->gs + 8, &c->x[2], 8); }
void fake_current_ws(::Ctx* c) { c->x[0] = reinterpret_cast<uint64_t>(Rig::cur->worlds); }
void fake_set_desired(::Ctx* c) { Rig* r = Rig::cur; r->warp_area[0] = c->x[1]; r->warp_area[1] = c->x[2]; }
void fake_continue(::Ctx* c) {
    Rig* r = Rig::cur;
    std::memcpy(r->pending_area, reinterpret_cast<void*>(c->x[1]), 16);
    int32_t slot;
    std::memcpy(&slot, reinterpret_cast<uint8_t*>(c->x[1]) + 16, 4);
    if (slot != -1 || c->x[0] != reinterpret_cast<uint64_t>(r->cgs)) r->warp_calls += 1000;     // wrong arguments are visible in the count
}
void fake_gis(::Ctx* c) { c->x[0] = 0x1234000; }
void fake_quit(::Ctx* c) { Rig* r = Rig::cur; r->quit_reason = c->x[1]; r->pending = true; r->pending_in = 3; }
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
    nxrt::config_set("test", "game_base", std::to_string(kBase).c_str());
    for (auto& p : kv) nxrt::config_set("test", p.first, p.second.c_str());
    r.layout();
    r.exit_code_seen = -999;
    r.pending = false;
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

// the 'h' column of every frame line of a trace file
std::vector<std::string> hashes_of(const std::string& trace) {
    std::vector<std::string> v;
    std::istringstream is(trace);
    std::string line;
    while (std::getline(is, line)) {
        const size_t f = line.find("\"f\":"), h = line.find("\"h\":\"");
        if (f == std::string::npos || h == std::string::npos) continue;
        v.push_back(line.substr(f, line.find(',', f) - f) + line.substr(h, 22));
    }
    return v;
}

const char* kSession =
    "# walk and hash\n"
    "5\n"
    "hold UP\n"
    "20\n"
    "release UP\n"
    "stick left 1.0 0 for 10\n"
    "press A for 3\n"
    "mouse 4 -2 for 2\n"
    "@hash walked\n"
    "@assert health >= 90\n"
    "@pos 2.5 10.0 0.375 0.01\n"
    "@pass\n";

}  // namespace

NXTEST(probes_read_the_fake_game) {
    Rig r("probes");
    r.set_igt(12.5);
    r.xf[3] = 1.5f; r.xf[7] = -2.0f; r.xf[11] = 3.25f;
    begin(r, {{"trace", "none"}, {"deterministic", "1"}});
    GameState g;
    REQUIRE(read_state(&g));
    CHECK(g.have_world && g.have_player && g.have_igt && g.have_hp);
    CHECK_EQ(tt_guid_to_text(g.world), std::string(kWorld));
    CHECK_EQ(tt_guid_to_text(g.area), std::string(kAreaA));
    CHECK(g.pos[0] == 1.5f && g.pos[1] == -2.0f && g.pos[2] == 3.25f);
    CHECK(g.hp == 99.0f);
    CHECK(g.igt == 12.5);
    shutdown();
}

NXTEST(script_runs_and_passes) {
    Rig r("pass");
    const std::string s = scriptfile("pass.nxs", kSession);
    begin(r, {{"script", s}, {"deterministic", "1"}});
    run_frames(r, 200);
    CHECK(finished());
    CHECK_EQ(r.exit_code_seen, 0);
    const std::string res = slurp(r.out + "\\result.json");
    CHECK(res.find("\"verdict\":\"pass\"") != std::string::npos);
    CHECK(res.find("\"label\":\"walked\"") != std::string::npos);
    // UP held for 20 frames (+ the frame the hold takes effect): y = 0.5 per frame
    CHECK(std::fabs(r.xf[7] - 10.0f) < 0.01f);
    CHECK(std::fabs(r.xf[11] - 0.375f) < 1e-6f);
    CHECK(!hashes_of(slurp(r.out + "\\trace.jsonl")).empty());
    shutdown();
}

NXTEST(replay_of_a_recording_is_bit_identical) {
    Rig a("rec");
    const std::string s = scriptfile("session.nxs", kSession);
    const std::string rec = "tt_test_out\\session.nxin";
    begin(a, {{"script", s}, {"deterministic", "1"}, {"record", rec}});
    run_frames(a, 200);
    REQUIRE(a.exit_code_seen == 0);
    const uint64_t chain_script = chain_hash();
    const auto trace_script = hashes_of(slurp(a.out + "\\trace.jsonl"));
    shutdown();
    CHECK(slurp(rec).find("# nxin 1") == 0);

    uint64_t chain_replay[2] = {0, 0};
    std::vector<std::string> traces[2];
    for (int i = 0; i < 2; i++) {
        Rig b(i ? "replay1" : "replay0");
        begin(b, {{"replay", rec}, {"deterministic", "1"}, {"replay_exit", "1"}});
        run_frames(b, 300);
        CHECK_EQ(b.exit_code_seen, 0);
        chain_replay[i] = chain_hash();
        traces[i] = hashes_of(slurp(b.out + "\\trace.jsonl"));
        CHECK(std::fabs(b.xf[7] - 10.0f) < 0.01f);       // the replay moved the player the same way
        shutdown();
    }
    CHECK_EQ(chain_replay[0], chain_replay[1]);
    CHECK(traces[0] == traces[1]);
    // the replay runs past the script's last frame by a frame or two (the trace's end): compare the common prefix
    REQUIRE(traces[0].size() >= trace_script.size() - 1);
    for (size_t i = 0; i + 2 < trace_script.size(); i++) CHECK_EQ(traces[0][i], trace_script[i]);
}

NXTEST(two_script_runs_have_the_same_final_hash) {
    uint64_t h[2];
    for (int i = 0; i < 2; i++) {
        Rig r("twice");
        begin(r, {{"script", scriptfile("twice.nxs", kSession)}, {"deterministic", "1"}});
        run_frames(r, 200);
        CHECK_EQ(r.exit_code_seen, 0);
        h[i] = chain_hash();
        CHECK(slurp(r.out + "\\result.json").find("\"final_hash\":\"") != std::string::npos);
        shutdown();
    }
    CHECK_EQ(h[0], h[1]);
}

NXTEST(warp_runs_the_practice_mod_sequence) {
    Rig r("warp");
    nxrt::fn_register(kBase + 0xc838e4, fake_set_world);
    nxrt::fn_register(kBase + 0xc84478, fake_current_ws);
    nxrt::fn_register(kBase + 0xc8275c, fake_set_desired);
    nxrt::fn_register(kBase + 0x40a820, fake_continue);
    nxrt::fn_register(kBase + 0xb01cc8, fake_gis);
    nxrt::fn_register(kBase + 0x410544, fake_quit);
    spit("tt_test_out\\regions.tsv", std::string("# test table\nfrig\t") + kWorld + "\t\nfrig_b\t" + kWorld + "\t" + kAreaB + "\n");
    const std::string s = scriptfile("warp.nxs", "2\n@warp frig frig_b\n@assert area == frig_b\n@assert world == frig\n@pass\n");
    begin(r, {{"script", s}, {"regions", "tt_test_out\\regions.tsv"}});
    run_frames(r, 100);
    CHECK_EQ(r.exit_code_seen, 0);
    CHECK_EQ(r.warp_calls, 1);                              // SetCurrentWorldId once, ContinueWithNewInitialState got the right arguments
    uint8_t w[16], a[16];
    tt_guid_to_bytes(kWorld, w);
    tt_guid_to_bytes(kAreaB, a);
    CHECK(std::memcmp(r.warp_world, w, 16) == 0);
    CHECK(std::memcmp(r.warp_area, a, 16) == 0);
    CHECK_EQ(r.quit_reason, 7u);
    shutdown();
}

// S42: @warp waits while the game's resource loader has work queued, and warps once it has been idle for 10 frames in a row.
NXTEST(warp_waits_for_the_idle_resource_loader) {
    Rig r("warpgate");
    r.with_loader = true;
    r.set_loader_queue(3);
    register_warp_fakes();
    const std::string s = scriptfile("warpgate.nxs", kWarpScript);
    begin(r, {{"script", s}, {"regions", "tt_test_out\\regions.tsv"}});
    run_frames(r, 60);
    CHECK_EQ(r.warp_calls, 0);                              // still busy: no warp
    CHECK(!finished());
    r.set_loader_queue(0);
    run_frames(r, 9);
    CHECK_EQ(r.warp_calls, 0);                              // idle, but not yet for 10 frames in a row
    r.set_loader_queue(1);
    run_frames(r, 5);                                       // busy again: the count starts over
    r.set_loader_queue(0);
    run_frames(r, 9);
    CHECK_EQ(r.warp_calls, 0);
    run_frames(r, 100);
    CHECK_EQ(r.warp_calls, 1);
    CHECK_EQ(r.exit_code_seen, 0);
    shutdown();
}

NXTEST(warp_gate_can_be_turned_off_and_gives_up_after_its_limit) {
    for (int mode = 0; mode < 2; mode++) {
        Rig r(mode ? "warpgate_max" : "warpgate_off");
        r.with_loader = true;
        r.set_loader_queue(5);                              // busy for the whole run
        register_warp_fakes();
        const std::string s = scriptfile("warpgate2.nxs", kWarpScript);
        if (mode == 0) begin(r, {{"script", s}, {"regions", "tt_test_out\\regions.tsv"}, {"warp_wait_loader", "0"}});
        else begin(r, {{"script", s}, {"regions", "tt_test_out\\regions.tsv"}, {"warp_loader_max_ms", "0"}});
        run_frames(r, 100);
        CHECK_EQ(r.warp_calls, 1);
        CHECK_EQ(r.exit_code_seen, 0);
        shutdown();
    }
}

