// SPDX-License-Identifier: GPL-3.0-or-later
// The scripted-run executor (S5): runs the steps of a parsed script (tt_script.h) one frame boundary at a time.
// Each call to Runner::frame is one frame; a step that blocks consumes the frame, a step that does not lets the next one run in the
// same call. Failure and pass call tt::end_run (exit code 1 / 0).
#include <windows.h>

#include <cmath>
#include <cstring>

#include "nxrt/log.h"
#include "tt_internal.h"

namespace nxrt::tt::detail {

namespace {
int16_t to_stick(double v) {
    const double c = v < -1 ? -1 : v > 1 ? 1 : v;
    return static_cast<int16_t>(std::lround(c * 32767.0));
}
}  // namespace

bool Runner::load(const std::string& script_file, const RegionTable* regions, std::string* err) {
    script_ = Script();
    idx_ = 0; started_ = false; done_ = false; warped_ = false; remaining_ = 0;
    default_timeout_ = 3600;
    held_ = pressing_ = 0;
    std::memset(stick_, 0, sizeof stick_);
    stick_override_ = false;
    mouse_x_ = mouse_y_ = 0;
    fail_line_ = 0;
    has_keys_ = false;
    const bool ok = parse_script_file(script_file, regions, &script_, err);
    for (const Step& st : script_.steps) {
        if (st.kind == StepKind::Key || st.kind == StepKind::KeyDown || st.kind == StepKind::KeyUp) has_keys_ = true;
    }
    return ok;
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
    auto num = [&](double* v) -> bool {
        switch (st.field) {
            case AssertField::Frame: *v = static_cast<double>(main_frame_count() ? main_frame_count() - 1 : 0); return true;
            case AssertField::Igt: *v = s.igt; return valid && s.have_igt;
            case AssertField::Health: *v = s.hp; return valid && s.have_hp;
            case AssertField::PosX: *v = s.pos[0]; return valid && s.have_player;
            case AssertField::PosY: *v = s.pos[1]; return valid && s.have_player;
            case AssertField::PosZ: *v = s.pos[2]; return valid && s.have_player;
            case AssertField::VelX: *v = s.vel[0]; return valid && s.have_player;
            case AssertField::VelY: *v = s.vel[1]; return valid && s.have_player;
            case AssertField::VelZ: *v = s.vel[2]; return valid && s.have_player;
            case AssertField::Speed: *v = std::sqrt(double(s.vel[0]) * s.vel[0] + double(s.vel[1]) * s.vel[1] + double(s.vel[2]) * s.vel[2]); return valid && s.have_player;
            case AssertField::State: *v = s.move_state; return valid && s.have_player;
            default: return false;
        }
    };
    if (st.field == AssertField::Area || st.field == AssertField::World) {
        const uint8_t* have = st.field == AssertField::Area ? s.area : s.world;
        if (!valid || !s.have_world) { *detail = "the id is not readable (no game state)"; return false; }
        const bool eq = std::memcmp(have, st.id, 16) == 0;
        *detail = "have " + tt_guid_hex(have) + ", want " + tt_guid_hex(st.id);
        return st.op == AssertOp::Eq ? eq : !eq;
    }
    double v = 0;
    if (!num(&v)) { *detail = "the value is not readable (no game state)"; return false; }
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

void Runner::frame(uint64_t k, const GameState& s, bool valid, ::Ctx* c) {
    if (done_) return;
    mouse_x_ = mouse_y_ = 0;       // the impulse lasts for the frame of the step that sets it
    for (int guard = 0; guard < 100000; guard++) {
        if (done_) return;
        if (idx_ >= script_.steps.size()) {
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
            case StepKind::WaitArea: r = wait_cond(valid && s.have_world && std::memcmp(s.area, st.id, 16) == 0, ("area " + (st.name1.empty() ? tt_guid_to_text(st.id) : st.name1)).c_str()); break;
            case StepKind::WaitWorld: r = wait_cond(valid && s.have_world && std::memcmp(s.world, st.id, 16) == 0, ("world " + (st.name1.empty() ? tt_guid_to_text(st.id) : st.name1)).c_str()); break;
            case StepKind::WaitPos: {
                bool met = false;
                if (valid && s.have_player) {
                    const double dx = s.pos[0] - st.x, dy = s.pos[1] - st.y, dz = s.pos[2] - st.z;
                    met = std::sqrt(dx * dx + dy * dy + dz * dz) <= st.r;
                }
                char b[96];
                std::snprintf(b, sizeof b, "position (%.2f, %.2f, %.2f) within %.2f", st.x, st.y, st.z, st.r);
                r = wait_cond(met, b);
                break;
            }
            case StepKind::WaitIgt: r = wait_cond(valid && s.have_igt && s.igt >= st.x, "the in-game time"); break;
            case StepKind::WaitRun: {
                if (first) start_igt_ = -1;
                if (valid && s.have_igt) {
                    if (start_igt_ < 0) start_igt_ = s.igt;
                    r = wait_cond(s.igt - start_igt_ >= st.x, "the game to run for the given time");
                } else {
                    r = wait_cond(false, "the in-game time (not readable)");
                }
                break;
            }
            case StepKind::Mash: {
                const uint64_t now_ms = GetTickCount64();
                if (first) { start_ms_ = now_ms; next_mash_ms_ = now_ms; remaining_ = 0; }
                const bool met = valid && s.have_world && std::memcmp(st.text == "area" ? s.area : s.world, st.id, 16) == 0;
                if (met) { pressing_ = 0; advance(); continue; }
                const int64_t to_ms = st.timeout < 0 ? 600000 : st.timeout;
                if (to_ms > 0 && static_cast<int64_t>(now_ms - start_ms_) >= to_ms) { pressing_ = 0; fail(st, "timeout waiting for " + st.text + " " + (st.name1.empty() ? tt_guid_to_text(st.id) : st.name1)); return; }
                // The press lasts 250 ms of host time: in deterministic mode a frame is a tiny slice of host time and the front end runs on the host clock.
                if (remaining_ > 0) { if (now_ms >= mash_release_ms_) { remaining_ = 0; pressing_ = 0; } }
                else if (now_ms >= next_mash_ms_) { pressing_ = st.buttons; remaining_ = 1; mash_release_ms_ = now_ms + 250; next_mash_ms_ = now_ms + static_cast<uint64_t>(st.x); }
                return;
            }
            case StepKind::Give:
                if (!detail::give_item(static_cast<int>(st.x), static_cast<int>(st.y))) { fail(st, "@give: the player state is not readable yet"); return; }
                advance();
                continue;
            case StepKind::WaitMs: {
                const uint64_t now_ms = GetTickCount64();
                if (first) start_ms_ = now_ms;
                if (static_cast<double>(now_ms - start_ms_) >= st.x) { advance(); continue; }
                return;
            }
            case StepKind::Warp: {
                if (!warped_) {
                    if (detail::warp(c, st.id2, st.id)) {
                        warped_ = true;
                        start_frame_ = k;
                        NXLOG_I("tt", "warp issued: world %s area %s (frame %llu)", tt_guid_to_text(st.id2).c_str(), tt_guid_to_text(st.id).c_str(), static_cast<unsigned long long>(k));
                        return;                       // the quit is processed by the game over the next frames
                    }
                    const int64_t to = timeout_of();
                    if (to > 0 && static_cast<int64_t>(k - start_frame_) >= to) { fail(st, "cannot warp: no state manager seen (the game is not in a level, or the hooks are not built in)"); return; }
                    return;
                }
                const bool met = valid && s.have_world && std::memcmp(s.area, st.id, 16) == 0 && std::memcmp(s.world, st.id2, 16) == 0;
                if (met) { warped_ = false; advance(); continue; }
                const int64_t to = timeout_of();
                if (to > 0 && static_cast<int64_t>(k - start_frame_) >= to) { fail(st, "timeout waiting for the warp target area to become current"); return; }
                return;
            }
            case StepKind::Shot: {
                std::string why;
                if (!take_shot(st.text, &why) && require_shots()) { fail(st, "@shot " + st.text + ": " + why); return; }
                advance();
                continue;
            }
            case StepKind::Hash: add_checkpoint(st.text.empty() ? "hash" + std::to_string(idx_) : st.text); advance(); continue;
            case StepKind::Log: NXLOG_I("script", "%s", st.text.c_str()); advance(); continue;
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
        return;       // 0 blocked, -1 failed
    }
    NXLOG_E("tt", "script runner: more than 100000 steps without consuming a frame (a loop of non-blocking steps?)");
    done_ = true;
    end_run(6, "error", "runner guard tripped");
}

}  // namespace nxrt::tt::detail
