// SPDX-License-Identifier: GPL-3.0-or-later
// Test tooling of the runtime (S5): deterministic mode, frame boundary, per-frame state trace and hash, game-state probes,
// input record / replay, the scripted-run runner. Docs: games/mpr/docs/work/S5.md. Everything is off unless a [test] key (or its NXRT_TEST_* variable)
// asks for it, so a normal run pays nothing.
//
// The central idea is ONE frame boundary, tt::frame_boundary(), called once per game frame by whichever source is configured
// ([test] frame_source): the game hook (an entry hook the game registers on its per-frame update; its hook site has to be in the sites the
// game's lifted code was built with), the runtime's diag_frame_tick (the present path), an SVC counter
// (bring-up: svc:N = a pseudo frame every N SVCs of the first guest thread) or the caller itself (unit tests). At each boundary:
//   1. the virtual clock steps (deterministic mode),
//   2. the game state is read from guest memory (probes) and one trace line + state hash is written,
//   3. the script runner / replay decides the pad for the next frame, a recorder notes the live pad,
//   4. the pad is published to the input source (hid) and the raw mouse accumulator.
// So the state in trace line N is the state at the START of frame N and the pad of frame N acts on frame N.
//
// Configuration keys ([test] section, env NXRT_TEST_<KEY>):
//   script=<file.nxs>      run a scripted run (tt_script.h); exit code 0 pass / 1 fail / 5 freeze / 124 wall timeout
//   regions=<file.tsv>     name table for the game's steps and state keys that take a region name (default: regions.tsv next to the script, else none)
//   out=<dir>              output dir (default "testout"): result.json, trace.jsonl, input.nxin, shots/
//   deterministic=1        fixed RNG seed + virtual clock: one frame = clock_step_ticks (default 320000 = 1/60 s at 19.2 MHz)
//   seed=<n>               RNG seed (default 0x5EED5EED)
//   frame_source=hook|tick|svc:N|manual   (default hook)
//   trace=<file>           per-frame JSON lines (default <out>/trace.jsonl when anything is enabled; "none" disables)
//   record=<file>          record the live pad of every frame (input trace, tt_inputfile.h)
//   replay=<file>          feed that input trace instead of the live pad; with replay_exit=1 the run ends where the trace ends
//   hash_regions=<list>    memory folded into the per-frame hash: comma separated 0xADDR+0xSIZE; "module:<name>" = the RW pages of a module
//   max_frames=<n>         end the run (fail) after n frames; wall_timeout_s=<s> (default 900 with a script) ends it with exit code 124
//   freeze_exit=1          a watchdog freeze ends the run with exit code 5 (default on with a script)
//   shot_dir=<dir>         default <out>/shots; require_shots=1 turns a missing screenshot into a failure; shot_timeout_ms=<n> (default 30000) how long @shot waits for its file
// What the tooling knows of a game is its GameProbes (below, nxrt/game.h Info::probes): the frame-source hooks, how to read the state, the
// state keys and the script steps the game adds. A game's own keys ([test] game_base, [probe] ...) are read by that game's probes.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>
#include <string>

#include "nxrt/abi.h"
#include "nxrt/tt_inputfile.h"
#include "nxrt/tt_script.h"

namespace nxrt::tt {

// ---- the game state the probes read ------------------------------------------------------------------------------------------------
// One value the game adds to the state. The trace line, the live status, @assert and the frame hash all iterate the list. The tooling knows no
// game value by name: a world id, a position or a health is a field the game's probes add.
struct StateField {
    enum class Kind : uint8_t {
        Int,       // `value`, printed as an integer
        Float,     // `value`, printed with %.9g
        Id,        // `id`: 16 bytes, printed as 32 hex digits (the live status: GUID text)
        Vec3,      // `vec`: printed as [x,y,z]
    };
    enum class Hash : uint8_t {
        None,      // not part of the hash
        Word,      // its 32-bit value (an integer as unsigned 32 bits, a float as its bits) is the next word folded into the hash
        High,      // its 32-bit value goes into the upper half of the word of the last Word field
        Wide,      // the bits of its double value are one 64-bit word
        Bytes,     // an Id: its 16 bytes
    };
    const char* key = "";              // name in the trace line and the live status, and what @assert can name (GameProbes::state_keys); empty = not printed
    const char* status_key = nullptr;  // name in the live status when it differs from `key`
    const char* alias = nullptr;       // a second name `GameState::field` finds it by
    Kind kind = Kind::Int;
    double value = 0;                  // integers are exact
    uint8_t id[16] = {};
    float vec[3] = {};
    bool shown = true;                 // false: left out of the trace line and refused by @assert (the part of the game it comes from is not readable now)
    bool quiet = false;                // @assert can name it, but the trace line and the live status leave it out
    Hash hash = Hash::Word;
};

struct GameState {
    uint32_t hash_flags = 0;           // folded into the hash after the frame number: which groups of the state were readable
    const GameState* previous = nullptr;    // set only while read_state runs: the state of the last frame boundary when it was readable (for values derived from it)
    std::vector<StateField> fields;     // the game's own values, in the order they are printed and hashed

