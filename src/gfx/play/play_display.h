// SPDX-License-Identifier: GPL-3.0-or-later
// Display options of play mode (S10): internal resolution, aspect ratio, field of view, dynamic resolution, filtering and post effects.
//
// Two halves. The first (display_math, no game, no host state) is the arithmetic every option reduces to and is covered by unit tests.
// The second installs entry hooks into the lifted game (recomp/games/mpr/main.sites, the "S10" block) and applies the options to the
// game's own objects. All addresses are offsets in `main` (docs/mpr/hooks.md, validated against the 1.0.0 binary in IDA):
//
//   COsContext::GetScreenWidth / GetScreenHeight   0x1248 / 0x1250   docked render size, read by the graphics context, the render
//        targets, every GUI viewport and text pane; the hooks answer the configured internal size (the handheld getters stay as they
//        are: the touch screen maps its coordinates through them)
//   CDynamicResolutionControl::GetResolutionScale  0x3c08a0          called once per frame by CGameRenderJob::RenderGamePass; 1.0 turns
//        dynamic resolution off
//   CDisplayManager::UpdateCameraNodes             0xafe41c          builds the world projection from the aspect stored at +152 of the
//        camera scene node proxy ([this + 16]); the hook writes the aspect there (and into CCameraManagerMP1's cached default aspect,
//        0x20e7a7c) before the original runs. The game's field of view is vertical, so a wider aspect widens the view (Hor+)
//   CGuiCamera::GetProjectionState                 0xbd52fc          every GUIF frame (HUD, menus, title) goes through this camera; the
//        hook widens a 16:9 camera to the display aspect so the frame keeps its proportions and sits centred
//   CGameCamera::GetVerticalFOV                    0x4c2998          returns [this + 0x460] in radians; the hook adds the FOV offset
//   CRenderPass_Bloom::Render                      0x3a450c          skipped when bloom is off (it composites into the frame in place)
//   CRenderPass_ChromaticAberration::Render        0x3a69b0          its strength is zeroed when the effect is off
#pragma once
#include <cstdint>

#include "play_types.h"

namespace nxrt::play {

// ---- arithmetic -----------------------------------------------------------------------------------------------------------------
constexpr double kHudAspect = 16.0 / 9.0;           // what the GUIF frames and the retail cameras are authored for
constexpr int kMinRenderDim = 320, kMaxRenderDim = 16384;
constexpr double kMinAspect = 1.0, kMaxAspect = 5.0;

// The internal (render) size. A fixed size (render_width and render_height both > 0) wins; otherwise the window's client size times
// render_scale percent. The result is rounded to even numbers and clamped to kMinRenderDim..kMaxRenderDim per side and to at most
// 8192 x 8192 x 2 pixels in area (the clamp keeps the aspect).
void internal_size(const DisplaySettings& d, int window_w, int window_h, int* out_w, int* out_h);

// The aspect the 3D view and the HUD use: the forced ratio when `aspect` > 0 (clamped to kMinAspect..kMaxAspect), else w / h.
double display_aspect(const DisplaySettings& d, int w, int h);

// A GUI perspective camera's aspect: returns true and sets *aspect when it was the authored 16:9 (or already the display aspect).
bool hud_perspective(float* aspect, double display_aspect);
// A GUI orthographic camera's extents (left, right, bottom, top as the game stores them: either order of bottom and top): widens the
// width around its centre so width / height = display aspect. Only cameras whose own ratio is 16:9 (or already the display aspect)
// are touched.
bool hud_ortho(float* left, float* right, float* bottom, float* top, double display_aspect);

// Vertical field of view in radians plus an offset in degrees, kept between 10 and 170 degrees.
float fov_with_offset(float base_radians, double offset_degrees);

// ---- the game side -------------------------------------------------------------------------------------------------------------
// The options the hooks read; written by play mode (any thread).
struct DisplayLive {
    int render_w = 1600, render_h = 900;   // internal size, fixed once the game has asked
    double aspect = 0;                     // 0 = render_w / render_h
    double fov_offset = 0;
    bool dynamic_resolution = false;
    bool bloom = true;
    bool chromatic_aberration = true;
};

// Installs the hooks relative to the module base (st.base). Idempotent. `window_w/h`: client size in pixels at start-up (the internal
// size is derived from it once, here).
void display_install(uint64_t module_base, const DisplaySettings& d, int window_w, int window_h);
void display_remove();
// Applies the live options (everything except the internal size, which only changes with a restart).
void display_apply(const DisplaySettings& d);
DisplayLive display_live();
uint32_t display_hook_calls();             // calls into the hooks since install (tests, log)

}  // namespace nxrt::play
