// SPDX-License-Identifier: GPL-3.0-or-later
// The script steps Metroid Prime Remastered registers with the test tooling (nxrt/tt.h GameProbes::steps). The base's grammar (tt_script.h) has
// no game in it: these lines parse only with this module registered, and run in the frame hook with the live context.
//
//   @area <id> [timeout=<n>]  wait until the current area id equals <id> (GUID text 8-4-4-4-12, or a name from the region table)
//   @world <id> [timeout=<n>] the same for the world id
//   @pos <x> <y> <z> [<r>] [timeout=<n>]   wait until the player is within r (default 1.0) world units of the point
//   @igt <seconds> [timeout=<n>]           wait until the in-game play time is >= seconds
//   @run <seconds> [timeout=<n>] [soft]    wait until the in-game play time has advanced by <seconds> since this step began (the game clock stands
//                             still while an area loads, so this is "the game ran for that long"). `soft`: when the timeout runs out, log
//                             "SOFT-TIMEOUT" and go on instead of failing the run
//   @mash <keys> <period_ms> area|world <id> [timeout=<ms>]
//                             press the buttons for 250 ms of host time every <period_ms> until the area (or world) becomes current: gets through the
//                             front end without knowing how long each menu and load takes. Timeout in host ms, default 600000
//   @warp <world> <area>      warp to the area (the practice mod's sequence, mpr_tt_probe.cpp); then waits until that area is current. The warp waits
//                             until the game's resource loader has been idle for 10 frames ([test] warp_wait_loader, default on; S42: the warp's QuitGame
//                             tears the game instance down while the loader may still be building assets for it), or [test] warp_loader_max_ms (30000)
//                             of host time with a warning. <world>, <area>: GUID text or names from the region table
//   @give <item> <amount>     set an item of the player state (CPlayerStateMP1 item table): amount and capacity. <item> is a number or the game's name
//                             without case or underscores (MorphBall, ChargeBeam, Missiles, XRayVisor, WaveBeam ...)
//   @grant <item|all> [<amount> [<capacity>]]   the same through the power-up array (amount default 1, capacity default = amount); `all` = every upgrade.
//                             Retries each frame until the player state exists (timeout as every wait)
//   @call <offset> [<arg>...] call the guest function at main+offset with up to 6 arguments (numbers, $gs $ps $mgr $player); logs the result
//   @objs <class>...          list every live object of the named classes with its position (door dock pickup special actor platform trigger player, or a
//                             main-module vtable offset): a memory scan for the vtable pointer; result in <out>/objs.txt, one `class x y z addr` line per object
//   @rays <in> <out>          batch of ray casts against the static collision of the loaded areas (the game's own NCollisionMP1::RayStaticIntersection):
//                             in = lines `x y z dx dy dz len` (dir is a unit vector), out = lines `hit t px py pz nx ny nz` (hit 0/1). Runs in the frame hook,
//                             the game stands still meanwhile
//   @probe <in> <out>         the player's own collision box against the static world at positions: in = lines `x y z`, out = `0` free / `1` collides per line
//   @watch on [vely|posy|both] | off   hardware write watch (debug registers) on the player's physics velocity.y and/or transform position.y; every write is
//                             logged with the writing instruction, the frame and the value (lines `watch:` in the log); off stops it and logs how many hits were seen
// The state keys @assert and @mashuntil take (mpr_tt_probe.cpp): area world (ids, == and != only), igt health px py pz vx vy vz speed, state spd pitch cam
// morph orbit camyaw campitch beam visor msl.
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "mpr_tt_probe.h"

