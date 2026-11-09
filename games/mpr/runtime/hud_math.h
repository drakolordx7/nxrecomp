// SPDX-License-Identifier: GPL-3.0-or-later
// The arithmetic of the display options that applies to the game's own GUI cameras (display_hooks.h): what a 16:9 HUD becomes at another
// display aspect. Pure functions, covered by unit tests.
#pragma once
#include "nxrt/game/play_display.h"

namespace nxrt::play {

// A GUI perspective camera's aspect: returns true and sets *aspect when it was the authored 16:9 (or already the display aspect).
bool hud_perspective(float* aspect, double display_aspect, float* fov_degrees = nullptr);
// A GUI orthographic camera's extents (left, right, bottom, top as the game stores them: either order of each pair; the retail ball HUD has
// left 8, right -8): widens the width around its centre so |width| / |height| = display aspect, keeping the order. Only cameras whose own
// ratio is 16:9 (or already the display aspect) are touched.
bool hud_ortho(float* left, float* right, float* bottom, float* top, double display_aspect);

}  // namespace nxrt::play
