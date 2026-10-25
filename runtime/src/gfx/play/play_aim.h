// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: raw mouse aim. Turns accumulated mouse counts into a yaw on the player's transform and a pitch in CPlayerMP1's free-look
// angle, and decides when that is allowed. Works on guest memory through plain reads and writes, so the tests run it against a fake player
// built on the host heap (guest pointers are host pointers). Offsets are Metroid Prime Remastered 1.0.0 (docs/mpr/structs.json and the
// disassembly of the functions named below); every one is a field of AimLayout.
//
// How it hooks in (play.cpp): an entry hook on CPlayerMP1::ProcessInput (x0 player, x2 CStateManager). AimDriver::apply() does, in order:
//   1. read the gate (first person, not morphed, not locked on, not in a cinematic, alive); if closed, forget the pitch and just run the game;
//   2. yaw: rotate the player's transform about Y (the world is Y up) by dx * yaw_rad and hand it to CActorMP1::SetTransform (the game's own function, so the
//      entity's change notification runs);
//   3. pitch: our own accumulated angle (adopted from the game's value when aim becomes allowed), clamped to the tweak's free-look limit;
//   4. run the game's ProcessInput (it reads the new transform; its free-look code may pull the pitch back toward level);
//   5. write the pitch into CPlayerMP1+0x5cc again, so the camera and the gun use the angle the mouse chose.
// Sign conventions (the same as the GameCube port's): mouse right = yaw decreases (turn right), mouse up (dy < 0) = pitch increases (look up).
#pragma once
#include <atomic>
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
    uint32_t orbit_state = 0x4e0;            // EPlayerOrbitState, 0 = none, 1 = orbit object (the normal lock-on), 2 point, 3 carcass, 4 forced, 5 grapple (SetOrbitState 0xc77e5c)
    uint32_t orbit_reached = 0x550;          // byte: set by ValidateCurrentOrbitTargetId once the view has centred the target (within 1 degree); from then on the lock breaks when the view
                                             // turns away from the target by more than the tweak's angle. Also read by UpdateOrbitOrientation and by the aiming cursor, which only
                                             // follows the locked target (and swings the arm cannon to it) while it is set. Cleared when the lock is let go. Free lock-on starts when it is set.
    uint32_t orbit_target_pos = 0x4f0;       // 3 floats: world position of the locked target (CPlayerMP1::UpdateOrbitInput refreshes it every frame while locked on an object)
    // first-person camera object (CFirstPersonCameraMP1)
    uint32_t fp_base_xf = 660;               // 3x4 floats: the camera's own transform before the camera bob is applied (UpdateTransform keeps it; translation = the eye)
    uint32_t fp_vertical_fov = 0x25c;        // CGameCameraMP1::CurrentFOV_acc: the vertical field of view in degrees (55 for the first person camera)
    uint32_t fp_aspect = 0x264;              // CGameCamera: the picture's width / height the camera projects with
    uint32_t free_look_pitch = 0x5cc;        // float radians, + = up
    uint32_t look_filter = 0x1628;           // pointer to the CIIRFilter (type 0, response 4: y = 0.1 x + 0.4 y[-2] + 0.5 y[-1]) that smooths the pitch target (CPlayerMP1 constructor 0xc678bc)
    uint32_t free_look_target = 0x5d0;       // float radians: the pitch CPlayerMP1::UpdateFreeLook (every Think) eases free_look_pitch toward; the input code sets it from the stick
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
    // A pad is looking up or down this frame (the right stick's vertical axis, the gyro turning, or the game's gyro camera / Pointer scheme engaged): the game's
    // own pitch code owns the pitch (it eases it toward the stick, or follows the gyro or the pad's orientation), and this aim adopts what the game made of it
    // instead of writing its own angle over it. The mouse still adds to it.
    bool pad_look = false;
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

    // Free lock-on ([aim] lock_mode = free, S32): the lock stays, but the mouse keeps the view. Set from the settings at the frame boundary.
    //   free_lock_active(): the setting is on, the game is in the ordinary lock-on (orbit state 1: an object, not a grapple, a forced orbit, a point
    //   or a carcass), the visor is not the scan visor (scanning keeps the classic lock) and the game's own lock-on has already centred the target
    //   (the "reached" flag). Before that everything is the game's own, so a new lock starts exactly like the classic one. While it is active the
    //   aim gate stays open, the hooks in play_lockcam.cpp stop the game from turning the body, the pitch and the camera to the target and from breaking the
    //   lock when the view turns away, and the view is held so that the target stays inside the margin (keep_target_in_view).
    void set_free_lock(bool on) { free_lock_.store(on, std::memory_order_relaxed); }
    bool free_lock_setting() const { return free_lock_.load(std::memory_order_relaxed); }
    bool free_lock_active(uint64_t player) const;
    // margin: fraction of the screen kept free along every edge; aspect: width / height of the 3D picture; fov_offset_deg: [display] fov_offset.
    void set_free_lock_view(double margin, double aspect, double fov_offset_deg);

    // What the view must do so that the locked target stays inside the margin. yaw (body heading, radians, + = turn left) and pitch (radians, + = up) are the
    // values the view is about to have; they are changed in place and true is returned when they had to be. False when the geometry cannot be read.
    bool keep_target_in_view(uint64_t player, uint64_t mgr, double* yaw, double* pitch) const;
    // The player's body transform: where it lives and what it is (false when unreadable).
    bool body_transform(uint64_t player, uint64_t* xf_addr, float xf[12]) const;
    // The view pitch the game holds now (CPlayerMP1+0x5cc); false when unreadable.
    bool view_pitch(uint64_t player, double* pitch) const;
    // The yaw speed (radians per second) of a full right-stick turn, from the game's player tweak (what ComputeMovement turns the body with); 0 when unreadable.
    double stick_turn_rate() const;
    // Another part of the code changed the pitch: keep the aim in step with it.
    void set_pitch(double pitch) { pitch_ = pitch; pitch_valid_ = true; }

    const AimLayout& layout() const { return l_; }

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
    bool pad_look_prev_ = false;           // a pad was looking up or down at the previous call (adopt the game's pitch once more when it lets go)
    AimBlock block_ = AimBlock::None;
    uint64_t applied_ = 0;
    std::atomic<bool> free_lock_{false};
    std::atomic<double> margin_{0.15}, aspect_{16.0 / 9.0}, fov_offset_{0.0};
};

