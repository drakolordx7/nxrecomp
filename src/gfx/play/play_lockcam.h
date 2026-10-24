// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: free lock-on and the decoupled gun (S32). Entry hooks on a handful of CPlayerMP1, CFirstPersonCameraMP1 and CPlayerGunMP1 functions.
//
//   [aim] lock_mode = free   The game's lock-on starts as it always does (the view turns until the target is centred). From then on the lock stays, with its homing
//                            shots and its strafing, but nothing turns the view to the target any more: the mouse (and the right stick) keep it, held by
//                            keep_target_in_view (play_aim.h) so that the target stays on the screen inside [aim] free_lock_margin. The arm cannon and the aiming cursor
//                            keep following the target, which is the game's own behaviour once its lock-on has centred it.
//                            Hooks: UpdateOrbitOrientation (skipped: it turns the body to face the target; the view is clamped there instead),
//                            ComputeFreeLook (the pitch target is the stick's, not the direction to the target), TurnInput as called by ComputeMovement (the right
//                            stick turns Samus), CFirstPersonCameraMP1::UpdateTransform (the camera turns its own view to the target while locked),
//                            ValidateCurrentOrbitTargetId (its "view turned too far" break becomes "still locked"), UpdateAimTarget (the locked object is the aim
//                            target too) and the aiming cursor's update (told that command 75 is held, so the cursor and the arm cannon go to the target).
//                            Walking and strafing in the lock are the game's own dash code, which circles the target by position and does not read the body's heading,
//                            so they need no hook.
//   [camera] gun_drives_view = 0
//                            The camera bob (idle wander, walking bob, landing dip) normally moves the whole view, and the arm cannon with it. With 0 the
//                            camera Think hook in play.cpp leaves the camera on its own transform without the bob, hands the difference to lockcam_set_gun_delta,
//                            and the hook on CPlayerGunMP1::Update moves the gun by that difference: the view stays still and the gun sways on screen.
//
// The hook sites must be in recomp games/mpr/main.sites (entryhook 0x7100c77854 0x7100c71638 0x7100cefa54 0x7100c765d0 0x7100d710ac 0x7100c93950 0x7100c7ad8c 0x7100cb6250 0x71004d7540);
// without them the options do nothing and everything else works.
#pragma once
#include <cstdint>

#include "play_aim.h"
#include "play_types.h"

namespace nxrt::play {

void lockcam_install(uint64_t game_base, AimDriver* aim);
void lockcam_remove();
void lockcam_set(const Settings& s);        // any thread; takes effect at the next call of the hooked functions
void lockcam_frame();                       // once per frame boundary: hands the live picture aspect and field of view offset to the lock-on view limit

// The camera bob decoupling (play.cpp's camera Think hook calls these). gun_drives_view = 0 makes lockcam_gun_decoupled() true.
bool lockcam_gun_decoupled();
// vanilla: the camera transform the game made (with the bob); steady: the same without the bob (3x4 floats, row major). Keeps steady^-1 * vanilla for the gun.
void lockcam_set_gun_delta(const float vanilla[12], const float steady[12]);
void lockcam_clear_gun_delta();

// The transform maths behind it, public for the tests: out = a * b for 3x4 affine transforms (row major, translation in column 3), and out = a * inverse(b).
void xf_mul(const float a[12], const float b[12], float out[12]);
void xf_rel(const float a[12], const float b[12], float out[12]);      // a * b^-1 (b a rigid transform)

struct LockCamStats {
    uint64_t orientation_skipped = 0, freelook_calls = 0, turn_calls = 0, cursor_calls = 0, view_clamped = 0, gun_moved = 0;
};
LockCamStats lockcam_stats();

}  // namespace nxrt::play
