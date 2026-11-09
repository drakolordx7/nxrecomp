// SPDX-License-Identifier: GPL-3.0-or-later
// The render hooks of a game (game.h, Info::render): what the base's graphics layer cannot know about a title and asks the game for.
//
// The base keeps the mechanism: float16 promotion of render targets, the HDR variant of a pipeline, the FSR 1 pass in front of a draw,
// the NVN object sizes. The game says which shader is its tone map and how to patch it, which render targets are its scene chain, how
// much smaller its pre-exposed scene colour is, and where its own scene resolution is set. Every member is optional; with no table (or a
// null member) the feature that needs it stays off:
//   HDR scene            needs tonemap_patch and promote_target (otherwise the swapchain stays SDR: the base cannot tell what to promote or patch)
//   scene upscale (FSR)  needs is_tonemap (the pass the scene is read in) and scene_scale to have an effect on the game
//   NVN struct sizes     none: the base's generated defaults (the NVN header) stand
//
// Where the Metroid Prime Remastered table lives: games/mpr/runtime/mpr_render.cpp.
#pragma once
#include <cstdint>
#include <vector>

namespace nxrt::game {

// A texture the NVN back end is about to create, as far as the promotion rule needs to know. Only 8-bit sRGB RGBA textures are asked
// about (the only ones that can become float16).
struct RenderTarget {
    bool is_2d = false;
    uint32_t levels = 1, layers = 1, samples = 1;
    bool display = false;          // NVN_TEXTURE_FLAGS_DISPLAY
    bool compressible = false;     // NVN_TEXTURE_FLAGS_COMPRESSIBLE
};

// One NVN object size the game's own code fixes (the room it leaves for an object it embeds), overriding the generated default.
struct NvnStructSize {
    const char* name;              // "NVNtexture"
    uint32_t size;
};

struct GameRender {
    // The game's tone-map fragment shader: true when `spirv` is it. Called once per shader (the base caches the answer by hash) and only for
    // shaders of at most 16384 words.
    bool (*is_tonemap)(const std::vector<uint32_t>& spirv) = nullptr;
    // The HDR variant of the tone-map shader: writes the patched module to `out` and returns true, or returns false when `in` is not it.
    // Used while the HDR scene is active.
    bool (*tonemap_patch)(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) = nullptr;
    // True for a render target the HDR scene keeps as float16 instead of 8-bit sRGB (the colour targets of the game's post chain).
    bool (*promote_target)(const RenderTarget& t) = nullptr;
    // The scene colour the tone-map pass reads is this many times the display-referred range (the game's pre-exposure: 0.1 = about ten times
    // smaller). FSR 1 runs on the scene colour scaled back by it. 1 = the scene is already display-referred.
    float scene_prescale = 1.0f;
    // Told the percent of the output size ([upscale] scene_scale, 25..100) the game should render its 3D scene at, whenever it changes
    // (and once at start-up). The game applies it through its own dynamic-resolution path. May run on any thread; store and return.
    void (*scene_scale)(int percent) = nullptr;

    // NVN object sizes the game fixes (struct_sizes below), `nvn_struct_size_count` entries; null = the generated defaults.
    const NvnStructSize* nvn_struct_sizes = nullptr;
    uint32_t nvn_struct_size_count = 0;
};

}  // namespace nxrt::game
