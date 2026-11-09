// SPDX-License-Identifier: GPL-3.0-or-later
#include "extras/hdr_nvn.h"

#include <map>
#include <mutex>

#include <nvn/nvn.h>

#include "nxrt/extras.h"
#include "nxrt/game.h"
#include "nxrt/game_render.h"
#include "nxrt/log.h"
#include "extras/fsr1_spatial.h"

namespace nxrt::extras {
namespace {

const game::GameRender* render() { return game::info().render; }

// What is known of one fragment shader: whether it is the game's tone-map shader, and its HDR variant (made the first time it is asked for).
struct ToneMap {
    bool is = false;
    bool variant_tried = false, variant_ok = false;
    std::vector<uint32_t> variant;
};

std::mutex g_mu;
std::map<std::string, ToneMap> g_shaders;

ToneMap& entry(const std::vector<uint32_t>& words, const std::string& hash) {   // g_mu held
    auto it = g_shaders.find(hash);
    if (it != g_shaders.end()) return it->second;
    ToneMap t;
    const game::GameRender* r = render();
    t.is = r && r->is_tonemap && words.size() <= 16384 && r->is_tonemap(words);
    return g_shaders.emplace(hash, std::move(t)).first->second;
}

}  // namespace

VkFormat hdr_promote_target(VkFormat chosen, int flags, bool is_2d, uint32_t levels, uint32_t layers, uint32_t samples, bool nvn_srgb) {
    const game::GameRender* r = render();
    if (!r || !r->promote_target || !r->tonemap_patch) return VK_FORMAT_UNDEFINED;
    if (!nvn_srgb || chosen != VK_FORMAT_R8G8B8A8_SRGB) return VK_FORMAT_UNDEFINED;
    if (!hdr_scene_active()) return VK_FORMAT_UNDEFINED;
    game::RenderTarget t;
    t.is_2d = is_2d;
    t.levels = levels;
    t.layers = layers;
    t.samples = samples;
    t.display = (flags & NVN_TEXTURE_FLAGS_DISPLAY) != 0;
    t.compressible = (flags & NVN_TEXTURE_FLAGS_COMPRESSIBLE) != 0;
    return r->promote_target(t) ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_UNDEFINED;
}

bool hdr_tonemap_variant(const std::vector<uint32_t>& words, const std::string& hash, std::vector<uint32_t>& out) {
    const game::GameRender* r = render();
    if (!r || !r->tonemap_patch || !hdr_scene_active()) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    ToneMap& t = entry(words, hash);
    if (!t.is) return false;
    if (!t.variant_tried) {
        t.variant_tried = true;
        t.variant_ok = r->tonemap_patch(words, t.variant);
        if (t.variant_ok) NXLOG_I("hdr", "tone-map shader %s: HDR variant made", hash.c_str());
    }
    if (!t.variant_ok) return false;
    out = t.variant;
    return true;
}

bool hdr_is_tonemap(const std::vector<uint32_t>& words, const std::string& hash) {
    if (!render() || !render()->is_tonemap) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    return entry(words, hash).is;
}

bool scene_upscale_wanted() { return upscale_mode() == UpscaleMode::Fsr1 && render() && render()->is_tonemap; }

Fsr1Spatial* scene_upscaler(void* gfx_device) {
    static Fsr1Spatial* inst = nullptr;
    static bool tried = false;
    if (!tried && gfx_device) {
        tried = true;
        auto* f = new Fsr1Spatial();
        if (f->init(*static_cast<gfx::Device*>(gfx_device))) inst = f;
        else delete f;
    }
    return inst;
}

float scene_upscale_prescale() {
    const game::GameRender* r = render();
    return r && r->scene_prescale > 0 ? r->scene_prescale : 1.0f;
}

}  // namespace nxrt::extras
