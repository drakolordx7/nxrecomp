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
// The glowing stripes of the ball are that inner glow and the ball's light (S44c: the CModelFlagsMPT mode-2 incandescence colour that S44 gave
// the ball's draws is gone: it lit the whole shell). S44b: the arm cannon's lit panels take their colour from a small ramp texture and the
// suits' lights from their glow maps; those textures are recoloured as the game uploads them (nxrt/tex_edit.h through mpr_texedit.h, told apart
// by a hash of their smaller mip levels).
#pragma once
#include <cstddef>
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
uint8_t glow_linear_to_srgb(float v);
// Identity of a glow texture: a hash of the last quarter of its tiled storage (the smaller mip levels, which the game loads before level 0)
// with its size and shape.
uint64_t glow_texture_tail_hash(const uint8_t* tiled, size_t size, uint32_t w, uint32_t h, uint32_t levels);
// One texel of a glow ramp texture (RGBA8, sRGB-encoded when srgb) recoloured to want in place: glow_recolor in linear light, alpha kept.
void glow_recolor_texel(uint8_t rgba[4], bool srgb, const GlowRgb& want);

// The glow textures (S44b, S44c): the arm cannon's ramps and the suits' glow maps, recoloured at upload. All ASTC 8x5 sRGB.
enum class GlowTexPart : uint8_t { Gun, Suit };
struct GlowTexture {
    uint32_t w, h, levels;
    uint64_t tail;      // glow_texture_tail_hash of the game's texture
    GlowTexPart part;
    const char* what;
};
const GlowTexture* glow_textures(size_t* count);
// The texture edit id of a texture that matched row `row`: the row and a hash of the whole storage, so data edited from an early upload
// (level 0 not streamed in yet, same tail) is not reused for the complete texture. Below 2^55 (the discovery ids have bit 55).
uint64_t glow_texture_id(size_t row, const uint8_t* tiled, size_t size);
int glow_texture_row(uint64_t id);   // the row of a glow_texture_id, -1 for 0, a discovery id or an unknown row

struct GlowStats {
    bool ball_tables_known = false;       // the game's table values were read (static init has run)
    bool ball_written = false;            // the tables hold our colours now
    uint64_t gun_writes = 0;              // frames the charge effect was tinted
    uint64_t ramp_levels = 0;             // glow ramp texture levels recoloured (the cannon's panels, the suit's lights)
};
GlowStats glow_stats();

}  // namespace nxrt::play
