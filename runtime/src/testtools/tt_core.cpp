// SPDX-License-Identifier: GPL-3.0-or-later
// Test tooling core (S5): configuration, the frame boundary, virtual clock, seeded RNG, per-frame hash and trace, result file,
// input recording / replay, crash and freeze plumbing. See include/nxrt/tt.h.
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <regex>
#include <thread>

#include "nxrt/config.h"
#include "nxrt/memory.h"
#include "nxrt/diag.h"
#include "nxrt/game.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/runtime.h"
#include "nxrt/module.h"
#include "nxrt/timestep.h"
#include "nxrt/tt.h"
#include "tt_internal.h"

namespace nxrt::tt {
namespace detail {
}  // namespace detail

// The kernel never names tt: deterministic mode hands it a clock and a random stream through its provider hooks (nxrt/kernel.h).
namespace {
bool provide_rng(void* p, size_t n);
bool provide_wall(uint64_t* ft);
void install_kernel_providers();
void clear_kernel_providers();
}  // namespace

namespace {

enum class Source { Hook, Tick, Svc, Manual };

struct Region { uint64_t addr = 0, size = 0; std::string module; };

struct State {
    std::recursive_mutex mu;
    bool inited = false, on = false, det = false, script_mode = false, finished = false;
    int exit_code = -1;
    std::string out, script, regions_file, trace_path, record_path, replay_path;
    uint64_t seed = 0x5EED5EEDull;
    uint64_t step_ticks = 320000;            // 1/60 s at 19.2 MHz
    Source src = Source::Hook;
    uint32_t svc_n = 0;
    std::vector<Region> hash_regions;
    uint64_t max_frames = 0;
    double wall_timeout_s = 0;
    bool freeze_exit = false, require_shots = false, replay_exit = false;
    std::string shot_dir;
    int shot_timeout_ms = 30000;
    std::string script_text;                 // a script that has no file (set_script_text)

    std::atomic<uint64_t> frames{0};
    bool started = false;
    std::atomic<uint64_t> vticks{0};         // virtual clock (19.2 MHz ticks)
    std::atomic<DWORD> frame_tid{0};         // the host thread that runs the frame boundary (the one whose sleeps move the clock)
    std::atomic<uint64_t> rng_ctr{0};
    uint64_t vbase = 19200000ull * 100;
    uint64_t chain = 0xcbf29ce484222325ull;
    uint64_t last_hash = 0;
    GameState gs;
    bool gs_valid = false;
    GameState prev;
    bool have_prev = false;

    FILE* trace = nullptr;
    bool trace_header = false;
    InputTrace rec;
    InputTrace play;
    bool replaying = false, recording = false;
    PadFrame pad;
    PadFrame live_pad;
    std::vector<Checkpoint> cps;
    std::vector<std::string> shots, missing_shots;
    detail::Runner runner;
    bool runner_on = false;
    RegionTable table;
    bool have_table = false;
    std::string message;
    std::string verdict;

    ScreenshotFn shot_fn = nullptr;
    ScreenshotPollFn shot_poll_fn = nullptr;
    void* shot_user = nullptr;
    std::string shot_name, shot_path;        // the @shot that waits for its file
    uint64_t shot_start_ms = 0;
    ExitFn exit_fn = nullptr;
    MouseSinkFn mouse_sink = nullptr;
    KeySinkFn key_sink = nullptr;
    std::vector<std::string> keys_down;      // names the key steps pressed and not yet released (released when the run ends)
    LARGE_INTEGER qpf{}, t_last{}, t_start{};
    double last_ms = 0;
    uint64_t last_mem_frame = 0;
    double mem_mb = 0;
    std::atomic<bool> svc_thread_known{false};
    std::atomic<::Ctx*> svc_ctx{nullptr};
    std::atomic<uint64_t> svc_count{0};
    std::vector<std::thread> timers;         // the wall deadlines and the timed shots
    std::atomic<bool> stop_wall{false};
};

State& S() {
    static State* s = new State;
    return *s;
}

// Releases the keys the key steps left down (the run ended or the tooling shut down). Called with s.mu held.
void release_held_keys(State& s) {
    std::vector<std::string> keys;
    keys.swap(s.keys_down);
    if (!s.key_sink) return;
    for (const std::string& k : keys) s.key_sink(k.c_str(), false);
}

uint64_t splitmix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

inline uint64_t mix64(uint64_t h, uint64_t w) {
    h = (h ^ w) * 0x100000001b3ull;
    return h ^ (h >> 29);
}
uint64_t mix_bytes(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; std::memcpy(&w, b + i, 8); h = mix64(h, w); }
    uint64_t tail = 0;
    if (i < n) { std::memcpy(&tail, b + i, n - i); h = mix64(h, tail ^ (static_cast<uint64_t>(n - i) << 56)); }
    return h;
}
uint32_t fbits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
uint64_t dbits(double f) { uint64_t u; std::memcpy(&u, &f, 8); return u; }

std::string json_escape(const std::string& s) {
    std::string o;
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
    return o;
}

void make_dirs(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        const char ch = path[i];
        cur += ch;
        if ((ch == '\\' || ch == '/') && i > 2) CreateDirectoryA(cur.c_str(), nullptr);
    }
    CreateDirectoryA(path.c_str(), nullptr);
}

