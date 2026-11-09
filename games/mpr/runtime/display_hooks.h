// SPDX-License-Identifier: GPL-3.0-or-later
// Display options of play mode (S10), the Metroid Prime Remastered side: entry hooks into the lifted game (recomp/games/mpr/main.sites,
// the "S10" block) that apply the options to the game's own objects. The arithmetic is the base's (src/gfx/play/play_display.h). All addresses
// are offsets in `main` (docs/mpr/hooks.md, validated against the 1.0.0 binary in IDA):
//
//   COsContext::GetScreenWidth / GetScreenHeight   0x1248 / 0x1250   docked render size, read by the graphics context, the render
//        targets, every GUI viewport and text pane; the hooks answer the configured internal size (the handheld getters stay as they
//        are: the touch screen maps its coordinates through them)
//   CDynamicResolutionControl::GetResolutionScale  0x3c08a0          called once per frame by CGameRenderJob::RenderGamePass; 1.0 turns
//        dynamic resolution off
//   CDisplayManager::UpdateCameraNodes             0xafe41c          builds the world projection from the aspect stored at +152 of the
//        camera scene node proxy ([this + 16]); the hook writes the aspect there (and into CCameraManagerMP1's cached default aspect,
//        0x20e7a7c) before the original runs. The game's field of view is vertical, so a wider aspect widens the view (Hor+)
//   CUIDisplayManager::UpdateCameraNodes           0xb0e5ec          the front end (title, menus) draws its 2D UI through the "UIOrthoCamera"
//        proxy of this manager, authored for 16:9; the hook widens the proxy's projection state to the display aspect
//   CGuiCamera::GetProjectionState                 0xbd52fc          every GUIF frame (HUD, menus, title) goes through this camera; the
//        hook widens a 16:9 camera to the display aspect so the frame keeps its proportions and sits centred
//   CGameCamera::GetVerticalFOV                    0x4c2998          returns [this + 0x460] in radians; the hook adds the FOV offset
//   CRenderPass_Bloom::Render                      0x3a450c          skipped when bloom is off (it composites into the frame in place)
//   CRenderPass_ChromaticAberration::Render        0x3a69b0          its strength is zeroed when the effect is off
#pragma once
#include <cstdint>

#include "play_display.h"

namespace nxrt::play {

// Installs the hooks relative to the module base. Idempotent. `window_w/h`: client size in pixels at start-up (the internal size is derived
// from it once, here).
void display_install(uint64_t module_base, const DisplaySettings& d, int window_w, int window_h);
void display_remove();
// Applies the live options (everything except the internal size, which only changes with a restart).
void display_apply(const DisplaySettings& d);
DisplayLive display_live();
uint32_t display_hook_calls();             // calls into the hooks since install (tests, log)

}  // namespace nxrt::play
