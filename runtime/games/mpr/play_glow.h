// SPDX-License-Identifier: GPL-3.0-or-later
// S44: the glow colour tweak ([tweaks] glow_color, suit_glow, ball_glow, gun_glow; play_tweaks.h). docs/S44.md has what is recoloured and how.
//
// Morph ball: the game keeps its glow colours in five static tables of CColor4f (linear), one entry per glow index (Power, Varia, Varia with
// the Spider Ball, Gravity, Phazon): CMorphBallMP1::skBallNormalSwooshColors, skBallBoostedSwooshColors (the trail), skBallBoostedHullGlowColors
// (the ball's glow while boosting), skBallLightningSwooshColors and skBallLightColors (the ball's light and the morph flash). Static data the
// render code reads every frame, so they are rewritten in place at the frame boundary; the game's values are kept and written back.
// The ball's inner glow (BallInnerGlow_*) and the arm cannon's charge glow (the weapon's charge / muzzle effect modules) are particle effects
// whose colours are in the effect assets; their particles are near white, so they get the colour as the generators' modulation colour
// (CParticleGen::SetModulationColor, called on the guest every frame while on: from CPlayerMP1::ProcessInput for the ball and after
// CPlayerGunMP1::Update for the gun, both existing hook sites).
// The glowing stripes of the ball, the cannon's lit parts and the suit's lights are the models' incandescence maps: their draw calls
// (CModelDataMP1::Render / RenderGun, S44 hook sites, told apart by the return address) get CModelFlagsMPT colour mode 2, the
// incandescence modulation colour (CModelFlagsMPT::ColorModulateIncan), with the setting's colour.
#pragma once
#include <cstdint>

#include "nxrt/abi.h"
#include "play_types.h"

namespace nxrt::play {

void glow_install(uint64_t game_base);   // after lockcam_install (it shares CPlayerGunMP1::Update and must run after the decoupled gun's hook)
void glow_remove();
void glow_set(const TweakSettings& t);   // the saved settings (settings_changed)
void glow_preview(const TweakSettings& t);   // the F1 page's unsaved values; used while the page keeps calling this (a quarter of a second)
void glow_frame();                       // frame boundary, main thread: (re)writes the ball tables when the wanted colours changed
// From CPlayerMP1::ProcessInput (play.cpp), on the game thread, with that hook's guest context: tints the morph ball's inner glow.
void glow_player_tick(::Ctx* c, uint64_t player);
bool glow_tab(Settings& edit);           // the F1 page's Tweaks tab (play_glow_ui.cpp)

// The colour math, exposed for the tests. Colours are linear RGBA as the game keeps them.
float glow_srgb_to_linear(uint8_t c);
// The game's colour `vanilla` recoloured to `want`: the hue and saturation of `want`, the brightness of `want` scaled by the game's brightest channel,
// the game's alpha. A black game colour stays black.
void glow_recolor(const float vanilla[4], const GlowRgb& want, float out[4]);

struct GlowStats {
    bool ball_tables_known = false;       // the game's table values were read (static init has run)
    bool ball_written = false;            // the tables hold our colours now
    uint64_t gun_writes = 0;              // frames the charge effect was tinted
    uint64_t model_tints = 0;             // model draws (ball, cannon, suit) given the incandescence colour
};
GlowStats glow_stats();

}  // namespace nxrt::play
