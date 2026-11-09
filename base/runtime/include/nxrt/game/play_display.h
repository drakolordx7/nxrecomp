// SPDX-License-Identifier: GPL-3.0-or-later
// Display options of play mode (S10): internal resolution, aspect ratio, field of view and filtering.
//
// Two halves. This one (no game, no host state) is the arithmetic every option reduces to and is covered by unit tests, plus the renderer
// tuning (anisotropy, LOD bias) that applies to every game. The other half applies the options to one game's own objects through hooks
// and belongs to the game module; play mode reaches it through GamePlay (play_game.h: install_render, settings_changed, display_live). The
// options that only make sense for one game (post effects, dynamic resolution) are that game's own (settings.h GameOption).
#pragma once
#include <cstdint>

#include "nxrt/game/play_types.h"

namespace nxrt::play {

constexpr double kHudAspect = 16.0 / 9.0;           // the aspect a game with no display of its own is taken to be drawn for
constexpr int kMinRenderDim = 320, kMaxRenderDim = 16384;
constexpr double kMinAspect = 1.0, kMaxAspect = 5.0;

// The internal (render) size. A fixed size (render_width and render_height both > 0) wins; otherwise the window's client size times
// render_scale percent. The result is rounded to even numbers and clamped to kMinRenderDim..kMaxRenderDim per side and to at most
// 8192 x 8192 x 2 pixels in area (the clamp keeps the aspect).
void internal_size(const DisplaySettings& d, int window_w, int window_h, int* out_w, int* out_h);

// The aspect the 3D view and the HUD use: the forced ratio when `aspect` > 0 (clamped to kMinAspect..kMaxAspect), else w / h.
double display_aspect(const DisplaySettings& d, int w, int h);

// Vertical field of view in radians plus an offset in degrees, kept between 10 and 170 degrees.
float fov_with_offset(float base_radians, double offset_degrees);

// What the game's display hooks report back to play mode: the internal size and aspect (the menu pointer's geometry) and the field of view offset
// in force (the game's camera hooks add it).
struct DisplayLive {
    int render_w = 1600, render_h = 900;   // internal size, fixed once the game has asked
    double aspect = 0;                     // 0 = render_w / render_h
    double fov_offset = 0;
};

// The renderer's sampler tuning from the settings ([display] anisotropy, lod_bias): at start and whenever the settings change.
void display_tuning(const DisplaySettings& d);

}  // namespace nxrt::play