std::string parent_dir(const std::string& p) {
    const size_t i = p.find_last_of("\\/");
    return i == std::string::npos ? std::string() : p.substr(0, i);
}

std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    const char l = a.back();
    return (l == '\\' || l == '/') ? a + b : a + "\\" + b;
}



std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> v;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(sep, i);
        if (j == std::string::npos) j = s.size();
        if (j > i) v.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return v;
}

void parse_regions(State& s, const std::string& spec) {
    s.hash_regions.clear();
    for (std::string item : split(spec, ',')) {
        while (!item.empty() && item.front() == ' ') item.erase(item.begin());
        while (!item.empty() && item.back() == ' ') item.pop_back();
        if (item.empty()) continue;
        Region r;
        if (item.rfind("module:", 0) == 0) { r.module = item.substr(7); s.hash_regions.push_back(r); continue; }
        const size_t plus = item.find('+');
        if (plus == std::string::npos) { NXLOG_W("tt", "hash_regions: ignoring '%s' (want 0xADDR+0xSIZE or module:<name>)", item.c_str()); continue; }
        r.addr = std::strtoull(item.substr(0, plus).c_str(), nullptr, 0);
        r.size = std::strtoull(item.substr(plus + 1).c_str(), nullptr, 0);
        if (r.addr && r.size) s.hash_regions.push_back(r);
    }
}

uint64_t hash_region_memory(State& s, uint64_t h) {
    for (const Region& r : s.hash_regions) {
        std::vector<std::pair<uint64_t, uint64_t>> spans;
        uint64_t lo = r.addr, hi = r.addr + r.size;
        bool rw_only = false;
        if (!r.module.empty()) {
            const Module* m = nullptr;
            for (Module* mm : modules()) if (mm->name() == r.module) m = mm;
            if (!m) { h = mix64(h, 0xdeadull); continue; }
            lo = m->base(); hi = m->base() + m->size(); rw_only = true;
        }
        uint64_t a = lo;
        while (a < hi) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) break;
            const uint64_t end = std::min<uint64_t>(hi, reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize);
            const DWORD p = mbi.Protect & 0xff;
            const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && p != PAGE_NOACCESS;
            const bool rw = p == PAGE_READWRITE || p == PAGE_WRITECOPY;
            if (readable && (!rw_only || rw)) spans.emplace_back(a, end - a);
            if (end <= a) break;
            a = end;
        }
        for (auto& sp : spans) {
            h = mix64(h, sp.first);
            h = mix_bytes(h, reinterpret_cast<const void*>(sp.first), static_cast<size_t>(sp.second));
        }
    }
    return h;
}

uint64_t hash_state(State& s, uint64_t frame, const GameState& g, bool valid) {
    uint64_t h = 0xcbf29ce484222325ull;
    h = mix64(h, frame);
    h = mix64(h, (valid ? 1u : 0u) | (static_cast<uint64_t>(g.hash_flags) << 1));
    // The game's fields: one word each (an integer as unsigned 32 bits, a float as its bits); a High field shares the word of the Word field before it.
    using H = StateField::Hash;
    bool pending = false;
    uint64_t word = 0;
    auto flush = [&] { if (pending) h = mix64(h, word); pending = false; };
    for (const StateField& f : g.fields) {
        if (f.hash == H::None) continue;
        if (f.hash == H::Wide) { flush(); h = mix64(h, dbits(f.value)); continue; }
        if (f.hash == H::Bytes) { flush(); h = mix_bytes(h, f.id, 16); continue; }
        const uint64_t v = f.kind == StateField::Kind::Float ? fbits(static_cast<float>(f.value)) : static_cast<uint32_t>(static_cast<int64_t>(f.value));
        if (f.hash == H::High && pending) { word |= v << 32; continue; }
        flush();
        word = v;
        pending = true;
    }
    flush();
    h = hash_region_memory(s, h);
    return h;
}

double now_ms(State& s) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart - s.t_start.QuadPart) * 1000.0 / static_cast<double>(s.qpf.QuadPart);
}

