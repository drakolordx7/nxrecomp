// SPDX-License-Identifier: GPL-3.0-or-later
// Scripted-run DSL (stream S5): text format, parser and step list. The runner (tt_runner.cpp) executes the steps frame by frame.
// Grammar (one step per line, '#' starts a comment, blank lines ignored, keywords are case-insensitive, numbers decimal or 0x hex,
// floats decimal). A "frame" is one game frame boundary (tt.h). Waits count frames from the moment the step starts.
//
//   <n>                               wait n frames (a bare integer)
//   wait <n>                          same
//   <n>:<keys>[/<h>]                  smo-recomp style: wait n frames, then press <keys> for h frames (default 4); two steps: Wait, Press
//   press <keys> [for <n>]            hold the buttons for n frames (default 4), release them, go on. Blocks for n frames.
//   hold <keys>                       press the buttons and keep them down (until release); goes on at once
//   release <keys>|all                release some or all held buttons; goes on at once
//   key <name> [for <n>]              press the named keyboard key / mouse button / wheel notch for n frames (default 4), release, go on
//                                     after n frames (blocks like press)
//   keydown <name>                    press the named input and keep it down; goes on at once
//   keyup <name>                      release the named input; goes on at once
//                                     <name> is the rest of the line up to an optional trailing " for <n>" (case-insensitive): "W",
//                                     "Left Ctrl", "Page Up", "Mouse1", "WheelUp". The name is not checked here; the input layer
//                                     (tt.h set_key_sink) decides, and an unknown name fails the run.
//   stick left|right|l|r <x> <y> [for <n>]   stick vector, each -1.0..1.0 (y up = +). With `for n` it blocks n frames and then returns to 0;
//                                     without it the vector persists until the next stick step for that stick
//   mouse <dx> <dy> [for <n>]         raw mouse counts delivered once per frame for n frames (default 1); blocks for n frames
//   @frame <n>                        wait until the global frame counter is >= n
//   @area <id> [timeout=<n>]          wait until the current area id equals <id> (GUID text 8-4-4-4-12, or a name from the region table)
//   @world <id> [timeout=<n>]         the same for the world id
//   @pos <x> <y> <z> [<r>] [timeout=<n>]   wait until the player is within r (default 1.0) world units of the point
//   @igt <seconds> [timeout=<n>]      wait until the in-game play time is >= seconds
//   @run <seconds> [timeout=<n>]      wait until the in-game play time has advanced by <seconds> since this step began (the game clock stands
//                                     still while an area loads, so this is "the game ran for that long")
//   @mash <keys> <period_ms> area|world <id> [timeout=<ms>]
//                                     press the buttons for 40 frames every <period_ms> of host time until the area (or world) becomes current: gets
//                                     through the front end without knowing how long each menu and load takes. Timeout in host ms, default 600000
//   @sleep <ms>                       wait <ms> of host wall-clock time (not game time: loading and fade-outs run on the host clock even in
//                                     deterministic mode); frames keep being delivered meanwhile
//   @warp <world> <area>              warp to the area (practice-mod sequence, done by the runner); then waits until that area is current
//   @shot <name>                      save a screenshot <shot_dir>/<name>.png at this frame (does not block)
//   @hash [<label>]                   write a labelled hash line into the trace and the result file (does not block)
//   @log <text...>                    write the rest of the line to the log
//   @assert <field> <op> <value>      fail the run now if false. fields: area world frame igt health px py pz vx vy vz speed state.
//                                     ops: == != < <= > >=. area/world compare ids (== and != only); the others compare numbers
//   @timeout <n>                      default timeout (frames) of every later wait; initial 3600; 0 = none
//   @pass | @fail [<message...>]      end the run (exit code 0 or 1) at once
//   @end                              the same as @pass
//
// <keys>: button names joined with '+': A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT LSTICK RSTICK (LSTICK/RSTICK = the stick clicks),
//         also START = PLUS, SELECT = MINUS. Names are case-insensitive. Same bit numbers as nn::hid::NpadButton.
// A wait with a timeout that runs out fails the run (exit code 1) with a message naming the script line.
// A script that reaches its last step passes unless an @fail/@assert failed.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nxrt::tt {

