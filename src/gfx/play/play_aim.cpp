// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode raw mouse aim, see play_aim.h.
#include "play_aim.h"

#include "play_display.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nxrt::play {

bool guest_read(uint64_t addr, void* out, size_t n) {
    if (addr < 0x10000) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(addr), out, n, &got) && got == n;
}

// A direct store (after a read that proves the page is mapped, under an exception guard). WriteProcessMemory looks the region up first (NtQueryVirtualMemory) and for an address in the guest heap that
// lookup takes tens of milliseconds (measured 56 ms per call against the player object, 3 to 10 us elsewhere), once per frame: the game ran at 8 frames
// per second. ReadProcessMemory has no such lookup (2 us).
bool guest_write(uint64_t addr, const void* in, size_t n) {
    if (addr < 0x10000) return false;
    uint8_t probe[64];
    if (n > sizeof probe || !guest_read(addr, probe, n)) return false;      // unmapped or reserved: a read says so in 2 us (a fault here would reach the runtime's own handler first)
    __try {
        std::memcpy(reinterpret_cast<void*>(addr), in, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

namespace {
template <class T>
bool rd(uint64_t a, T* v) { return guest_read(a, v, sizeof(T)); }
uint64_t rd64(uint64_t a) { uint64_t v = 0; rd(a, &v); return v; }
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
}  // namespace

const char* aim_block_name(AimBlock b) {
    switch (b) {
        case AimBlock::None: return "aim on";
        case AimBlock::NoPlayer: return "no player";
        case AimBlock::NotFirstPerson: return "not first person";
        case AimBlock::Morphed: return "morph ball";
        case AimBlock::LockedOn: return "locked on";
        case AimBlock::Cinematic: return "cinematic";
        case AimBlock::Dead: return "dead";
        case AimBlock::BadState: return "unreadable state";
    }
    return "?";
}

double yaw_of_transform(const float xf[12]) { return std::atan2(static_cast<double>(xf[2]), static_cast<double>(xf[0])); }

void rotate_transform_yaw(const float in[12], double dyaw, float out[12]) {
    const double c = std::cos(dyaw), s = std::sin(dyaw);
    // The game's world is Y up (forward is +Z at yaw 0: forward = (sin yaw, 0, cos yaw), + = turn left): the heading turns about Y, left-multiplied onto the rotation (rows 0 and 2 mix, row 1 stays).
    for (int col = 0; col < 3; ++col) {
        const double r0 = in[col], r2 = in[8 + col];
        out[col] = static_cast<float>(c * r0 + s * r2);
        out[4 + col] = in[4 + col];
        out[8 + col] = static_cast<float>(-s * r0 + c * r2);
    }
    out[3] = in[3];
    out[7] = in[7];
    out[11] = in[11];
}

AimDriver::AimDriver(uint64_t game_base, const AimLayout& l) : base_(game_base), l_(l) {}

AimBlock AimDriver::gate(uint64_t player, uint64_t mgr) const {
    if (!player || !mgr) return AimBlock::NoPlayer;
    uint32_t cam = 0, morph = 0, orbit = 0;
    if (!rd(player + l_.camera_state, &cam) || !rd(player + l_.morph_state, &morph) || !rd(player + l_.orbit_state, &orbit)) return AimBlock::BadState;
    if (cam != 0) return AimBlock::NotFirstPerson;
    if (morph != 0) return AimBlock::Morphed;
    if (orbit != 0 && !free_lock_active(player)) return AimBlock::LockedOn;
    const uint64_t logic = rd64(mgr + l_.mgr_logic);
    const uint64_t cm = logic ? rd64(logic + l_.logic_camera_mgr) : 0;
    if (!cm) return AimBlock::BadState;
    uint32_t cur = 0, cine = 0, cine2 = 0, fpid = 0;
    const uint64_t fp = rd64(cm + l_.cam_fp_object);
    if (!rd(cm + l_.cam_current_id, &cur) || !rd(cm + l_.cam_cinematic, &cine) || !rd(cm + l_.cam_cinematic2, &cine2) || !fp ||
        !rd(fp + l_.fp_object_id, &fpid))
        return AimBlock::BadState;
    if (cine != 0 || cine2 != 0) return AimBlock::Cinematic;
    if (cur != fpid) return AimBlock::NotFirstPerson;
    const GameHints h = hints(player);
    if (h.valid && !h.alive) return AimBlock::Dead;
    return AimBlock::None;
}

bool AimDriver::free_lock_active(uint64_t player) const {
    if (!player || !free_lock_setting()) return false;
    uint32_t orbit = 0;
    uint8_t reached = 0;
    if (!rd(player + l_.orbit_state, &orbit) || orbit != 1) return false;
    if (!rd(player + l_.orbit_reached, &reached) || reached == 0) return false;      // the game's own lock-on is still centring the target
    return hints(0).visor != 2;      // the scan visor keeps the classic lock
}

void AimDriver::set_free_lock_view(double margin, double aspect, double fov_offset_deg) {
    margin_.store(std::max(0.0, std::min(0.4, margin)), std::memory_order_relaxed);
    aspect_.store(aspect > 0.2 && aspect < 10.0 ? aspect : 16.0 / 9.0, std::memory_order_relaxed);
    fov_offset_.store(fov_offset_deg, std::memory_order_relaxed);
}

bool AimDriver::body_transform(uint64_t player, uint64_t* xf_addr, float xf[12]) const {
    const uint64_t xfp = rd64(rd64(rd64(player + l_.player_goc) + l_.goc_inner) + l_.inner_xf);
    if (!xfp || !guest_read(xfp, xf, 12 * sizeof(float))) return false;
    if (xf_addr) *xf_addr = xfp;
    return true;
}

bool AimDriver::view_pitch(uint64_t player, double* pitch) const {
    float v = 0;
    if (!rd(player + l_.free_look_pitch, &v) || !std::isfinite(v)) return false;
    *pitch = v;
    return true;
}

double AimDriver::stick_turn_rate() const {
    const uint64_t gp = l_.gp_tweak_player_abs ? l_.gp_tweak_player_abs : abs_(l_.gp_tweak_player);
    const uint64_t tw = rd64(gp);
    float a = 0, b = 0;
    if (!tw || !rd(tw + 304, &a) || !rd(tw + 152, &b) || !std::isfinite(a) || !std::isfinite(b)) return 0;
    return static_cast<double>(a) * b;
}

namespace {
double wrap_pi(double a) {
    while (a > 3.14159265358979323846) a -= 2.0 * 3.14159265358979323846;
    while (a < -3.14159265358979323846) a += 2.0 * 3.14159265358979323846;
    return a;
}
}  // namespace

bool keep_in_view(const double d[3], double* yaw, double* pitch, double tan_h, double tan_v, double pitch_limit) {
    const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 0.25) || !std::isfinite(len)) return false;
    const double max_x = std::atan(std::max(1e-3, tan_h)), max_y = std::atan(std::max(1e-3, tan_v));
    const double y0 = *yaw, p0 = *pitch;
    double y = y0, p = p0;
    for (int it = 0; it < 96; ++it) {      // coordinate descent: slow near the poles (about 45 rounds at worst)
        const double sy = std::sin(y), cy = std::cos(y), sp = std::sin(p), cp = std::cos(p);
        // the target in the camera's axes: z ahead, x to the left, y up (forward = (sin y cos p, sin p, cos y cos p), left = (cos y, 0, -sin y))
        const double z = d[0] * sy * cp + d[1] * sp + d[2] * cy * cp;
        const double xl = d[0] * cy - d[2] * sy;
        const double yu = -d[0] * sy * sp + d[1] * cp - d[2] * cy * sp;
        if (z <= 1e-3 * len) {      // beside or behind: face it first
            y = std::atan2(d[0], d[2]);
            p = std::max(-pitch_limit, std::min(pitch_limit, std::atan2(d[1], std::hypot(d[0], d[2]))));
            continue;
        }
        const double ax = std::atan2(xl, z), ay = std::atan2(yu, z);
        bool moved = false;
        if (std::fabs(ax) > max_x + 1e-7) {
            y += std::copysign(std::fabs(ax) - max_x, ax);
            moved = true;
        }
        if (std::fabs(ay) > max_y + 1e-7) {
            const double np = std::max(-pitch_limit, std::min(pitch_limit, p + std::copysign(std::fabs(ay) - max_y, ay)));
            if (np != p) moved = true;
            p = np;
        }
        if (!moved) break;
    }
    y = y0 + wrap_pi(y - y0);
    if (std::fabs(y - y0) < 1e-9 && std::fabs(p - p0) < 1e-9) return false;
    *yaw = y;
    *pitch = p;
    return true;
}

bool AimDriver::keep_target_in_view(uint64_t player, uint64_t mgr, double* yaw, double* pitch) const {
    if (!player || !mgr) return false;
    const uint64_t logic = rd64(mgr + l_.mgr_logic);
    const uint64_t cm = logic ? rd64(logic + l_.logic_camera_mgr) : 0;
    const uint64_t fp = cm ? rd64(cm + l_.cam_fp_object) : 0;
    if (!fp) return false;
    float eye[3], tgt[3], vfov_deg = 0, cam_aspect = 0;
    if (!rd(fp + l_.fp_base_xf + 12, &eye[0]) || !rd(fp + l_.fp_base_xf + 28, &eye[1]) || !rd(fp + l_.fp_base_xf + 44, &eye[2])) return false;
    if (!guest_read(player + l_.orbit_target_pos, tgt, sizeof tgt) || !rd(fp + l_.fp_vertical_fov, &vfov_deg)) return false;
    rd(fp + l_.fp_aspect, &cam_aspect);
    for (float f : {eye[0], eye[1], eye[2], tgt[0], tgt[1], tgt[2]})
        if (!std::isfinite(f)) return false;
    if (!(vfov_deg > 5.0f && vfov_deg < 150.0f)) vfov_deg = 55.0f;      // not an angle in degrees: the game's usual first person value
    const double aspect = (cam_aspect > 0.5f && cam_aspect < 10.0f) ? cam_aspect : aspect_.load(std::memory_order_relaxed);
    const double v = std::max(10.0, std::min(170.0, vfov_deg + fov_offset_.load(std::memory_order_relaxed))) * kDegToRad;
    const double limit_frac = std::max(0.05, 1.0 - 2.0 * margin_.load(std::memory_order_relaxed));
    const double tan_v = std::tan(v * 0.5) * limit_frac;
    const double tan_h = std::tan(v * 0.5) * aspect * limit_frac;
    const double d[3] = {static_cast<double>(tgt[0]) - eye[0], static_cast<double>(tgt[1]) - eye[1], static_cast<double>(tgt[2]) - eye[2]};
    return keep_in_view(d, yaw, pitch, tan_h, tan_v, limit_ > 0 ? limit_ : 1.5184);
}

GameHints AimDriver::hints(uint64_t player) const {
    GameHints h;
    const uint64_t gp = l_.gp_game_state_abs ? l_.gp_game_state_abs : abs_(l_.gp_game_state);
    const uint64_t gs = rd64(gp);
    const uint64_t ps = gs ? rd64(gs + 0x20) : 0;
    if (!ps) return h;
    uint8_t flags = 0;
    uint32_t beam = 0, visor = 0;
    if (!rd(ps + l_.ps_alive_byte, &flags) || !rd(ps + l_.ps_current_beam, &beam) || !rd(ps + l_.ps_visor, &visor)) return h;
    h.valid = true;
    h.alive = (flags & 1) != 0;
    h.beam = beam < 4 ? static_cast<int>(beam) : -1;
    h.visor = visor < 4 ? static_cast<int>(visor) : -1;
    for (int i = 0; i < 4; ++i) {
        uint32_t cap = 0;
        if (rd(ps + l_.ps_items + 8u * i, &cap) && static_cast<int32_t>(cap) > 0) h.owned_beams |= 1u << i;
    }
    if (player) {
        uint32_t morph = 0;
        if (rd(player + l_.morph_state, &morph)) h.morphed = morph != 0;
    }
    return h;
}

double AimDriver::pitch_limit(const AimParams& p) const {
    if (p.pitch_limit_rad > 0) return p.pitch_limit_rad;
    const uint64_t gp = l_.gp_tweak_player_abs ? l_.gp_tweak_player_abs : abs_(l_.gp_tweak_player);
    const uint64_t tw = rd64(gp);
    float v = 0;
    if (tw && rd(tw + l_.tweak_max_pitch, &v) && std::isfinite(v) && v > 0.01f) {
        const double a = v > 3.5f ? v * kDegToRad : v;     // the tweak is in degrees on the console (50.0); a radian value is below pi
        return std::min(a, 1.5533);                        // never closer than ~89 degrees to the pole
    }
    return 1.5184;                                         // 87 degrees when the tweak cannot be read
}

bool AimDriver::apply(uint64_t player, uint64_t mgr, double dx, double dy, const AimParams& p, const std::function<void()>& original) {
    block_ = gate(player, mgr);
    if (block_ != AimBlock::None) {
        pitch_valid_ = false;     // the game owns the view now; adopt its pitch when aim comes back
        pad_look_prev_ = false;
        if (original) original();
        return false;
    }
    float cur_pitch = 0;
    rd(player + l_.free_look_pitch, &cur_pitch);
    if (!pitch_valid_ || !std::isfinite(pitch_)) {
        pitch_ = std::isfinite(cur_pitch) ? cur_pitch : 0.0;
        pitch_valid_ = true;
    } else if ((p.pad_look || pad_look_prev_) && std::isfinite(cur_pitch)) {
        pitch_ = cur_pitch;       // the pad moved the view since the last call (and the game's code eased it there): carry on from it
    }
    pad_look_prev_ = p.pad_look;
    limit_ = pitch_limit(p);
    double dyaw = dx * p.yaw_rad * (p.invert_x ? 1.0 : -1.0);
    pitch_ = std::max(-limit_, std::min(limit_, pitch_ + dy * p.pitch_rad * (p.invert_y ? 1.0 : -1.0)));
    uint64_t xfp = 0;
    float xf[12];
    const bool have_xf = (dyaw != 0.0 || free_lock_active(player)) && body_transform(player, &xfp, xf);
    if (have_xf && free_lock_active(player)) {      // free lock-on: the mouse moves the view, but the target stays inside the margin
        double yaw = yaw_of_transform(xf) + dyaw, pitch = pitch_;
        if (keep_target_in_view(player, mgr, &yaw, &pitch)) {
            dyaw = wrap_pi(yaw - yaw_of_transform(xf));
            pitch_ = pitch;
        }
    }
    if (have_xf && dyaw != 0.0) {
        float nx[12];
        rotate_transform_yaw(xf, dyaw, nx);
        if (set_xf_) set_xf_(set_xf_user_, player, nx);
        else guest_write(xfp, nx, sizeof nx);
    }
    const float pre = static_cast<float>(pitch_);
    guest_write(player + l_.free_look_pitch, &pre, sizeof pre);       // what the mouse made of it, before the game reads the stick
    guest_write(player + l_.free_look_target, &pre, sizeof pre);
    if (original) original();
    if (!p.pad_look) {
        const float out = static_cast<float>(pitch_);
        guest_write(player + l_.free_look_pitch, &out, sizeof out);
        guest_write(player + l_.free_look_target, &out, sizeof out);  // else Think eases the view back toward the stick's (zero) target
    }
    ++applied_;
    return true;
}

}  // namespace nxrt::play