    StateField& add(StateField f) { fields.push_back(f); return fields.back(); }
    void add_int(const char* key, int64_t v, bool shown = true, StateField::Hash h = StateField::Hash::Word, const char* status_key = nullptr) {
        StateField f; f.key = key; f.status_key = status_key; f.value = static_cast<double>(v); f.shown = shown; f.hash = h;
        fields.push_back(f);
    }
    void add_float(const char* key, double v, bool shown = true, StateField::Hash h = StateField::Hash::Word, const char* status_key = nullptr) {
        StateField f; f.key = key; f.status_key = status_key; f.kind = StateField::Kind::Float; f.value = v; f.shown = shown; f.hash = h;
        fields.push_back(f);
    }
    void add_id(const char* key, const uint8_t id[16], bool shown = true, StateField::Hash h = StateField::Hash::Bytes) {
        StateField f; f.key = key; f.kind = StateField::Kind::Id; std::memcpy(f.id, id, 16); f.shown = shown; f.hash = h;
        fields.push_back(f);
    }
    void add_vec3(const char* key, const float v[3], bool shown = true, StateField::Hash h = StateField::Hash::None) {
        StateField f; f.key = key; f.kind = StateField::Kind::Vec3; std::memcpy(f.vec, v, sizeof f.vec); f.shown = shown; f.hash = h;
        fields.push_back(f);
    }
    const StateField* field(const std::string& key) const {
        for (const StateField& f : fields) if (key == f.key || (f.alias && key == f.alias)) return &f;
        return nullptr;
    }
};

struct Checkpoint { std::string label; uint64_t frame = 0; uint64_t hash = 0; };

// ---- lifecycle ---------------------------------------------------------------------------------------------------------------
// Called from nxrt::init. Reads [test]; returns whether any test feature is on. Idempotent.
bool init();
// Called from nxrt::shutdown (and by end_run): flushes the trace, the recording and result.json (if not already written).
void shutdown();
bool enabled();
bool deterministic();
uint64_t frame();
const std::string& out_dir();

// ---- the frame boundary ---------------------------------------------------------------------------------------------------------
// `c` is the live context when called from a hook (the game's probes and steps may call guest functions with it; it is saved and restored).
void frame_boundary(::Ctx* c = nullptr);
// Unit tests: run with frame_source=manual and call this; `now` hooks are not registered. Resets everything (counters, files closed).
void reset_for_tests();
// Registers the game's entry hooks of the frame source (GameProbes::install_hooks).
void install_game_hooks();

// ---- state ---------------------------------------------------------------------------------------------------------------------------
bool read_state(GameState* out, ::Ctx* c = nullptr);   // the game's probes, now (guest memory); false when nothing could be read. `c`: the hook's context, for probes that call guest functions
const GameState& last_state();             // the state sampled at the last boundary (valid flags inside)
uint64_t last_hash();
uint64_t chain_hash();                     // running hash over every frame hash: what result.json calls final_hash
// ---- input -----------------------------------------------------------------------------------------------------------------------------
PadFrame current_pad();                    // the pad of the frame that just began (script / replay / last live pad)
void set_live_pad(const PadFrame& p);      // the live pad for recording when no hid source is installed (tests; also used by the hid adapter)
// ---- the virtual clock and the RNG (deterministic mode) ----------------------------------------------------------------------
// Installed into the kernel (kernel::set_clock_provider / set_rng_provider) while deterministic mode is on. Return false when deterministic mode is off (the caller uses the host clock).
// The script step `@rate <hz>`: the game's step becomes exactly 1/hz for every frame from here on (nothing waits and no host time enters it), 0 = the
// configured mode again. Areas are loaded at the real rate and measured at a fixed one, in one process.
void set_clock_rate(double hz);
bool virtual_tick(uint64_t* ticks);
bool virtual_sleep(int64_t ns);
void rng_fill(void* p, size_t n);          // seeded stream in deterministic mode, OS entropy otherwise
uint64_t rng_next();
uint64_t wall_filetime();                  // 100 ns units since 1601: fixed epoch + virtual time in deterministic mode, the host clock otherwise

// ---- plumbing --------------------------------------------------------------------------------------------------------------------------
// Screenshot provider: the graphics layer registers a function that asks for the next presented frame to be written as PNG (false: it cannot) and one
// that says whether that file is complete (1 written, 0 not yet, -1 failed). The request never waits, because the thread that runs the frame boundary
// may be the one that presents the frame; @shot completes when the poll says 1. Without a poll function the request is taken as complete.
using ScreenshotFn = bool (*)(const char* png_path, void* user);
using ScreenshotPollFn = int (*)(const char* png_path, void* user);
void set_screenshot_provider(ScreenshotFn fn, void* user, ScreenshotPollFn poll = nullptr);
// A screenshot from any thread that is not the frame thread: requests it and waits up to `timeout_ms` for the file. false: not written.
bool take_screenshot(const std::string& png_path, int timeout_ms = 5000);
// Timed work for the front ends that map a start key of a player or a tool (boot.py presets, the release smoke test) onto the tooling; neither needs a
// script, and both run whether or not the tooling is otherwise on.
//   start_timed_shots: `shot_<n>.png` in `dir` (the working folder when empty), the first `first_s` seconds after the call, then every `every_s` (0 = once).
//   start_wall_deadline: after `seconds` the process ends with exit code 124 (through end_run when the tooling is on); `report` (may be null) logs the
//   game side's state first.
void start_timed_shots(double first_s, double every_s, const std::string& dir = std::string());
void start_wall_deadline(double seconds, void (*report)() = nullptr);
// `[boot] press`, a start key of players and tools, is a script of the tooling: tt::init turns the list `A@12,A@20:500,PLUS@30,A#1500` into script text with
// this (`<button>@<seconds>[:<hold ms>]` seconds since the start, `<button>#<frame>[:<hold ms>]`), runs it with the frame source "tick" (unless [test]
// frame_source says otherwise) and goes on after its last step.
std::string press_list_to_script(const std::string& spec, std::string* err = nullptr);
// Where a replayed / scripted mouse impulse goes (the PC-input layer registers input::mouse_delta_add here; counts per frame).
using MouseSinkFn = void (*)(double dx, double dy);
void set_mouse_sink(MouseSinkFn fn);
// Where a scripted key step goes (the play layer registers its key/button/wheel handler here). down=true presses the named input,
// false releases it. Return false when the name is unknown or cannot be delivered: the run then fails at that script line.
// Keys still down when the run ends (pass, fail or shutdown) are released with down=false.
using KeySinkFn = bool (*)(const char* input_name, bool down);
void set_key_sink(KeySinkFn fn);
// What happens when the run is over (default: flush files, TerminateProcess(code)). Tests install their own.
using ExitFn = void (*)(int code);
void set_exit_handler(ExitFn fn);
bool finished();
int exit_code();
// Ends the run now. verdict: "pass" "fail" "crash" "freeze" "timeout" "error". Writes result.json and calls the exit handler.
void end_run(int code, const char* verdict, const std::string& message);
const std::vector<Checkpoint>& checkpoints();

// frame_source=svc:N: tt installs on_svc as the kernel's SVC observer (kernel::set_svc_observer) for the length of the run.
namespace detail {
void on_svc(::Ctx* c);
}

// ---- the game's probes (nxrt/game.h Info::probes) -----------------------------------------------------------------------------------
// What the test tooling knows of one game: its frame boundary hooks, how to read the state, and the script steps it adds (a name and a handler
// each; the line is parsed when the script loads and runs in the frame hook). Without a table the frame source "hook" registers nothing, read_state
// reads nothing and a script naming a game step is a parse error.

// What a game step sees each frame it runs (until it returns Done or Fail).
struct StepRun {
    const Step& step;
    ::Ctx* ctx;                    // the frame hook's context; null when the frame source is not a hook
    uint64_t frame;
    bool first;                    // the first call of this step
    const GameState& state;
    bool valid;                    // the state was readable this frame
    int64_t timeout;               // frames, 0 = none: the step's timeout, else the script's default
    uint64_t* start_frame;         // the frame the step began; a step may move it to restart its timeout
    uint64_t* press;               // buttons the step holds on the pad for this frame (NpadButton bit mask); a step that sets them clears them when it ends
    std::string* error;            // what a Fail says (the runner adds the script line)
    bool timed_out() const { return timeout > 0 && static_cast<int64_t>(frame - *start_frame) >= timeout; }
};
enum class StepStatus : uint8_t {
    Done,     // finished: the runner goes on to the next step in this same frame
    Wait,     // not finished: the step runs again next frame
    Fail,     // the run fails at this line with *error
};

struct GameStepDef {
    const char* name;                                                       // as written in a script, with the '@': "@warp"
    bool (*parse)(const StepLine& line, Step* out, std::string* err);       // fills `out` (count, x, y, id, id2, name1, name2, text, timeout); false and *err on a bad line
    StepStatus (*run)(StepRun& r);
};

struct IdKeyDef {
    const char* name;
    bool region_area;              // a region name stands for this column of the region table: false the first (world), true the second (area)
};

struct GameProbes {
    void (*install_hooks)() = nullptr;                                      // the frame-boundary hook (calls tt::frame_boundary) and any state capture
    bool (*read_state)(GameState* out, ::Ctx* c) = nullptr;                 // `c` is the hook's context when there is one
    void (*on_frame)(uint64_t frame) = nullptr;                             // start of every scripted frame, before its steps run
    void (*reset)() = nullptr;                                              // reset_for_tests: forget test overrides
    const char* const* state_keys = nullptr;                                // null-terminated: the numeric StateField keys (or aliases) @assert / @mashuntil accept
    const IdKeyDef* id_keys = nullptr;                                      // the Id keys they accept (== and != against a GUID or a region name), the list ends with a null name
    const GameStepDef* steps = nullptr;
    size_t step_count = 0;
};

}

// The adapter to the hid service (S3c's input_source.h) lives in tt_hid.cpp and is compiled when that header exists.