void write_trace_line(State& s, uint64_t k, uint64_t h, const GameState& g, bool valid, double ms, double mem) {
    if (!s.trace) return;
    char b[256];
    std::string line;
    auto add = [&](const char* fmt, auto... a) {
        const int n = std::snprintf(b, sizeof b, fmt, a...);
        if (n > 0) line.append(b, static_cast<size_t>(std::min<int>(n, sizeof b - 1)));
    };
    add("{\"f\":%llu,\"h\":\"%016llx\"", static_cast<unsigned long long>(k), static_cast<unsigned long long>(h));
    if (valid) {
        for (const StateField& f : g.fields) {
            if (!f.shown || f.quiet || !f.key[0]) continue;
            switch (f.kind) {
                case StateField::Kind::Float: add(",\"%s\":%.9g", f.key, f.value); break;
                case StateField::Kind::Int: add(",\"%s\":%lld", f.key, static_cast<long long>(f.value)); break;
                case StateField::Kind::Id: add(",\"%s\":\"%s\"", f.key, tt_guid_hex(f.id).c_str()); break;
                case StateField::Kind::Vec3: add(",\"%s\":[%.9g,%.9g,%.9g]", f.key, f.vec[0], f.vec[1], f.vec[2]); break;
            }
        }
    }
    add(",\"t\":%llu,\"ms\":%.3f", static_cast<unsigned long long>(s.vticks.load()), ms);
    if (mem > 0) add(",\"mem\":%.1f", mem);
    line += "}\n";
    std::fwrite(line.data(), 1, line.size(), s.trace);
    if (k % 64 == 0) std::fflush(s.trace);
}

void write_result(State& s, int code, const char* verdict, const std::string& msg) {
    const std::string path = join(s.out, "result.json");
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const double wall = static_cast<double>(c.QuadPart - s.t_start.QuadPart) / static_cast<double>(s.qpf.QuadPart);
    std::fprintf(f, "{\n \"schema\":1,\n \"verdict\":\"%s\",\n \"exit_code\":%d,\n \"message\":\"%s\",\n \"script\":\"%s\",\n \"frames\":%llu,\n \"wall_s\":%.3f,\n",
                 verdict, code, json_escape(msg).c_str(), json_escape(s.script).c_str(), static_cast<unsigned long long>(s.frames.load()), wall);
    std::fprintf(f, " \"deterministic\":%s,\n \"seed\":%llu,\n \"final_hash\":\"%016llx\",\n \"steps_total\":%zu,\n \"steps_done\":%zu,\n \"fail_line\":%d,\n",
                 s.det ? "true" : "false", static_cast<unsigned long long>(s.seed), static_cast<unsigned long long>(s.chain), s.runner.steps_total(), s.runner.steps_done(), s.runner.fail_line());
    std::fprintf(f, " \"checkpoints\":[");
    for (size_t i = 0; i < s.cps.size(); i++)
        std::fprintf(f, "%s{\"label\":\"%s\",\"frame\":%llu,\"hash\":\"%016llx\"}", i ? "," : "", json_escape(s.cps[i].label).c_str(),
                     static_cast<unsigned long long>(s.cps[i].frame), static_cast<unsigned long long>(s.cps[i].hash));
    std::fprintf(f, "],\n \"shots\":[");
    for (size_t i = 0; i < s.shots.size(); i++) std::fprintf(f, "%s\"%s\"", i ? "," : "", json_escape(s.shots[i]).c_str());
    std::fprintf(f, "],\n \"missing_shots\":[");
    for (size_t i = 0; i < s.missing_shots.size(); i++) std::fprintf(f, "%s\"%s\"", i ? "," : "", json_escape(s.missing_shots[i]).c_str());
    std::fprintf(f, "]\n}\n");
    std::fclose(f);
}

void flush_files(State& s) {
    if (s.trace) std::fflush(s.trace);
    if (s.recording && !s.record_path.empty()) {
        std::string err;
        s.rec.end_frame = s.frames.load();
        if (!s.rec.write(s.record_path, &err)) NXLOG_E("tt", "cannot write the input recording %s: %s", s.record_path.c_str(), err.c_str());
    }
}

void default_exit(int code) {
    log_flush();
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
}

void on_crash_report(const char* text, void*) {
    State& s = S();
    if (!s.on || s.finished) return;
    s.finished = true;
    s.exit_code = 4;
    s.verdict = "crash";
    std::string first(text ? text : "");
    const size_t nl = first.find('\n');
    if (nl != std::string::npos) first.resize(nl);
    write_result(s, 4, "crash", first);
    if (s.trace) std::fflush(s.trace);
    // the crash handler terminates the process with exit code 4 itself
}

void on_freeze(const char* dump, void*) {
    State& s = S();
    if (!s.on) return;
    NXLOG_E("tt", "freeze detected by the watchdog at frame %llu", static_cast<unsigned long long>(s.frames.load()));
    if (s.freeze_exit) end_run(5, "freeze", "the watchdog saw no frame tick (thread dump in the freeze report)");
}

void tick_listener() {
    State& s = S();
    if (s.src == Source::Tick && s.on && !s.finished) frame_boundary(nullptr);
}

