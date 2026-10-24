// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: free lock-on and the two camera feel options (S32). Entry hooks on a handful of CPlayerMP1 / CPlayerCameraBobMP1 functions:
//
//   [aim] lock_mode = free   The game's lock-on (hold the lock button) normally turns Samus and the view to face the target. With free it keeps
//                            the lock, the homing shots and the strafing, but nothing turns the view: the mouse (and the right stick) keep it.
//                            Hooks: UpdateOrbitOrientation (skipped: it turns the body to face the target), ComputeFreeLook (the pitch target
//                            is the stick's, not the direction to the target) and TurnInput as called by ComputeMovement (the right stick turns
//                            Samus), and CFirstPersonCameraMP1::UpdateTransform (the camera turns its own view to the target while locked).
//                            ValidateCurrentOrbitTargetId (its "view turned too far" break is turned into "still locked"). The aim driver (play_aim.h) keeps the mouse aim open.
//   [camera] idle_sway = 0   No slow wandering of the view while Samus stands still (CPlayerCameraBobMP1::UpdateViewWander).
//   [camera] walk_bob = 0    No up-and-down sway of the view (and the gun) while walking (CPlayerCameraBobMP1::CalculateMovingTranslation).
//
// The hook sites must be in recomp games/mpr/main.sites (entryhook 0x7100c77854 0x7100c71638 0x7100cefa54 0x7100c765d0 0x7100d710ac 0x7100d0ada0 0x7100d0b17c); without
// them the options do nothing and everything else works.
#pragma once
#include <cstdint>

#include "play_aim.h"
#include "play_types.h"

namespace nxrt::play {

void lockcam_install(uint64_t game_base, AimDriver* aim);
void lockcam_remove();
void lockcam_set(const Settings& s);        // any thread; takes effect at the next call of the hooked functions

struct LockCamStats {
    uint64_t orientation_skipped = 0, freelook_calls = 0, turn_calls = 0, sway_skipped = 0, bob_zeroed = 0;
};
LockCamStats lockcam_stats();

}  // namespace nxrt::play
