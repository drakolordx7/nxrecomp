// SPDX-License-Identifier: GPL-3.0-or-later
// Suit and arm cannon colour of Metroid Prime Remastered (S45, docs/S45.md; settings [tweaks] suit_color, gun_color in play_tweaks.h).
//
// Samus's look comes from base colour (BCLR) textures; this module recolours their painted areas on the CPU when the game uploads them, through
// the runtime's texture edit hook (nxrt/tex_edit.h). Glow (ICAN), normal, metal and reflection maps are other textures and stay the game's.
//   * Which textures: a table of asset ids per part (suit body per suit, the low-poly third-person suit, the morph ball and spider ball, the
//     first-person arm cannon per suit, the beam gun shared by every suit). At the first use the module reads them from the player's own paks
//     (Preload/DuplicateData.pak, Preload/MPR1/SamusGunMP1.pak, Preload/MPR1/TestAnimMP1.pak) and keeps the size and a hash of each tiled
//     buffer; an uploaded texture with the same size, shape and hash is that asset.
//   * Mask: which texels are paint. A smooth weight from the texel's hue, saturation and brightness: the warm paint of the Power and Varia
//     suits (red, orange, beige), the Gravity Suit's purple, the Phazon Suit's light grey panels, the beam gun's grey-green. The visor glass and
//     the green, cyan and red lights are outside every window and keep their colour.
//   * Recolour (OKLab): the texel's lightness keeps its distance from the paint's reference lightness (the chroma-weighted median of the
//     painted texels of level 0), so shading, wear and the two-tone panels stay; the chosen colour sets the hue and chroma, scaled by the texel's
//     own chroma against the paint's. The chosen colour's lightness is kept within 0.24..0.92 so black reads as a dark charcoal with its detail and
//     white keeps its shading. The result is mixed with the game's texel by the mask weight, in linear light.
//   * The morph ball (and the spider ball) follow suit_color: the game paints the ball in the suit's colours.
// Changes are applied 250 ms after the last one (a slider being dragged), then only the matching textures are rebuilt in the background and
// uploaded again.
#pragma once
#include <cstddef>
#include <cstdint>

#include "play/play_tweaks.h"

namespace nxrt::play {

void tint_install();                       // play mode start (no hook sites: it only needs the texture hook)
void tint_remove();
void tint_set(const TweakSettings& t);      // the saved settings
void tint_preview(const TweakSettings& t);  // the F1 page's unsaved values, used while the page keeps calling this
void tint_frame();                          // frame boundary: applies a change once it has been still for 250 ms

// ---- the parts, for the tests --------------------------------------------------------------------------------------------------
namespace tint {

enum class Part : uint8_t { Suit, Gun };
enum class Profile : uint8_t { Warm, Gravity, Phazon, Teal };

struct Target {
    const char* uuid;      // TXTR asset id
    const char* pak;       // RomFS-relative pak that holds it
    Part part;
    Profile profile;
    const char* what;
};
const Target* targets(size_t* count);

// Paint weight 0..1 of one sRGB texel.
float weight(uint8_t r, uint8_t g, uint8_t b, Profile p);

// The paint's reference lightness and chroma (OKLab) over an RGBA8 image.
struct PaintRef {
    float L = 0.5f, C = 0.05f;
};
PaintRef paint_ref(const uint8_t* rgba, uint32_t w, uint32_t h, Profile p);

// Recolours n RGBA8 texels in place toward `want` (sRGB 0..255). Alpha stays.
void recolour(uint8_t* rgba, size_t n, Profile p, const PaintRef& ref, const GlowRgb& want);

// 64-bit hash of a tiled buffer (identity of the game's texture).
uint64_t hash(const uint8_t* p, size_t n);

struct Stats {
    uint32_t known = 0;        // targets found in the player's paks
    uint64_t matches = 0;      // uploads recognised (while a colour was on)
    uint64_t edits = 0;        // levels recoloured
    uint64_t applies = 0;      // colour changes applied (after the debounce)
};
Stats stats();

}  // namespace tint
}  // namespace nxrt::play