enum class StepKind : uint8_t {
    Wait,        // count frames
    Press,       // buttons for count frames, then release
    Hold,        // buttons
    Release,     // buttons (all = every held button)
    Stick,       // which (0 left 1 right), x, y; count > 0: blocks count frames then zero; count == 0: persists
    Mouse,       // x, y raw counts per frame for count frames
    WaitFrame,   // count = target frame
    WaitArea,    // id (16 bytes), timeout
    WaitWorld,   // id (16 bytes), timeout
    WaitPos,     // x, y, z, r, timeout
    WaitIgt,     // x = seconds, timeout
    WaitMs,      // x = milliseconds of host time
    Mash,        // buttons every x ms until area (text "area") / world (text "world") id; timeout in host ms (-1 = 600000)
    WaitRun,     // x = seconds of in-game play time from the step start, timeout
    Warp,        // id = area, id2 = world
    Shot,        // text = name
    Hash,        // text = label (may be empty)
    Log,         // text
    Assert,      // field, op, text = value text, x = numeric value, id = id value
    Timeout,     // count
    Pass,
    Fail,        // text = message
    Key,         // text = input name, count = frames (-1 = not given: the runner uses 4); blocks like Press
    KeyDown,     // text = input name
    KeyUp,       // text = input name
};

enum class AssertField : uint8_t { Area, World, Frame, Igt, Health, PosX, PosY, PosZ, VelX, VelY, VelZ, Speed, State };
enum class AssertOp : uint8_t { Eq, Ne, Lt, Le, Gt, Ge };

struct Step {
    StepKind kind = StepKind::Wait;
    int line = 0;                  // 1-based source line
    std::string src;               // the source line, trimmed (for messages)
    uint64_t buttons = 0;          // Press/Hold/Release: bit mask (bit n = 1 << NpadButton n); Release all: ~0ull
    int64_t count = 0;             // frames; -1 = "not given" for Press (the runner uses 4) and Mouse (1)
    int64_t timeout = -1;          // frames; -1 = use the script default; 0 = none
    int which = 0;                 // Stick: 0 left, 1 right
    double x = 0, y = 0, z = 0, r = 0;
    uint8_t id[16] = {};           // Area/World/Assert(area, world)/Warp (the area): the resolved id in memory order (see tt_guid_to_bytes)
    uint8_t id2[16] = {};          // Warp: the world id
    std::string name1, name2;      // the id/name text as written (Warp: name1 = world, name2 = area)
    std::string text;
    AssertField field = AssertField::Frame;
    AssertOp op = AssertOp::Eq;
};

struct Script {
    std::vector<Step> steps;
    std::string path;
};

// Name -> (world id, area id) table. File format (UTF-8, '#' comments): `name <TAB> world-guid <TAB> area-guid` per line; an area
// name is unique ("frigate_cargo_freight_lift") and a world entry has an empty area column ("frigate\t<guid>\t").
struct RegionTable {
    struct Entry { std::string name; uint8_t world[16]; uint8_t area[16]; bool has_area; };
    std::vector<Entry> entries;
    bool load(const std::string& path, std::string* err);
    bool load_text(const std::string& text, std::string* err);
    const Entry* find(const std::string& name) const;     // case-insensitive
};

// GUID text "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" -> the 16 bytes as the game's CObjectId holds them in memory.
// VERIFIED against the real game under Eden (docs/S5.md): a Windows-style GUID struct, the first three groups little-endian.
// The layout lives only in these two functions.
bool tt_guid_to_bytes(const std::string& text, uint8_t out[16]);
std::string tt_guid_to_text(const uint8_t in[16]);
std::string tt_guid_hex(const uint8_t in[16]);        // raw: 32 hex digits in memory order

// Parse. `regions` may be null (then only GUID text is accepted as an id). On error returns false and *err =
// "<path>:<line>: message" (the first error). `path` is only used for messages.
bool parse_script(const std::string& text, const std::string& path, const RegionTable* regions, Script* out, std::string* err);
bool parse_script_file(const std::string& path, const RegionTable* regions, Script* out, std::string* err);

}  // namespace nxrt::tt
