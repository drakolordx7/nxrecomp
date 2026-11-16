// SPDX-License-Identifier: GPL-3.0-or-later
// Display options of play mode, the Age of Calamity side: entry and mid hooks into the lifted game (recomp/addrs.tsv, the "M7 display" block)
// that make the game render at the window's size and aspect. The arithmetic is the base's (nxrt/game/play_display.h).
//
//   kDeviceCreate      the KTGL device's config gets the display size (the window, or the window fitted to a forced aspect) in place of 1920x1080:
//                      swapchain textures, vi layer and the 2D layout's screen
//   kSceneSize         the 3D scene's render size (1440x810 on the console, upscaled to 1080p) becomes the internal size ([display] render_scale or
//                      the fixed render size)
//   kDynResScaleA/B    the game's dynamic resolution (567p..810p docked, as a scale = height / 810) is held at 1.0 unless [display] dynamic_resolution
//   kCameraProjection  every perspective camera authored for 16:9 gets the display aspect (its field of view is vertical: Hor+); narrower displays
//                      widen the vertical angle so the 16:9 width stays in view; [display] fov_offset is added
#pragma once
#include <cstdint>

#include "nxrt/game/play_display.h"
#include "nxrt/game/play_types.h"

namespace nxrt::play {

// The sizes the hooks use for a client area of window_w x window_h (pure; also the launcher's preview of the numbers).
struct HwaocSizes {
    int display_w = 1920, display_h = 1080;   // swapchain / layout size
    int scene_w = 1920, scene_h = 1080;       // the 3D scene
    double aspect = 16.0 / 9.0;               // the 3D view's and the HUD's aspect
};
HwaocSizes hwaoc_sizes(const DisplaySettings& d, int window_w, int window_h);

void display_install(uint64_t module_base, const Settings& s, int window_w, int window_h);
void display_remove();
void display_apply(const Settings& s);   // the live options: FOV offset, dynamic resolution
DisplayLive display_live();
uint32_t display_hook_calls();

}  // namespace nxrt::play
