// SPDX-License-Identifier: GPL-3.0-or-later
// What the NVN back end asks of the HDR scene pipeline (S14). The mechanism is here; which targets and which shader are the game's comes from its
// render table (nxrt/game_render.h, game::Info::render), and a game without one gets neither HDR scene nor scene upscale:
//   hdr_promote_target   should this render target be kept as float16 instead of 8-bit sRGB? (the game's rule, while the HDR scene is active)
//   hdr_tonemap_variant  the HDR variant of a fragment shader (the tone-map shader), or false when the shader is not one
#pragma once
#include <volk.h>

#include <cstdint>
#include <string>
#include <vector>

namespace nxrt::extras {

// The format to use instead of `chosen` for a texture: VK_FORMAT_R16G16B16A16_SFLOAT for an 8-bit sRGB texture the game's rule names, while the HDR
// scene is active; VK_FORMAT_UNDEFINED otherwise.
VkFormat hdr_promote_target(VkFormat chosen, int nvn_texture_flags, bool is_2d, uint32_t levels, uint32_t layers, uint32_t samples, bool nvn_format_is_srgb);

// SPIR-V of the HDR variant of the fragment shader `words` (hash for the log), or false when it is not the tone-map shader / the HDR scene is off / the
// game has no patch. Made once per hash.
bool hdr_tonemap_variant(const std::vector<uint32_t>& words, const std::string& hash, std::vector<uint32_t>& out);

// True when `words` is the game's tone-map fragment shader (its is_tonemap); cached by hash. Independent of the HDR scene.
bool hdr_is_tonemap(const std::vector<uint32_t>& words, const std::string& hash);

// Scene upscaler (S14): the game renders the 3D scene below the output size ([upscale] scene_scale, the game's dynamic-resolution path) and its tone-map
// pass reads that small HDR target and writes the full-size window texture. With FSR 1 on, the replay runs EASU + RCAS on the HDR target first and the tone-map
// draw reads the result instead of the small target (the game's bilinear filter is bypassed).
bool scene_upscale_wanted();                          // the upscale mode is FSR 1 and the game has a tone-map predicate
class Fsr1Spatial;
Fsr1Spatial* scene_upscaler(void* gfx_device);        // created on first use; the gfx::Device of the NVN back end
float scene_upscale_prescale();                       // the game's scene pre-exposure scale (GameRender::scene_prescale)

}  // namespace nxrt::extras