// Sleeps `secs` in slices; false when the tooling was shut down meanwhile.
bool timer_sleep(State* s, double secs) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int64_t>(secs * 1000));
    while (std::chrono::steady_clock::now() < until) {
        if (s->stop_wall.load()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return !s->stop_wall.load();
}

void deadline_loop(State* s, double secs, void (*report)()) {
    log_set_thread_name("tt-wall");
    if (!timer_sleep(s, secs) || s->finished) return;
    const std::string what = "wall-clock timeout of " + std::to_string(static_cast<int64_t>(secs)) + " s reached after " + std::to_string(s->frames.load()) + " frames";
    NXLOG_W("tt", "max_seconds reached: the guest is still running; ending the process (%s)", what.c_str());
    log_flush();
    if (report) report();
    if (s->on) {
        end_run(124, "timeout", what);
        return;
    }
    NXLOG_I("tt", "state at the deadline:\n%s", nxrt::summary().c_str());
    log_flush();
    TerminateProcess(GetCurrentProcess(), 124);
}

void shots_loop(State* s, double first, double every, std::string dir) {
    log_set_thread_name("tt-shots");
    if (!timer_sleep(s, first)) return;
    for (int n = 0;; n++) {
        char name[64];
        std::snprintf(name, sizeof name, "shot_%d.png", n);
        const std::string path = dir.empty() ? std::string(name) : join(dir, name);
        if (take_screenshot(path)) NXLOG_I("tt", "screenshot: %s", name);
        if (every <= 0 || !timer_sleep(s, every)) return;
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------
bool init() {
    State& s = S();
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    if (s.inited) return s.on;
    s.inited = true;
    s.frame_tid.store(0, std::memory_order_relaxed);   // set again by the first frame boundary of this run
    s.script = config_get_str("test", "script", "");
    if (const std::string press = config_get_str("boot", "press", ""); s.script.empty() && !press.empty())      // the start key of players and tools, see press_list_to_script
        s.script_text = "@timeout 0\n" + press_list_to_script(press);
    s.record_path = config_get_str("test", "record", "");
    s.replay_path = config_get_str("test", "replay", "");
    s.det = config_get_bool("test", "deterministic", false);
    const std::string trace = config_get_str("test", "trace", s.script.empty() && !s.script_text.empty() ? "none" : "");
    const std::string src = config_get_str("test", "frame_source", s.script.empty() && !s.script_text.empty() ? "tick" : "hook");
    s.on = !s.script.empty() || !s.script_text.empty() || !s.record_path.empty() || !s.replay_path.empty() || s.det || !trace.empty() || config_get_bool("test", "enabled", false);
    if (!s.on) return false;
    s.script_mode = !s.script.empty();
    s.shot_timeout_ms = static_cast<int>(config_get_int("test", "shot_timeout_ms", 30000));
    s.out = config_get_str("test", "out", "testout");
    s.seed = static_cast<uint64_t>(config_get_int("test", "seed", 0x5EED5EED));
    s.step_ticks = static_cast<uint64_t>(config_get_int("test", "clock_step_ticks", 320000));
    s.max_frames = static_cast<uint64_t>(config_get_int("test", "max_frames", 0));
    s.wall_timeout_s = config_get_double("test", "wall_timeout_s", s.script_mode ? 900.0 : 0.0);
    s.freeze_exit = config_get_bool("test", "freeze_exit", s.script_mode);
    s.require_shots = config_get_bool("test", "require_shots", false);
    s.replay_exit = config_get_bool("test", "replay_exit", false);
    s.shot_dir = config_get_str("test", "shot_dir", "");
    if (s.shot_dir.empty()) s.shot_dir = join(s.out, "shots");
    parse_regions(s, config_get_str("test", "hash_regions", ""));
    if (src == "tick") s.src = Source::Tick;
    else if (src == "manual") s.src = Source::Manual;
    else if (src.rfind("svc:", 0) == 0) { s.src = Source::Svc; s.svc_n = static_cast<uint32_t>(std::strtoul(src.c_str() + 4, nullptr, 0)); if (!s.svc_n) s.svc_n = 1; }
    else s.src = Source::Hook;
    QueryPerformanceFrequency(&s.qpf);
    QueryPerformanceCounter(&s.t_start);
    s.t_last = s.t_start;
    s.vticks = s.vbase;
    s.rng_ctr = 0;

    make_dirs(s.out);
    make_dirs(s.shot_dir);
    const std::string tp = trace.empty() ? join(s.out, "trace.jsonl") : trace;
    s.trace_path = tp;
    if (tp != "none") {
        s.trace = std::fopen(tp.c_str(), "wb");
        if (!s.trace) NXLOG_E("tt", "cannot open the trace file %s", tp.c_str());
        else std::fprintf(s.trace, "{\"trace\":1,\"source\":\"nxrt\",\"seed\":%llu,\"deterministic\":%s}\n", static_cast<unsigned long long>(s.seed), s.det ? "true" : "false");
    }
    if (!s.replay_path.empty()) {
        std::string err;
        if (!s.play.read(s.replay_path, &err)) { NXLOG_E("tt", "cannot read the input trace %s: %s", s.replay_path.c_str(), err.c_str()); end_run(2, "error", "cannot read replay file: " + err); return true; }
        s.replaying = true;
    }
    s.recording = !s.record_path.empty();
    if (s.script_mode) {
        std::string rf = config_get_str("test", "regions", "");
        if (rf.empty()) {
            const std::string cand = join(parent_dir(s.script), "regions.tsv");
            if (GetFileAttributesA(cand.c_str()) != INVALID_FILE_ATTRIBUTES) rf = cand;
        }
        if (!rf.empty()) {
            std::string err;
            if (!s.table.load(rf, &err)) { end_run(2, "error", "regions file: " + err); return true; }
            s.have_table = true;
        }
        std::string err;
        if (!s.runner.load(s.script, s.have_table ? &s.table : nullptr, &err)) {
            NXLOG_E("tt", "script error: %s", err.c_str());
            end_run(2, "error", err);
            return true;
        }
        s.runner_on = true;
    } else if (!s.script_text.empty()) {
        std::string err;
        if (!s.runner.load_text(s.script_text, "[boot] press", nullptr, &err)) {
            NXLOG_E("tt", "script error: %s", err.c_str());
            end_run(2, "error", err);
            return true;
        }
        s.runner_on = true;
    }
    NXLOG_I("tt", "test tooling on: script='%s' replay='%s' record='%s' deterministic=%d seed=0x%llx source=%s out='%s' regions=%zu", s.script.c_str(),
            s.replay_path.c_str(), s.record_path.c_str(), s.det, static_cast<unsigned long long>(s.seed), src.c_str(), s.out.c_str(), s.hash_regions.size());

    // plumbing: crash report -> result.json (exit code 4 by the crash handler), watchdog -> exit code 5, wall-clock deadline -> 124
    if (s.script_mode) {
        CrashOptions co;
        co.on_report = on_crash_report;
        co.exit_code = 4;
        crash_install(co);
        if (s.freeze_exit) {
            watchdog_stop();
            WatchdogOptions wo;
            wo.on_fire = on_freeze;
            watchdog_start(wo);
        }
    }
    diag_set_frame_listener(tick_listener);
    if (s.det) install_kernel_providers();
    if (s.src == Source::Svc) kernel::set_svc_observer(detail::on_svc);
    if (s.src == Source::Hook) install_game_hooks();
    if (s.wall_timeout_s > 0) start_wall_deadline(s.wall_timeout_s);
    return true;
}

void shutdown() {
    State& s = S();
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    if (!s.on) return;
    s.stop_wall = true;
    for (std::thread& t : s.timers) if (t.joinable()) t.join();
    s.timers.clear();
    clear_kernel_providers();
    release_held_keys(s);
    if (!s.finished) {
        s.finished = true;
        s.exit_code = 0;
        s.verdict = s.script_mode ? "error" : "pass";
        write_result(s, s.script_mode ? 6 : 0, s.verdict.c_str(), s.script_mode ? "the process ended before the script finished" : "");
        s.exit_code = s.script_mode ? 6 : 0;
    }
    flush_files(s);
    if (s.trace) { std::fclose(s.trace); s.trace = nullptr; }
    s.on = false;
}

bool enabled() { return S().on; }
bool deterministic() { return S().on && S().det; }
uint64_t frame() { return S().frames.load(); }
const std::string& out_dir() { return S().out; }
bool finished() { return S().finished; }
int exit_code() { return S().exit_code; }
const std::vector<Checkpoint>& checkpoints() { return S().cps; }
uint64_t last_hash() { return S().last_hash; }
uint64_t chain_hash() { return S().chain; }
const GameState& last_state() { return S().gs; }
PadFrame current_pad() { return S().pad; }
void set_live_pad(const PadFrame& p) { S().live_pad = p; }
void set_screenshot_provider(ScreenshotFn fn, void* user, ScreenshotPollFn poll) { S().shot_fn = fn; S().shot_user = user; S().shot_poll_fn = poll; }
bool take_screenshot(const std::string& png_path, int timeout_ms) {
    State& s = S();
    if (!s.shot_fn || !s.shot_fn(png_path.c_str(), s.shot_user)) return false;
    if (!s.shot_poll_fn) return true;
    const uint64_t t0 = GetTickCount64();
    for (;;) {
        const int r = s.shot_poll_fn(png_path.c_str(), s.shot_user);
        if (r != 0) return r > 0;
        if (static_cast<int64_t>(GetTickCount64() - t0) >= timeout_ms) return false;
        Sleep(10);
    }
}
void start_wall_deadline(double seconds, void (*report)()) {
    State& s = S();
    if (seconds <= 0) return;
    s.timers.emplace_back(deadline_loop, &s, seconds, report);
}
void start_timed_shots(double first_s, double every_s, const std::string& dir) {
    State& s = S();
    if (first_s <= 0) return;
    s.timers.emplace_back(shots_loop, &s, first_s, every_s, dir);
}
void set_exit_handler(ExitFn fn) { S().exit_fn = fn; }
void set_mouse_sink(MouseSinkFn fn) { S().mouse_sink = fn; }
void set_key_sink(KeySinkFn fn) { S().key_sink = fn; }

void reset_for_tests() {
    State& s = S();
    s.stop_wall = true;
    for (std::thread& t : s.timers) if (t.joinable()) t.join();
    s.timers.clear();
    s.stop_wall = false;
    detail::log_watch_clear();
    s.frame_tid.store(0, std::memory_order_relaxed);   // a recycled thread id must not move the next run's clock
    if (s.trace) { std::fclose(s.trace); s.trace = nullptr; }
    clear_kernel_providers();
    ExitFn keep = s.exit_fn;
    MouseSinkFn keep_m = s.mouse_sink;
    KeySinkFn keep_k = s.key_sink;
    ScreenshotFn sf = s.shot_fn;
    ScreenshotPollFn sp = s.shot_poll_fn;
    void* su = s.shot_user;
    s.~State();
    new (&s) State;
    s.exit_fn = keep;
    s.mouse_sink = keep_m;
    s.key_sink = keep_k;
    s.shot_fn = sf;
    s.shot_poll_fn = sp;
    s.shot_user = su;
    if (const GameProbes* p = game::info().probes; p && p->reset) p->reset();
}

void set_clock_rate(double hz) {
    if (!S().on) return;
    timestep::set_run_rate(hz);
    NXLOG_I("tt", "game step: %s (frame %llu)", hz > 0 ? ("exactly 1/" + std::to_string(hz) + " s").c_str() : "the configured mode", static_cast<unsigned long long>(S().frames.load()));
}

bool virtual_tick(uint64_t* ticks) {
    State& s = S();
    if (!s.on || !s.det) return false;
    *ticks = s.vticks.fetch_add(1, std::memory_order_relaxed) + 1;     // every read moves time forward by one tick (52 ns)
    return true;
}

// A sleep of the frame thread moves the clock by its length and only yields (the frame's own pacing waits must end). Any other thread waits for
// the clock to reach its wake time, which the frames move, and for at most the sleep's length of host time; it does not move the clock. When
// every sleep moved it, a thread that polls in a sleep loop (a pad vibration thread, for one) pushed the
// shared clock as fast as the host ran the loop: 150 to 2000 virtual seconds per 60 frames, and the New Game load waited forever on the
// planet loading screen (docs/GAME_BOUNDARY.md, "Deterministic mode and the New Game stall").
bool virtual_sleep(int64_t ns) {
    State& s = S();
    if (!s.on || !s.det) return false;
    const uint64_t add = ns > 0 ? static_cast<uint64_t>(ns) * 192 / 10000 : 0;
    const DWORD ft = s.frame_tid.load(std::memory_order_relaxed);
    if (add == 0 || ft == 0 || ft == GetCurrentThreadId()) {
        if (add) s.vticks.fetch_add(add, std::memory_order_relaxed);
        SwitchToThread();
        return true;
    }
    const uint64_t target = s.vticks.load(std::memory_order_relaxed) + add;
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    const double limit_s = static_cast<double>(ns) * 1e-9;
    for (;;) {
        if (s.vticks.load(std::memory_order_relaxed) >= target || !s.on) return true;
        QueryPerformanceCounter(&t);
        const double left = limit_s - static_cast<double>(t.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart);
        if (left <= 0) return true;
        if (left >= 0.002) Sleep(1);
        else SwitchToThread();
    }
}

uint64_t rng_next() {
    State& s = S();
    if (s.on && s.det) return splitmix(s.seed + 0x9E3779B97F4A7C15ull * (s.rng_ctr.fetch_add(1) + 1));
    uint64_t v = 0;
    rng_fill(&v, 8);
    return v;
}

void rng_fill(void* p, size_t n) {
    State& s = S();
    uint8_t* b = static_cast<uint8_t*>(p);
    if (s.on && s.det) {
        size_t i = 0;
        while (i < n) {
            const uint64_t v = splitmix(s.seed + 0x9E3779B97F4A7C15ull * (s.rng_ctr.fetch_add(1) + 1));
            const size_t m = std::min<size_t>(8, n - i);
            std::memcpy(b + i, &v, m);
            i += m;
        }
        return;
    }
    using Fn = BOOLEAN(WINAPI*)(PVOID, ULONG);
    static Fn fn = reinterpret_cast<Fn>(GetProcAddress(LoadLibraryA("advapi32.dll"), "SystemFunction036"));
    if (fn) fn(p, static_cast<ULONG>(n));
    else for (size_t i = 0; i < n; i++) b[i] = static_cast<uint8_t>(rand());
}

namespace {
bool provide_rng(void* p, size_t n) {
    if (!deterministic()) return false;
    rng_fill(p, n);
    return true;
}
bool provide_wall(uint64_t* ft) {
    if (!deterministic()) return false;
    *ft = wall_filetime();
    return true;
}
const kernel::ClockProvider kClockProvider{virtual_tick, virtual_sleep, provide_wall};
void install_kernel_providers() {
    kernel::set_clock_provider(&kClockProvider);
    kernel::set_rng_provider(provide_rng);
}
void clear_kernel_providers() {
    kernel::set_clock_provider(nullptr);
    kernel::set_rng_provider(nullptr);
    kernel::set_svc_observer(nullptr);
}
}  // namespace

uint64_t wall_filetime() {
    State& s = S();
    if (s.on && s.det) return 133000000000000000ull + (s.vticks.load() - s.vbase) * 10000ull / 192ull;     // 100 ns units, fixed epoch
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// ---------------------------------------------------------------------------------------------------------------------------
namespace detail {

const RegionTable* regions() { return S().have_table ? &S().table : nullptr; }
const std::string& script_path() { return S().script; }
bool require_shots() { return S().require_shots; }
uint64_t current_frame_hash() { return S().last_hash; }
void set_pad(const PadFrame& p) { S().pad = p; }
uint64_t main_frame_count() { return S().frames.load(); }

void add_checkpoint(const std::string& label) {
    State& s = S();
    Checkpoint c;
    c.label = label;
    c.frame = s.frames.load() ? s.frames.load() - 1 : 0;
    c.hash = s.last_hash;
    s.cps.push_back(c);
    if (s.trace) std::fprintf(s.trace, "{\"f\":%llu,\"h\":\"%016llx\",\"label\":\"%s\"}\n", static_cast<unsigned long long>(c.frame), static_cast<unsigned long long>(c.hash), json_escape(label).c_str());
    NXLOG_I("tt", "checkpoint '%s' at frame %llu hash %016llx", label.c_str(), static_cast<unsigned long long>(c.frame), static_cast<unsigned long long>(c.hash));
}

bool key_event(const std::string& name, bool down) {
    State& s = S();
    if (!s.key_sink) return false;
    if (!s.key_sink(name.c_str(), down)) return false;
    auto it = std::find(s.keys_down.begin(), s.keys_down.end(), name);
    if (down) {
        if (it == s.keys_down.end()) s.keys_down.push_back(name);
    } else if (it != s.keys_down.end()) {
        s.keys_down.erase(it);
    }
    return true;
}

bool shot_request(const std::string& name, std::string* why_not) {
    State& s = S();
    s.shot_name = name;
    s.shot_path = join(s.shot_dir, name + ".png");
    if (!s.shot_fn) {
        s.missing_shots.push_back(name);
        if (why_not) *why_not = "no screenshot provider registered (the graphics layer has not hooked in yet)";
        NXLOG_W("tt", "@shot %s: no screenshot provider", name.c_str());
        return false;
    }
    s.shot_start_ms = GetTickCount64();
    if (!s.shot_fn(s.shot_path.c_str(), s.shot_user)) {
        s.missing_shots.push_back(name);
        if (why_not) *why_not = "the screenshot provider refused the request";
        return false;
    }
    return true;
}

ShotStatus shot_poll(std::string* why_not) {
    State& s = S();
    int r = 1;
    if (s.shot_poll_fn) r = s.shot_poll_fn(s.shot_path.c_str(), s.shot_user);
    if (r > 0) {
        s.shots.push_back(s.shot_name + ".png");
        return ShotStatus::Done;
    }
    const bool timed_out = static_cast<int64_t>(GetTickCount64() - s.shot_start_ms) >= s.shot_timeout_ms;
    if (r == 0 && !timed_out) return ShotStatus::Pending;
    s.missing_shots.push_back(s.shot_name);
    if (why_not) *why_not = r < 0 ? "the screenshot provider failed" : "the file was not written within " + std::to_string(s.shot_timeout_ms) + " ms";
    NXLOG_W("tt", "@shot %s: %s", s.shot_name.c_str(), why_not ? why_not->c_str() : "failed");
    return ShotStatus::Failed;
}

bool take_shot(const std::string& name, std::string* why_not) {
    if (!shot_request(name, why_not)) return false;
    for (;;) {
        const ShotStatus r = shot_poll(why_not);
        if (r != ShotStatus::Pending) return r == ShotStatus::Done;
        Sleep(10);
    }
}

// ---- @waitlog: the regexes of the script's steps, tapped from the runtime log ----
namespace {
struct LogWatch { std::regex rx; bool seen = false; };
std::mutex g_watch_mu;
std::vector<std::unique_ptr<LogWatch>> g_watches;
void watch_tap(LogLevel, const char* line, void*) {
    std::lock_guard<std::mutex> lk(g_watch_mu);
    for (auto& w : g_watches)
        if (!w->seen && std::regex_search(line, w->rx)) w->seen = true;
}
}  // namespace

int log_watch_add(const std::string& regex) {
    std::lock_guard<std::mutex> lk(g_watch_mu);
    auto w = std::make_unique<LogWatch>();
    try { w->rx = std::regex(regex); } catch (const std::regex_error&) { return -1; }
    g_watches.push_back(std::move(w));
    log_set_tap(watch_tap, nullptr);
    return static_cast<int>(g_watches.size()) - 1;
}

bool log_watch_seen(int id) {
    std::lock_guard<std::mutex> lk(g_watch_mu);
    return id >= 0 && id < static_cast<int>(g_watches.size()) && g_watches[static_cast<size_t>(id)]->seen;
}

void log_watch_clear() {
    log_set_tap(nullptr, nullptr);
    std::lock_guard<std::mutex> lk(g_watch_mu);
    g_watches.clear();
}

}  // namespace detail

void end_run(int code, const char* verdict, const std::string& message) {
    State& s = S();
    {
        std::lock_guard<std::recursive_mutex> lk(s.mu);
        if (s.finished) return;
        s.finished = true;
        s.exit_code = code;
        s.verdict = verdict;
        s.message = message;
        s.stop_wall = true;
        NXLOG_I("tt", "run ended: %s (exit code %d) %s", verdict, code, message.c_str());
        // the unknown-call tally of the whole run (SVCs, IPC commands, HLE stubs), for the sweep report; not on a freeze, where a lock may be held
        if (code == 0 || code == 1) NXLOG_I("tt", "state at the end of the run:\n%s", nxrt::summary().c_str());
        release_held_keys(s);
        write_result(s, code, verdict, message);
        flush_files(s);
        if (s.trace) std::fflush(s.trace);
    }
    ExitFn fn = s.exit_fn ? s.exit_fn : default_exit;
    fn(code);
}

void frame_boundary(::Ctx* c) {
    State& s = S();
    if (!s.on) return;
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    if (s.finished) return;
    const uint64_t k = s.frames.load();
    s.frames.store(k + 1);
    s.frame_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    diag_frame_tick_nolisten();
    if (s.det) {
        const uint64_t target = s.vbase + k * s.step_ticks;
        uint64_t cur = s.vticks.load();
        while (cur < target && !s.vticks.compare_exchange_weak(cur, target)) {}
    }
    detail::install_input();
    GameState g;
    g.previous = s.have_prev ? &s.prev : nullptr;
    const bool valid = read_state(&g, c);
    g.previous = nullptr;
    s.gs = g;
    s.gs_valid = valid;
    s.prev = g;
    s.have_prev = valid;
    const uint64_t h = hash_state(s, k, g, valid);
    s.last_hash = h;
    s.chain = mix64(s.chain, h);
    const double t = now_ms(s);
    const double ms = t - s.last_ms;
    s.last_ms = t;
    if (k - s.last_mem_frame >= 30 || k == 0) {
        s.last_mem_frame = k;
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc)) s.mem_mb = static_cast<double>(pmc.PrivateUsage) / 1048576.0;
        write_trace_line(s, k, h, g, valid, ms, s.mem_mb);
    } else {
        write_trace_line(s, k, h, g, valid, ms, 0);
    }
    PadFrame live;
    const bool have_live = detail::sample_live(&live);
    if (!have_live) live = s.live_pad;
    if (s.replaying) {
        s.pad = s.play.at(k);
        if (s.replay_exit && s.play.end_frame && k >= s.play.end_frame && !s.runner_on) {
            end_run(0, "pass", "the input trace ended");
            return;
        }
    } else if (s.runner_on) {
        s.runner.frame(k, g, valid, c);
        s.pad = s.runner.pad();
        if (s.finished) return;
    } else {
        s.pad = live;
    }
    if (s.recording) s.rec.record(k, s.recording && (s.replaying || s.runner_on) ? s.pad : live);
    detail::publish_pad(s.pad, s.replaying || s.runner_on, s.runner_on && s.runner.has_key_steps());
    if ((s.pad.mouse_dx != 0 || s.pad.mouse_dy != 0) && s.mouse_sink && (s.replaying || s.runner_on)) s.mouse_sink(s.pad.mouse_dx, s.pad.mouse_dy);
    if (s.max_frames && k + 1 >= s.max_frames && !s.finished) end_run(1, "fail", "frame budget (max_frames) exhausted");
}

namespace detail {
void on_svc(::Ctx* c) {
    State& s = S();
    ::Ctx* expect = nullptr;
    s.svc_ctx.compare_exchange_strong(expect, c);          // the first guest thread to make an SVC is the frame thread
    if (s.svc_ctx.load() != c) return;
    const uint64_t n = s.svc_count.fetch_add(1) + 1;
    const uint32_t every = s.svc_n;
    if (every && n % every == 0) frame_boundary(c);
}
}  // namespace detail

}  // namespace nxrt::tt
