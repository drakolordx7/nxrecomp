// SPDX-License-Identifier: GPL-3.0-or-later
// The script steps Metroid Prime Remastered registers with the test tooling (nxrt/tt.h GameProbes::steps). The base's grammar (tt_script.h) has
// no game in it: these lines parse only with this module registered, and run in the frame hook with the live context.
//
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
// The state keys @assert and @mashuntil take besides the base fields: state spd pitch cam morph orbit camyaw campitch beam visor msl (mpr_tt_probe.cpp).
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "mpr_tt_probe.h"

namespace nxrt::tt::mpr {

namespace {

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
    const GameState& s = r.state;
    const bool met = r.valid && s.have_world && std::memcmp(s.area, st.id, 16) == 0 && std::memcmp(s.world, st.id2, 16) == 0;
    if (met) { w.warped = false; return StepStatus::Done; }
    if (r.timed_out()) { *r.error = "timeout waiting for the warp target area to become current"; return StepStatus::Fail; }
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
