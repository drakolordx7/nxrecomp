// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode raw mouse aim, see play_aim.h.
#include "play_aim.h"

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
    if (orbit != 0) return AimBlock::LockedOn;
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
        if (original) original();
        return false;
    }
    float cur_pitch = 0;
    rd(player + l_.free_look_pitch, &cur_pitch);
    if (!pitch_valid_ || !std::isfinite(pitch_)) {
        pitch_ = std::isfinite(cur_pitch) ? cur_pitch : 0.0;
        pitch_valid_ = true;
    }
    limit_ = pitch_limit(p);
    const double dyaw = dx * p.yaw_rad * (p.invert_x ? 1.0 : -1.0);
    pitch_ = std::max(-limit_, std::min(limit_, pitch_ + dy * p.pitch_rad * (p.invert_y ? 1.0 : -1.0)));
    if (dyaw != 0.0) {
        const uint64_t xfp = rd64(rd64(rd64(player + l_.player_goc) + l_.goc_inner) + l_.inner_xf);
        float xf[12];
        if (xfp && guest_read(xfp, xf, sizeof xf)) {
            float nx[12];
            rotate_transform_yaw(xf, dyaw, nx);
            if (set_xf_) set_xf_(set_xf_user_, player, nx);
            else guest_write(xfp, nx, sizeof nx);
        }
    }
    if (original) original();
    const float out = static_cast<float>(pitch_);
    guest_write(player + l_.free_look_pitch, &out, sizeof out);
    guest_write(player + l_.free_look_target, &out, sizeof out);      // else Think eases the view back toward the stick's (zero) target
    ++applied_;
    return true;
}

}  // namespace nxrt::play