namespace nxrt::tt::mpr {

namespace {

std::string low(std::string v) {
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return v;
}

// A readable field of the state.
const StateField* readable(const StepRun& r, const char* key) {
    const StateField* f = r.valid ? r.state.field(key) : nullptr;
    return f && f->shown ? f : nullptr;
}

// EItemType (CPlayerStateMP1::EItemType, docs/mpr/enums.json): name (case and underscores ignored) or number -> id; -1 when unknown. The one table
// @give and @grant read.
int64_t item_id(const std::string& tok) {
    static const char* const names[] = {"powerbeam", "icebeam", "wavebeam", "plasmabeam", "missiles", "scanvisor", "morphballbombs", "powerbombs",
                                        "flamethrower", "thermalvisor", "chargebeam", "supermissile", "grapplebeam", "xrayvisor", "icespreader",
                                        "spacejumpboots", "morphball", "combatvisor", "boostball", "spiderball", "powersuit", "gravitysuit",
                                        "variasuit", "phazonsuit", "energytanks"};
    std::string k;
    for (char c : tok) if (c != '_') k.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) if (k == names[i]) return static_cast<int64_t>(i);
    if (k == "wavebuster") return 0x1c;
    if (k == "bombs") return 6;
    if (k == "spacejump") return 0xf;
    if (!k.empty() && std::isdigit(static_cast<unsigned char>(k[0]))) {
        char* end = nullptr;
        const long v = std::strtol(k.c_str(), &end, 0);
        if (end && *end == 0 && v >= 0 && v < 0x29) return v;
    }
    return -1;
}

// "IN OUT" (the two file names of @probe and @rays, joined by one space in Step::text).
bool split_files(const std::string& text, std::string* in, std::string* out) {
    const size_t sp = text.find(' ');
    if (sp == std::string::npos) return false;
    *in = text.substr(0, sp);
    *out = text.substr(sp + 1);
    return true;
}

// <id> [timeout=<n>] for @area (want_area) and @world.
bool parse_where(const StepLine& l, Step* s, std::string* err, bool want_area) {
    std::vector<std::string> pos;
    int64_t timeout = -1;
    if (!l.take_timeout(&pos, &timeout, err)) return false;
    if (pos.size() != 1) return l.usage(want_area ? "@area <id> [timeout=<n>]" : "@world <id> [timeout=<n>]", err);
    if (!l.id(pos[0], want_area, s->id, err)) return false;
    s->name1 = pos[0];
    s->text = want_area ? "area" : "world";
    s->timeout = timeout;
    return true;
}
bool parse_area(const StepLine& l, Step* s, std::string* err) { return parse_where(l, s, err, true); }
bool parse_world(const StepLine& l, Step* s, std::string* err) { return parse_where(l, s, err, false); }

bool parse_pos(const StepLine& l, Step* s, std::string* err) {
    std::vector<std::string> pos;
    int64_t timeout = -1;
    if (!l.take_timeout(&pos, &timeout, err)) return false;
    if (pos.size() != 3 && pos.size() != 4) return l.usage("@pos <x> <y> <z> [<r>] [timeout=<n>]", err);
    if (!l.real(pos[0], &s->x, err) || !l.real(pos[1], &s->y, err) || !l.real(pos[2], &s->z, err)) return false;
    s->r = 1.0;
    if (pos.size() == 4) {
        if (!l.real(pos[3], &s->r, err)) return false;
        if (s->r < 0.0) { *err = "@pos radius must be >= 0, got '" + pos[3] + "'"; return false; }
    }
    s->timeout = timeout;
    return true;
}

bool parse_igt(const StepLine& l, Step* s, std::string* err) {
    std::vector<std::string> pos;
    int64_t timeout = -1;
    if (!l.take_timeout(&pos, &timeout, err)) return false;
    if (pos.size() != 1) return l.usage("@igt <seconds> [timeout=<n>]", err);
    if (!l.real(pos[0], &s->x, err)) return false;
    if (s->x < 0.0) { *err = "@igt seconds must be >= 0, got '" + pos[0] + "'"; return false; }
    s->timeout = timeout;
    return true;
}

bool parse_run(const StepLine& l, Step* s, std::string* err) {
    std::vector<std::string> pos;
    int64_t timeout = -1;
    if (!l.take_timeout(&pos, &timeout, err)) return false;
    s->count = 0;                                    // 1: soft
    if (pos.size() == 2 && low(pos[1]) == "soft") { s->count = 1; pos.pop_back(); }
    if (pos.size() != 1) return l.usage("@run <seconds> [timeout=<n>] [soft]", err);
    if (!l.real(pos[0], &s->x, err)) return false;
    if (s->x < 0.0) { *err = "@run seconds must be >= 0, got '" + pos[0] + "'"; return false; }
    s->timeout = timeout;
    return true;
}

bool parse_mash(const StepLine& l, Step* s, std::string* err) {
    const char* form = "@mash <keys> <period_ms> area|world <id> [timeout=<ms>]";
    std::vector<std::string> pos;
    int64_t timeout = -1;
    if (!l.take_timeout(&pos, &timeout, err)) return false;
    if (pos.size() != 4) return l.usage(form, err);
    if (!l.keys(pos[0], &s->buttons, err)) return false;
    if (!l.real(pos[1], &s->x, err)) return false;
    const std::string what = low(pos[2]);
    if (what != "area" && what != "world") return l.usage(form, err);
    s->text = what;
    if (!l.id(pos[3], what == "area", s->id, err)) return false;
    s->name1 = pos[3];
    s->timeout = timeout;
    return true;
}

bool parse_warp(const StepLine& l, Step* s, std::string* err) {
    if (l.t.size() != 3) return l.usage("@warp <world> <area>", err);
    if (!l.id(l.t[1], false, s->id2, err)) return false;
    if (!l.id(l.t[2], true, s->id, err)) return false;
    s->name1 = l.t[1];
    s->name2 = l.t[2];
    return true;
}

bool parse_give(const StepLine& l, Step* s, std::string* err) {
    if (l.t.size() != 3) return l.usage("@give <item> <amount>", err);
    const int64_t item = item_id(l.t[1]);
    if (item < 0) { *err = "@give: unknown item '" + l.t[1] + "'"; return false; }
    s->x = static_cast<double>(item);
    return l.real(l.t[2], &s->y, err);
}

bool parse_grant(const StepLine& l, Step* s, std::string* err) {
    const size_t n = l.t.size();
    if (n < 2 || n > 4) return l.usage("@grant <item|all> [<amount> [<capacity>]]", err);
    s->text = l.t[1];
    s->count = -1;
    std::string all = l.t[1];
    for (char& c : all) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (all != "all") {
        s->count = item_id(l.t[1]);
        if (s->count < 0) { *err = "unknown item '" + l.t[1] + "' (an EItemType name or a number)"; return false; }
    }
    s->x = 1;
    if (n >= 3 && !l.real(l.t[2], &s->x, err)) return false;
    s->y = s->x;
    if (n == 4 && !l.real(l.t[3], &s->y, err)) return false;
    return true;
}

bool parse_call(const StepLine& l, Step* s, std::string* err) {
    const size_t n = l.t.size();
    if (n < 2 || n > 8) return l.usage("@call <offset> [<arg>...] (up to 6 arguments)", err);
    if (!l.count(l.t[1], &s->count, err)) return false;
    s->text = l.rest(2);
    return true;
}

bool parse_objs(const StepLine& l, Step* s, std::string* err) {
    if (l.t.size() < 2) return l.usage("@objs <class>... (door dock pickup special actor platform trigger player or a vtable offset)", err);
    s->text = l.rest(1);
    return true;
}

bool parse_files(const StepLine& l, Step* s, std::string* err, const char* form) {
    if (l.t.size() != 3) return l.usage(form, err);
    s->text = l.rest(1);
    return true;
}
bool parse_probe(const StepLine& l, Step* s, std::string* err) { return parse_files(l, s, err, "@probe <in-file> <out-file>"); }
bool parse_rays(const StepLine& l, Step* s, std::string* err) { return parse_files(l, s, err, "@rays <in-file> <out-file>"); }

bool parse_watch(const StepLine& l, Step* s, std::string* err) {
    const size_t n = l.t.size();
    if (n < 2 || n > 3) return l.usage("@watch on [vely|posy|both] | @watch off", err);
    auto low = [](std::string v) { for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return v; };
    s->text = low(l.t[1]);
    if (s->text != "on" && s->text != "off") { *err = "@watch: expected on or off, got '" + l.t[1] + "'"; return false; }
    s->y = 3;
    if (n == 3) {
        const std::string w = low(l.t[2]);
        if (w == "vely") s->y = 1; else if (w == "posy") s->y = 2; else if (w == "both") s->y = 3;
        else { *err = "@watch on: expected vely, posy or both, got '" + l.t[2] + "'"; return false; }
    }
    return true;
}

// The warp's state between the frames it spans (one warp runs at a time); reset when the step starts.
struct WarpGate {
    static constexpr int kIdleFrames = 10;
    bool warped = false, gate_open = false, gate_logged = false;
    int idle_frames = 0;
    bool wait_loader = true;
    uint64_t loader_max_ms = 30000, gate_start_ms = 0;
};
WarpGate g_warp;

StepStatus run_warp(StepRun& r) {
    const Step& st = r.step;
    WarpGate& w = g_warp;
    if (r.first) {
        w = WarpGate();
        w.wait_loader = config_get_bool("test", "warp_wait_loader", true);
        w.loader_max_ms = static_cast<uint64_t>(std::max<int64_t>(0, config_get_int("test", "warp_loader_max_ms", 30000)));
        w.gate_open = !w.wait_loader;
        w.gate_start_ms = GetTickCount64();
    }
    if (!w.warped && !w.gate_open) {
        uint32_t pending = 0;
        const detail::LoaderState ls = detail::loader_state(&pending);
        const uint64_t waited = GetTickCount64() - w.gate_start_ms;
        if (ls == detail::LoaderState::Unknown) {
            w.gate_open = true;                     // nothing readable to wait for (no game globals yet, or a test rig without them)
        } else if (ls == detail::LoaderState::Idle) {
            if (++w.idle_frames >= WarpGate::kIdleFrames) {
                w.gate_open = true;
                if (waited >= 100) NXLOG_I("tt", "warp gate: the resource loader was idle after %llu ms", static_cast<unsigned long long>(waited));
            }
        } else {
            if (!w.gate_logged) { w.gate_logged = true; NXLOG_I("tt", "warp gate: waiting for the resource loader (%u queued)", pending); }
            w.idle_frames = 0;
            if (waited >= w.loader_max_ms) {
                w.gate_open = true;
                NXLOG_W("tt", "warp gate: the resource loader is still busy after %llu ms (%u queued); warping anyway",
                        static_cast<unsigned long long>(waited), pending);
            }
        }
        if (!w.gate_open) return StepStatus::Wait;
    }
    if (!w.warped) {
        if (detail::warp(r.ctx, st.id2, st.id)) {
            w.warped = true;
            *r.start_frame = r.frame;
            NXLOG_I("tt", "warp issued: world %s area %s (frame %llu)", tt_guid_to_text(st.id2).c_str(), tt_guid_to_text(st.id).c_str(), static_cast<unsigned long long>(r.frame));
            return StepStatus::Wait;                       // the quit is processed by the game over the next frames
        }
        if (r.timed_out()) { *r.error = "cannot warp: no state manager seen (the game is not in a level, or the hooks are not built in)"; return StepStatus::Fail; }
        return StepStatus::Wait;
    }
    const StateField* area = readable(r, "area");
    const StateField* world = readable(r, "world");
    const bool met = area && world && std::memcmp(area->id, st.id, 16) == 0 && std::memcmp(world->id, st.id2, 16) == 0;
    if (met) { w.warped = false; return StepStatus::Done; }
    if (r.timed_out()) { *r.error = "timeout waiting for the warp target area to become current"; return StepStatus::Fail; }
    return StepStatus::Wait;
}

// A wait of the base's kind: done when `met`, a failure naming `what` after the step's timeout in frames.
StepStatus wait_cond(StepRun& r, bool met, const std::string& what) {
    if (met) return StepStatus::Done;
    if (r.timed_out()) { *r.error = "timeout after " + std::to_string(r.timeout) + " frames waiting for " + what; return StepStatus::Fail; }
    return StepStatus::Wait;
}

std::string id_label(const Step& st) { return st.text + " " + (st.name1.empty() ? tt_guid_to_text(st.id) : st.name1); }

StepStatus run_where(StepRun& r) {
    const StateField* f = readable(r, r.step.text.c_str());
    return wait_cond(r, f && std::memcmp(f->id, r.step.id, 16) == 0, id_label(r.step));
}

StepStatus run_pos(StepRun& r) {
    const Step& st = r.step;
    const StateField* f = readable(r, "pos");
    bool met = false;
    if (f) {
        const double dx = f->vec[0] - st.x, dy = f->vec[1] - st.y, dz = f->vec[2] - st.z;
        met = std::sqrt(dx * dx + dy * dy + dz * dz) <= st.r;
    }
    char b[96];
    std::snprintf(b, sizeof b, "position (%.2f, %.2f, %.2f) within %.2f", st.x, st.y, st.z, st.r);
    return wait_cond(r, met, b);
}

StepStatus run_igt(StepRun& r) {
    const StateField* f = readable(r, "igt");
    return wait_cond(r, f && f->value >= r.step.x, "the in-game time");
}

double g_run_base = -1;                              // @run: the play time at the first readable frame of the step

StepStatus run_run(StepRun& r) {
    const Step& st = r.step;
    if (r.first) g_run_base = -1;
    bool met = false;
    if (const StateField* f = readable(r, "igt")) {
        if (g_run_base < 0 || f->value < g_run_base) g_run_base = f->value;
        met = f->value - g_run_base >= st.x;
    }
    char b[64];
    std::snprintf(b, sizeof b, "%.1f s of play time", st.x);
    if (st.count && !met) {
        if (r.timed_out()) {
            NXLOG_W("script", "SOFT-TIMEOUT line %d: no %s within %lld frames, going on", st.line, b, static_cast<long long>(r.timeout));
            return StepStatus::Done;
        }
        return StepStatus::Wait;
    }
    return wait_cond(r, met, b);
}

struct MashState { uint64_t start_ms = 0, next_ms = 0, release_ms = 0; bool down = false; };
MashState g_mash;

StepStatus run_mash(StepRun& r) {
    const Step& st = r.step;
    const uint64_t now_ms = GetTickCount64();
    if (r.first) g_mash = MashState{now_ms, now_ms, 0, false};
    const StateField* f = readable(r, st.text.c_str());
    if (f && std::memcmp(f->id, st.id, 16) == 0) { *r.press = 0; return StepStatus::Done; }
    const int64_t to_ms = st.timeout < 0 ? 600000 : st.timeout;
    if (to_ms > 0 && static_cast<int64_t>(now_ms - g_mash.start_ms) >= to_ms) {
        *r.press = 0;
        *r.error = "timeout waiting for " + id_label(st);
        return StepStatus::Fail;
    }
    // The press lasts 250 ms of host time: in deterministic mode a frame is a tiny slice of host time and the front end runs on the host clock.
    if (g_mash.down) {
        if (now_ms >= g_mash.release_ms) { g_mash.down = false; *r.press = 0; }
    } else if (now_ms >= g_mash.next_ms) {
        *r.press = st.buttons;
        g_mash.down = true;
        g_mash.release_ms = now_ms + 250;
        g_mash.next_ms = now_ms + static_cast<uint64_t>(st.x);
    }
    return StepStatus::Wait;
}

StepStatus run_give(StepRun& r) {
    if (!detail::give_item(static_cast<int>(r.step.x), static_cast<int>(r.step.y))) { *r.error = "@give: the player state is not readable yet"; return StepStatus::Fail; }
    return StepStatus::Done;
}

StepStatus run_grant(StepRun& r) {
    const Step& st = r.step;
    if (!detail::grant(st.count, static_cast<int32_t>(st.x), static_cast<int32_t>(st.y))) {
        if (r.timed_out()) { *r.error = "@grant: the player state is not available"; return StepStatus::Fail; }
        return StepStatus::Wait;                       // retry next frame (the game may not have a player state yet)
    }
    NXLOG_I("tt", "grant %s: amount %d capacity %d", st.text.c_str(), static_cast<int>(st.x), static_cast<int>(st.y));
    return StepStatus::Done;
}

StepStatus run_call(StepRun& r) {
    const Step& st = r.step;
    uint64_t res = 0;
    std::string why;
    if (!detail::call_guest(r.ctx, static_cast<uint64_t>(st.count), st.text, &res, &why)) {
        if (r.timed_out()) { *r.error = "@call: " + why; return StepStatus::Fail; }
        return StepStatus::Wait;
    }
    NXLOG_I("tt", "call main+0x%llx(%s) = 0x%llx", static_cast<unsigned long long>(st.count), st.text.c_str(), static_cast<unsigned long long>(res));
    return StepStatus::Done;
}

StepStatus run_objs(StepRun& r) {
    std::string why;
    if (!detail::dump_objects(r.step.text, out_dir() + "/objs.txt", &why)) { *r.error = "@objs: " + why; return StepStatus::Fail; }
    return StepStatus::Done;
}

StepStatus run_probe(StepRun& r) {
    std::string in, out, why;
    if (!split_files(r.step.text, &in, &out) || !detail::probe_positions(r.ctx, in, out, &why)) { *r.error = "@probe: " + why; return StepStatus::Fail; }
    return StepStatus::Done;
}

StepStatus run_rays(StepRun& r) {
    std::string in, out, why;
    if (!split_files(r.step.text, &in, &out) || !detail::cast_rays(r.ctx, in, out, &why)) { *r.error = "@rays: " + why; return StepStatus::Fail; }
    return StepStatus::Done;
}

StepStatus run_watch(StepRun& r) {
    if (r.step.text == "off") detail::watch_stop();
    else if (!detail::watch_start(r.ctx, static_cast<int>(r.step.y))) {
        *r.error = "@watch: the player is not readable (not in a level, or not called from the game hook)";
        return StepStatus::Fail;
    }
    return StepStatus::Done;
}

const GameStepDef kSteps[] = {
    {"@area", &parse_area, &run_where},
    {"@world", &parse_world, &run_where},
    {"@pos", &parse_pos, &run_pos},
    {"@igt", &parse_igt, &run_igt},
    {"@run", &parse_run, &run_run},
    {"@mash", &parse_mash, &run_mash},
    {"@warp", &parse_warp, &run_warp},
    {"@give", &parse_give, &run_give},
    {"@grant", &parse_grant, &run_grant},
    {"@call", &parse_call, &run_call},
    {"@objs", &parse_objs, &run_objs},
    {"@probe", &parse_probe, &run_probe},
    {"@rays", &parse_rays, &run_rays},
    {"@watch", &parse_watch, &run_watch},
};

}

const GameStepDef* mpr_game_steps() { return kSteps; }
size_t mpr_game_step_count() { return sizeof kSteps / sizeof kSteps[0]; }

}