// Turns the view (yaw about Y, + = turn left; pitch + = up; radians) just far enough that the direction d (target minus eye, world axes, Y up) lies inside the
// box |x| <= tan_h * z and |y| <= tan_v * z of the camera space (tan_h, tan_v: tangent of the half field of view times the allowed fraction of the screen).
// Pitch stays within +-pitch_limit. Targets at the eye or behind the view are turned to first. Returns true when yaw or pitch changed. Public for the tests.
bool keep_in_view(const double d[3], double* yaw, double* pitch, double tan_h, double tan_v, double pitch_limit);

// The transform maths (row-major 3x4: three rows of {r0, r1, r2, translation}); public for the tests.
double yaw_of_transform(const float xf[12]);                               // atan2(xf[2], xf[0]): heading about Y (the world is Y up, forward is +Z at 0, + = turn left)
void rotate_transform_yaw(const float in[12], double dyaw, float out[12]); // Ry(dyaw) * rotation, translation kept

// Guest memory access used by the driver and the probes (ReadProcessMemory on this process: a bad pointer gives false, never a fault).
// Adds d to every sample of the game's CIIRFilter (the three history arrays: counts at +8, +76 and +120, samples from +12, +80 and +124). A filter with a
// DC gain of 1 (this one has) then carries on exactly as if its whole past had been d higher: moving the pitch target by d from outside (the mouse) does
// not get pulled back by the filter's memory. False when the memory does not look like that filter (a count over what its array holds: 16, 10 and 10 floats,
// a bad address).
bool shift_look_filter(uint64_t filter, float d);
bool guest_read(uint64_t addr, void* out, size_t n);
bool guest_write(uint64_t addr, const void* in, size_t n);

}  // namespace nxrt::play
