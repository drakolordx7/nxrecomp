// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: raw mouse aim. Turns accumulated mouse counts into a yaw on the player's transform and a pitch in CPlayerMP1's free-look
// angle, and decides when that is allowed. Works on guest memory through plain reads and writes, so the tests run it against a fake player
// built on the host heap (guest pointers are host pointers). Offsets are Metroid Prime Remastered 1.0.0 (docs/mpr/structs.json and the
// disassembly of the functions named below); every one is a field of AimLayout.
//
// How it hooks in (play.cpp): an entry hook on CPlayerMP1::ProcessInput (x0 player, x2 CStateManager). AimDriver::apply() does, in order:
//   1. read the gate (first person, not morphed, not locked on, not in a cinematic, alive); if closed, forget the pitch and just run the game;
//   2. yaw: rotate the player's transform about Z by dx * yaw_rad and hand it to CActorMP1::SetTransform (the game's own function, so the
//      entity's change notification runs);
//   3. pitch: our own accumulated angle (adopted from the game's value when aim becomes allowed), clamped to the tweak's free-look limit;
//   4. run the game's ProcessInput (it reads the new transform; its free-look code may pull the pitch back toward level);
//   5. write the pitch into CPlayerMP1+0x5cc again, so the camera and the gun use the angle the mouse chose.
// Sign conventions (the same as the GameCube port's): mouse right = yaw decreases (turn right), mouse up (dy < 0) = pitch increases (look up).
#pragma once
#include <cstdint>
#include <functional>

namespace nxrt::play {

struct AimLayout {
    // main-module relative addresses (added to the game base)
    uint64_t gp_game_state = 0x459cc70;      // gpGameState: pointer to CGameStateMP1; +0x20 = CPlayerStateMP1*
    uint64_t gp_tweak_player = 0x4b32880;    // gpTweakPlayer: pointer to CTweakPlayerMP1
    uint64_t fn_set_transform = 0xc4d588;    // CActorMP1::SetTransform(CTransform4f const&)
    // absolute overrides for tests (0 = base + relative)
    uint64_t gp_game_state_abs = 0, gp_tweak_player_abs = 0;
    // CPlayerMP1
    uint32_t player_goc = 0x08, goc_inner = 0x20, inner_xf = 0x40;   // player -> +8 -> +0x20 -> +0x40 = CTransform4f (3x4 floats, translation in column 3)
    uint32_t camera_state = 0x4d0;           // EPlayerCameraState, 0 = first person (SetCameraState 0xc6bd28)
    uint32_t morph_state = 0x4d4;            // 0 = unmorphed (GetMorphBallTransitionState 0xc686fc)
    uint32_t orbit_state = 0x4e0;            // EPlayerOrbitState, 0 = none (SetOrbitState 0xc77e5c)
    uint32_t free_look_pitch = 0x5cc;        // float radians, + = up
    // CStateManager -> CStateManagerGameLogicMP1 -> CCameraManagerMP1
    uint32_t mgr_logic = 0x4e0150, logic_camera_mgr = 0x30;
    uint32_t cam_current_id = 0x00, cam_cinematic = 0x08, cam_cinematic2 = 0x18, cam_fp_object = 0x80, fp_object_id = 0x10;   // IsInFPCamera 0xc8c95c, IsInCinematicCamera 0xc8c974
    // CTweakPlayerMP1
    uint32_t tweak_max_pitch = 0x138;        // MaxFreeLookPitchAngle (radians, or degrees when the value is over 3.5)
    // CPlayerStateMP1
    uint32_t ps_alive_byte = 0x00;           // bit 0 = alive (IsPlayerMotionAllowed 0xc69f14 reads it)
    uint32_t ps_current_beam = 0x08, ps_visor = 0x18, ps_items = 0x30;   // item capacity of item i at ps_items + 8 * i
};

struct AimParams {
    double yaw_rad = 0.000767945, pitch_rad = 0.000767945;   // radians per count
    bool invert_x = false, invert_y = false;
    double pitch_limit_rad = 0;              // > 0 overrides the tweak's limit
};

// Why aim is not applied right now (None = it is).
enum class AimBlock : uint8_t { None = 0, NoPlayer, NotFirstPerson, Morphed, LockedOn, Cinematic, Dead, BadState };
const char* aim_block_name(AimBlock b);

struct GameHints {
    bool valid = false;
    bool alive = false;
    int beam = -1;                // game numbering: 0 power, 1 ice, 2 wave, 3 plasma
    unsigned owned_beams = 0;     // bit i = beam i (game numbering) owned
    int visor = -1;               // 0 combat, 1 x-ray, 2 scan, 3 thermal
    bool morphed = false;
};

class AimDriver {
public:
    using SetTransformFn = void (*)(void* user, uint64_t player, const float xf[12]);
    explicit AimDriver(uint64_t game_base, const AimLayout& l = {});

    // How the new transform reaches the game. Default (not set): the transform memory is written directly, which is what the tests want.
    void set_transform_fn(SetTransformFn fn, void* user) { set_xf_ = fn; set_xf_user_ = user; }

    AimBlock gate(uint64_t player, uint64_t mgr) const;
    GameHints hints(uint64_t player) const;       // player may be 0: only the player-state fields are read then

    // One ProcessInput. original runs the game's own function (may be empty). Returns true when aim was applied (the gate was open).
    // dx, dy: raw mouse counts since the previous call (the caller takes them once per rendered frame).
    bool apply(uint64_t player, uint64_t mgr, double dx, double dy, const AimParams& p, const std::function<void()>& original);

    // State for the log, the settings page and the tests.
    double pitch() const { return pitch_; }
    bool pitch_tracked() const { return pitch_valid_; }
    AimBlock last_block() const { return block_; }
    uint64_t applied_frames() const { return applied_; }
    double last_pitch_limit() const { return limit_; }
    void forget() { pitch_valid_ = false; }

private:
    double pitch_limit(const AimParams& p) const;
    uint64_t abs_(uint64_t rel) const { return base_ + rel; }
    uint64_t base_;
    AimLayout l_;
    SetTransformFn set_xf_ = nullptr;
    void* set_xf_user_ = nullptr;
    double pitch_ = 0, limit_ = 0;
    bool pitch_valid_ = false;
    AimBlock block_ = AimBlock::None;
    uint64_t applied_ = 0;
};

// The transform maths (row-major 3x4: three rows of {r0, r1, r2, translation}); public for the tests.
double yaw_of_transform(const float xf[12]);                               // atan2(-xf[1], xf[5])
void rotate_transform_yaw(const float in[12], double dyaw, float out[12]); // Rz(dyaw) * rotation, translation kept

// Guest memory access used by the driver and the probes (ReadProcessMemory on this process: a bad pointer gives false, never a fault).
bool guest_read(uint64_t addr, void* out, size_t n);
bool guest_write(uint64_t addr, const void* in, size_t n);

}  // namespace nxrt::play
