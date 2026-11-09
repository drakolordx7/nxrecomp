// SPDX-License-Identifier: GPL-3.0-or-later
// The scripted-run executor (S5): runs the steps of a parsed script (tt_script.h) one frame boundary at a time.
// Each call to Runner::frame is one frame; a step that blocks consumes the frame, a step that does not lets the next one run in the
// same call. Failure and pass call tt::end_run (exit code 1 / 0).
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <thread>

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/ftrace.h"
#include "nxrt/game.h"
#include "nxrt/log.h"
#include "tt_internal.h"

namespace nxrt::tt::detail {

namespace {
std::string tt_guid_text_safe(const uint8_t id[16], bool have) { return have ? tt_guid_to_text(id) : std::string(); }
int16_t to_stick(double v) {
    const double c = v < -1 ? -1 : v > 1 ? 1 : v;
    return static_cast<int16_t>(std::lround(c * 32767.0));
}
}  // namespace

void Runner::reset(const RegionTable* regions) {
    script_ = Script();
    idx_ = 0; started_ = false; done_ = false; remaining_ = 0;
    default_timeout_ = 3600;
    held_ = pressing_ = 0;
    std::memset(stick_, 0, sizeof stick_);
    stick_override_ = false;
    mouse_x_ = mouse_y_ = 0;
    fail_line_ = 0;
    keep_running_ = false;
    has_keys_ = false;
    regions_ = regions;
    live_path_ = config_get_str("test", "live", "");
    live_off_ = 0;
    live_lines_ = 0;
}

bool Runner::load(const std::string& script_file, const RegionTable* regions, std::string* err) {
    reset(regions);
    const bool ok = parse_script_file(script_file, regions, &script_, err);
    for (const Step& st : script_.steps) {
        if (st.kind == StepKind::Key || st.kind == StepKind::KeyDown || st.kind == StepKind::KeyUp) has_keys_ = true;
    }
    register_watches(0);
    return ok;
}

bool Runner::load_text(const std::string& text, const std::string& name, const RegionTable* regions, std::string* err) {
    reset(regions);
    live_path_.clear();
    script_.path = name;
    const bool ok = parse_script(text, name, regions, &script_, err);
    keep_running_ = true;
    register_watches(0);
    return ok;
}

void Runner::register_watches(size_t from) {
    for (size_t i = from; i < script_.steps.size(); i++)
        if (script_.steps[i].kind == StepKind::WaitLog) script_.steps[i].count = log_watch_add(script_.steps[i].text);
}

PadFrame Runner::pad() const {
    PadFrame p;
    p.buttons = held_ | pressing_;
    p.lx = stick_[0][0]; p.ly = stick_[0][1];
    p.rx = stick_[1][0]; p.ry = stick_[1][1];
    if (stick_override_) {
        if (stick_tmp_which_ == 0) { p.lx = stick_tmp_[0]; p.ly = stick_tmp_[1]; }
        else { p.rx = stick_tmp_[0]; p.ry = stick_tmp_[1]; }
    }
    p.mouse_dx = mouse_x_;
    p.mouse_dy = mouse_y_;
    return p;
}

void Runner::fail(const Step& st, const std::string& msg) {
    fail_line_ = st.line;
    done_ = true;
    char b[64];
    std::snprintf(b, sizeof b, ":%d: ", st.line);
    end_run(1, "fail", script_.path + b + msg + "  [" + st.src + "]");
}

bool Runner::eval_assert(const Step& st, const GameState& s, bool valid, std::string* detail) {
    if (st.field == AssertField::GameId) {
        const StateField* f = s.field(st.name2);
        if (!valid || !f || !f->shown) { *detail = "the id is not readable (no game state)"; return false; }
        const bool eq = std::memcmp(f->id, st.id, 16) == 0;
        *detail = "have " + tt_guid_hex(f->id) + ", want " + tt_guid_hex(st.id);
        return st.op == AssertOp::Eq ? eq : !eq;
    }
    double v = 0;
    if (st.field == AssertField::Frame) {
        v = static_cast<double>(main_frame_count() ? main_frame_count() - 1 : 0);
    } else {
        const StateField* f = s.field(st.name2);
        if (!valid || !f || !f->shown) { *detail = "the value is not readable (no game state)"; return false; }
        v = f->value;
    }
    char b[96];
    std::snprintf(b, sizeof b, "have %.6g, want %.6g", v, st.x);
    *detail = b;
    switch (st.op) {
        case AssertOp::Eq: return v == st.x;
        case AssertOp::Ne: return v != st.x;
        case AssertOp::Lt: return v < st.x;
        case AssertOp::Le: return v <= st.x;
        case AssertOp::Gt: return v > st.x;
        case AssertOp::Ge: return v >= st.x;
    }
    return false;
}

bool Runner::write_live_status(uint64_t k, const GameState& s, bool valid, const char* note_in) {
    std::string note_s(note_in);
    for (char& ch : note_s) if (ch == 92 || ch == 34 || static_cast<unsigned char>(ch) < 32) ch = 32;       // keep the JSON line valid (paths have backslashes)
    char b[1024];
    std::snprintf(b, sizeof b, "{\"lines\":%d,\"frame\":%llu,\"valid\":%d", live_lines_, static_cast<unsigned long long>(k), valid ? 1 : 0);
    std::string out = b;
    for (const StateField& f : s.fields) {               // the game's own values, readable or not (a missing group shows its defaults)
        if (!f.key[0] || f.quiet) continue;
        const char* key = f.status_key ? f.status_key : f.key;
        switch (f.kind) {
            case StateField::Kind::Float: std::snprintf(b, sizeof b, ",\"%s\":%.4f", key, f.value); break;
            case StateField::Kind::Int: std::snprintf(b, sizeof b, ",\"%s\":%lld", key, static_cast<long long>(f.value)); break;
            case StateField::Kind::Id: std::snprintf(b, sizeof b, ",\"%s\":\"%s\"", key, tt_guid_text_safe(f.id, f.shown).c_str()); break;
            case StateField::Kind::Vec3: std::snprintf(b, sizeof b, ",\"%s\":[%.3f,%.3f,%.3f]", key, f.vec[0], f.vec[1], f.vec[2]); break;
        }
        out += b;
    }
    out += ",\"note\":\"" + note_s + "\"}\n";
    const std::string tmp = live_path_ + ".status.tmp", fin = live_path_ + ".status";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << out;
    }
    return MoveFileExA(tmp.c_str(), fin.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;       // fails while a reader has the old file open: the caller retries
}

bool Runner::poll_live(uint64_t k, const GameState& s, bool valid) {
    bool wrote = false;
    for (;;) {
        std::string text;
        {
            std::ifstream f(live_path_, std::ios::binary);
            if (f) {
                f.seekg(0, std::ios::end);
                const size_t size = static_cast<size_t>(f.tellg());
                if (size > live_off_) {
                    f.seekg(static_cast<std::streamoff>(live_off_));
                    text.resize(size - live_off_);
                    f.read(&text[0], static_cast<std::streamsize>(text.size()));
                    text.resize(static_cast<size_t>(f.gcount()));
                }
            }
        }
        const size_t nl = text.rfind('\n');
        if (nl != std::string::npos) {
            text.resize(nl + 1);
            live_off_ += text.size();
            int n = 0;
            for (char ch : text) if (ch == '\n') n++;
            Script add;
            std::string err;
            if (!parse_script(text, live_path_, regions_, &add, &err)) {
                NXLOG_E("tt", "live: %s (these %d lines are skipped)", err.c_str(), n);
                live_lines_ += n;
                wrote = write_live_status(k, s, valid, ("parse error: " + err).c_str());
                continue;
            }
            live_lines_ += n;
            const size_t from = script_.steps.size();
            for (Step& st : add.steps) script_.steps.push_back(st);
            register_watches(from);
            if (!add.steps.empty()) return true;       // the status is written when these have run and the runner is idle again
            wrote = write_live_status(k, s, valid, "ok");        // comments and blank lines only
            continue;
        }
        if (!wrote) wrote = write_live_status(k, s, valid, "idle");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void Runner::frame(uint64_t k, const GameState& s, bool valid, ::Ctx* c) {
    if (done_) return;
    if (const GameProbes* gp = game::info().probes; gp && gp->on_frame) gp->on_frame(k);
    mouse_x_ = mouse_y_ = 0;       // the impulse lasts for the frame of the step that sets it
    if (!live_path_.empty()) {         // express lane of live mode: a file <live>.shot holding a name asks for a screenshot now, whatever step is running
        const std::string abort_req = live_path_ + ".abort";       // drops every step that has not finished (a wait that can never be met), releases the pad
        if (GetFileAttributesA(abort_req.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA(abort_req.c_str());
            NXLOG_W("tt", "live: abort requested, dropping %zu pending steps", script_.steps.size() - idx_);
            idx_ = script_.steps.size();
            started_ = false;
            remaining_ = 0;
            held_ = pressing_ = 0;
            std::memset(stick_, 0, sizeof stick_);
            stick_override_ = false;
        }
        const std::string req = live_path_ + ".shot";
        if (GetFileAttributesA(req.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::string name;
            { std::ifstream f(req, std::ios::binary); std::getline(f, name); }
            DeleteFileA(req.c_str());
            while (!name.empty() && (name.back() == 13 || name.back() == ' ')) name.pop_back();
            std::string why;
            if (!name.empty() && !take_shot(name, &why)) NXLOG_W("tt", "live shot %s: %s", name.c_str(), why.c_str());
        }
    }
    for (int guard = 0; guard < 100000; guard++) {
        if (done_) return;
        if (idx_ >= script_.steps.size() && !live_path_.empty()) {
            if (poll_live(k, s, valid)) continue;
            if (done_) return;
        }
        if (idx_ >= script_.steps.size()) {
            if (keep_running_) return;
            done_ = true;
            end_run(0, "pass", "script finished");
            return;
        }
        const Step& st = script_.steps[idx_];
        const bool first = !started_;
        if (first) { started_ = true; start_frame_ = k; }
        auto timeout_of = [&]() { return st.timeout < 0 ? default_timeout_ : st.timeout; };
        auto wait_cond = [&](bool met, const char* what) -> int {       // 1 = continue to next step, 0 = block this frame, -1 = failed
            if (met) { advance(); return 1; }
            const int64_t to = timeout_of();
            if (to > 0 && static_cast<int64_t>(k - start_frame_) >= to) {
                fail(st, std::string("timeout after ") + std::to_string(to) + " frames waiting for " + what);
                return -1;
            }
            return 0;
        };
        int r = 0;
        switch (st.kind) {
            case StepKind::Wait:
                if (first) remaining_ = st.count;
                if (remaining_ <= 0) { advance(); continue; }
                remaining_--;
                return;
            case StepKind::Press:
                if (first) { pressing_ = st.buttons; remaining_ = st.count < 0 ? 4 : st.count; }
                if (remaining_ <= 0) { pressing_ = 0; advance(); continue; }
                remaining_--;
                return;
            case StepKind::Hold: held_ |= st.buttons; advance(); continue;
            case StepKind::Release: held_ = st.buttons == ~0ull ? 0 : (held_ & ~st.buttons); pressing_ = st.buttons == ~0ull ? 0 : (pressing_ & ~st.buttons); advance(); continue;
            case StepKind::Stick: {
                const int16_t x = to_stick(st.x), y = to_stick(st.y);
                if (st.count <= 0) { stick_[st.which][0] = x; stick_[st.which][1] = y; advance(); continue; }
                if (first) { remaining_ = st.count; stick_override_ = true; stick_tmp_which_ = st.which; stick_tmp_[0] = x; stick_tmp_[1] = y; }
                if (remaining_ <= 0) { stick_override_ = false; advance(); continue; }
                remaining_--;
                return;
            }
            case StepKind::Mouse:
                if (first) remaining_ = st.count < 0 ? 1 : st.count;
                if (remaining_ <= 0) { advance(); continue; }
                mouse_x_ = static_cast<float>(st.x);
                mouse_y_ = static_cast<float>(st.y);
                remaining_--;
                return;
            case StepKind::WaitFrame: r = wait_cond(k >= static_cast<uint64_t>(st.count), "the frame"); break;
            case StepKind::Rate:
                set_clock_rate(st.x);
                advance();
                continue;
            case StepKind::WaitLog: {
                const uint64_t now_ms = GetTickCount64();
                if (first) start_ms_ = now_ms;
                if (log_watch_seen(static_cast<int>(st.count))) { advance(); continue; }
                if (static_cast<double>(now_ms - start_ms_) >= st.x * 1000.0) { fail(st, "timeout after " + std::to_string(static_cast<int64_t>(st.x)) + " s waiting for a log line matching " + st.text); return; }
                return;
            }
            case StepKind::WaitMs: {
                const uint64_t now_ms = GetTickCount64();
                if (first) start_ms_ = now_ms;
                if (static_cast<double>(now_ms - start_ms_) >= st.x) { advance(); continue; }
                return;
            }
            case StepKind::Poke: {
                std::string why;
                if (!detail::poke_memory(st.text, &why)) { fail(st, "@poke: " + why); return; }
                advance();
                continue;
            }
            case StepKind::Peek: {
                std::string why;
                const size_t sp = st.text.find(' ');
                if (sp == std::string::npos || !detail::peek_memory(st.text.substr(0, sp), st.text.substr(sp + 1), &why)) { fail(st, "@peek: " + why); return; }
                advance();
                continue;
            }
            case StepKind::Game: {
                std::string why;
                StepRun run{st, c, k, first, s, valid, timeout_of(), &start_frame_, &pressing_, &why};
                switch (st.game->run(run)) {
                    case StepStatus::Done: advance(); continue;
                    case StepStatus::Wait: return;
                    case StepStatus::Fail: fail(st, why.empty() ? st.game->name : why); return;
                }
                return;
            }
            case StepKind::PressUntil: {
                std::string d;
                if (valid && eval_assert(st, s, valid, &d)) { pressing_ = 0; advance(); continue; }
                const int64_t to = timeout_of();
                if (to > 0 && static_cast<int64_t>(k - start_frame_) >= to) {
                    pressing_ = 0;
                    fail(st, "timeout after " + std::to_string(to) + " frames waiting for the @mashuntil condition (" + d + ")");
                    return;
                }
                pressing_ = ((k - start_frame_) % static_cast<uint64_t>(st.count)) < 4 ? st.buttons : 0;
                return;
            }
            case StepKind::Shot: {
                std::string why;
                if (first && !shot_request(st.text, &why)) {
                    if (require_shots()) { fail(st, "@shot " + st.text + ": " + why); return; }
                    advance();
                    continue;
                }
                const ShotStatus r2 = shot_poll(&why);
                if (r2 == ShotStatus::Pending) return;
                if (r2 == ShotStatus::Failed && require_shots()) { fail(st, "@shot " + st.text + ": " + why); return; }
                advance();
                continue;
            }
            case StepKind::Hash: add_checkpoint(st.text.empty() ? "hash" + std::to_string(idx_) : st.text); advance(); continue;
            case StepKind::Log:
                NXLOG_I("script", "%s", st.text.c_str());
                if (ft::on() && st.text.compare(0, 6, "SCENE ") == 0) ft::mark(st.text.c_str());   // S34: scene windows in the frame trace
                if (st.text.compare(0, 12, "SCENE start ") == 0) prof_sampler_label(st.text.c_str() + 12);   // and in the sampling profiler
                else if (st.text.compare(0, 10, "SCENE end ") == 0) prof_sampler_label("");
                advance();
                continue;
            case StepKind::Assert: {
                std::string d;
                if (!eval_assert(st, s, valid, &d)) { fail(st, "assertion failed: " + d); return; }
                advance();
                continue;
            }
            case StepKind::Timeout: default_timeout_ = st.count; advance(); continue;
            case StepKind::Pass: done_ = true; end_run(0, "pass", "@pass at " + script_.path + ":" + std::to_string(st.line)); return;
            case StepKind::Fail: fail(st, st.text.empty() ? "@fail" : st.text); return;
            case StepKind::Key:
                if (first) {
                    if (!key_event(st.text, true)) { fail(st, "unknown input '" + st.text + "'"); return; }
                    remaining_ = st.count < 0 ? 4 : st.count;
                }
                if (remaining_ <= 0) {
                    if (!key_event(st.text, false)) { fail(st, "unknown input '" + st.text + "'"); return; }
                    advance();
                    continue;
                }
                remaining_--;
                return;
            case StepKind::KeyDown:
                if (!key_event(st.text, true)) { fail(st, "unknown input '" + st.text + "'"); return; }
                advance();
                continue;
            case StepKind::KeyUp:
                if (!key_event(st.text, false)) { fail(st, "unknown input '" + st.text + "'"); return; }
                advance();
                continue;
        }
        if (r == 1) continue;
        return;
    }
    NXLOG_E("tt", "script runner: more than 100000 steps without consuming a frame (a loop of non-blocking steps?)");
    done_ = true;
    end_run(6, "error", "runner guard tripped");
}

}  // namespace nxrt::tt::detail
