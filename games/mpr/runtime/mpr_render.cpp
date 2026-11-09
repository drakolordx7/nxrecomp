// SPDX-License-Identifier: GPL-3.0-or-later
// Metroid Prime Remastered's render table (nxrt/game_render.h): what the base's graphics layer asks the game about its post chain.
//   tone-map shader   the structural match and the HDR patch (mpr_tonemap_patch.h); the curve block it reads is rewritten by mpr_tonemap.cpp
//   promotion rule    the HDR scene keeps the post chain's colour targets as float16: 8-bit sRGB, created for display or compressible, one 2D level
//   scene pre-exposure  the scene colour the tone-map pass reads is about ten times smaller than the display-referred range (the game exposes it
//                       before the tone map), so FSR 1 runs on it scaled by 0.1 / back
//   scene scale       [upscale] scene_scale arrives here and is answered by the game's dynamic-resolution hook (display_hooks.cpp)
//   NVN sizes         the room the game's own classes leave for the NVN objects they embed (docs/nvn_struct_sizes.md, IDA on main.elf: NVNManaged::CTexture
//                     keeps its refcount at +128, CProgram its flag at +128, CSampler its flag at +64, and the texture view sits 16 bytes below the next
//                     field of CGraphicsTextureToken_NVN)
#include <vector>

#include "display_hooks.h"
#include "mpr_tonemap_patch.h"
#include "nxrt/game_render.h"

namespace nxrt::games::mpr {
namespace {

bool is_tonemap(const std::vector<uint32_t>& spirv) {
    std::vector<uint32_t> scratch;
    return spirv_hdr_tonemap(spirv, scratch);
}

bool promote_target(const game::RenderTarget& t) {
    if (!t.is_2d || t.levels != 1 || t.layers != 1 || t.samples > 1) return false;
    return t.display || t.compressible;
}

void scene_scale(int percent) { play::display_set_scene_scale(percent); }

const game::NvnStructSize kNvnSizes[] = {
    {"NVNtexture", 0x80}, {"NVNprogram", 0x80}, {"NVNsampler", 0x40}, {"NVNsamplerBuilder", 0x40}, {"NVNtextureView", 0x10},
};

}

const game::GameRender& render_table() {
    static const game::GameRender r = [] {
        game::GameRender g;
        g.is_tonemap = &is_tonemap;
        g.tonemap_patch = &spirv_hdr_tonemap;
        g.promote_target = &promote_target;
        g.scene_prescale = 0.1f;
        g.scene_scale = &scene_scale;
        g.nvn_struct_sizes = kNvnSizes;
        g.nvn_struct_size_count = static_cast<uint32_t>(sizeof kNvnSizes / sizeof kNvnSizes[0]);
        return g;
    }();
    return r;
}

}
