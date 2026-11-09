// SPDX-License-Identifier: GPL-3.0-or-later
// Display options of play mode, the Metroid Prime Remastered side: the hooks on the game's screen size, camera, GUI camera, field of view
// and post-effect functions (display_hooks.h). The arithmetic they use is the base's (src/gfx/play/play_display.h).
#include "display_hooks.h"
#include "nxrt/extras.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "nxrt/abi.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::play {

// ---- the game side -------------------------------------------------------------------------------------------------------------
namespace {

constexpr const char* kTag = "display";
// offsets in main
constexpr uint64_t kGetScreenWidth = 0x1248, kGetScreenHeight = 0x1250;
constexpr uint64_t kGetResolutionScale = 0x3c08a0;
constexpr uint64_t kUpdateCameraNodes = 0xafe41c;
constexpr uint64_t kGuiProjectionState = 0xbd52fc;
constexpr uint64_t kGetVerticalFov = 0x4c2998;
constexpr uint64_t kUiUpdateCameraNodes = 0xb0e5ec;       // CUIDisplayManager::UpdateCameraNodes
constexpr uint64_t kBloomRender = 0x3a450c;
constexpr uint64_t kChromaticRender = 0x3a69b0;
constexpr uint64_t kDefaultAspectGlobal = 0x20e7a7c;      // CCameraManagerMP1's cached aspect (written by CreateCameras)
// object layouts
constexpr size_t kProxyAspect = 152;       // [CDisplayManager + 16] + 152
constexpr size_t kGuiCamType = 264, kGuiCamA = 268, kGuiCamB = 272, kGuiCamC = 276, kGuiCamD = 280;   // CGuiCamera
constexpr size_t kCameraVerticalFov = 1120;               // CGameCamera, radians
// CUIDisplayManager: [+312] the "UIOrthoCamera" scene node proxy, [+320] set once the level's UI camera was registered. The proxy keeps its
// SProjectionState at +144: [0] kind (0 perspective, 1 frustum, 2 orthographic), [4] fov, [8] aspect, then right, left, bottom, top, near, far.
constexpr size_t kUiProxy = 312, kUiRegistered = 320, kProxyState = 144;

struct State {
    uint64_t base = 0;
    std::atomic<int> w{1600}, h{900};
    std::atomic<double> aspect{0.0};
    std::atomic<double> fov_offset{0.0};
    std::atomic<bool> dynres{false}, bloom{true}, chroma{true};
    std::atomic<uint32_t> calls{0};
    std::vector<uint32_t> hooks;
    bool installed = false;
    std::mutex mu;
    int gui_logged = 0, ui_logged = 0;
};

State& st() {
    static State s;
    return s;
}

double current_aspect() {
    State& s = st();
    const double a = s.aspect.load(std::memory_order_relaxed);
    if (a > 0) return std::clamp(a, kMinAspect, kMaxAspect);
    return std::clamp(static_cast<double>(s.w.load()) / std::max(1, s.h.load()), kMinAspect, kMaxAspect);
}

int hook_width(::Ctx* c, uint64_t, void*) {
    c->x[0] = static_cast<uint64_t>(st().w.load(std::memory_order_relaxed));
    st().calls.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

int hook_height(::Ctx* c, uint64_t, void*) {
    c->x[0] = static_cast<uint64_t>(st().h.load(std::memory_order_relaxed));
    st().calls.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

int hook_resolution_scale(::Ctx* c, uint64_t, void*) {
    State& s = st();
    if (s.dynres.load(std::memory_order_relaxed)) return 0;      // the game's own controller
    // [upscale] scene_scale (S14): the 3D scene is rendered at that percent of the output size per side, through the game's own
    // dynamic-resolution path (it takes an area factor and makes the scene targets sqrt(factor) of the full size; the post chain and the HUD stay native)
    const float lin = static_cast<float>(extras::upscale_scene_scale()) * 0.01f;
    c->v[0].f32[0] = lin < 0.999f ? lin * lin : 1.0f;
    s.calls.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

int hook_camera_nodes(::Ctx* c, uint64_t, void*) {
    State& s = st();
    const uint64_t mgr = c->x[0];
    if (!mgr || !mem::is_mapped(mgr, 32)) return 0;
    const uint64_t node = *reinterpret_cast<const uint64_t*>(mgr + 16);
    if (!node || !mem::is_mapped(node, kProxyAspect + 4)) return 0;
    const float a = static_cast<float>(current_aspect());
    *reinterpret_cast<float*>(node + kProxyAspect) = a;
    if (s.base) *reinterpret_cast<float*>(s.base + kDefaultAspectGlobal) = a;
    s.calls.fetch_add(1, std::memory_order_relaxed);
    return 0;                                                    // the game's function runs with the aspect in place
}

int hook_ui_camera_nodes(::Ctx* c, uint64_t, void*) {
    State& s = st();
    const uint64_t mgr = c->x[0];
    if (!mgr || !mem::is_mapped(mgr, kUiRegistered + 1)) return 0;
    if (*reinterpret_cast<const uint8_t*>(mgr + kUiRegistered) == 0) return 0;
    const uint64_t proxy = *reinterpret_cast<const uint64_t*>(mgr + kUiProxy);
    if (!proxy || !mem::is_mapped(proxy + kProxyState, 36)) return 0;
    float* p = reinterpret_cast<float*>(proxy + kProxyState);
    const int kind = *reinterpret_cast<const int*>(p);
    const double disp = current_aspect();
    bool changed = false;
    if (kind == 2) {
        changed = hud_ortho(&p[4], &p[3], &p[5], &p[6], disp);                // left, right, bottom, top
    } else if (kind == 0) {
        const float old = p[2];
        if (hud_perspective(&p[2], disp) && old > 0) {
            const float f = p[2] / old;
            p[3] *= f;                                                        // the frustum's right and left follow the aspect
            p[4] *= f;
            changed = true;
        }
    }
    if (changed) s.calls.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.ui_logged < 6) {
            ++s.ui_logged;
            NXLOG_I(kTag, "UI camera %llx: kind %d, fov %.4f aspect %.4f right %.3f left %.3f bottom %.3f top %.3f near %.3f far %.3f, display aspect %.4f%s",
                    static_cast<unsigned long long>(proxy), kind, p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], disp, changed ? " (adjusted)" : "");
        }
    }
    return 0;
}

int hook_gui_projection(::Ctx* c, uint64_t, void*) {
    State& s = st();
    const uint64_t cam = c->x[0];
    if (!cam) return 0;
    const double disp = current_aspect();
    const int type = *reinterpret_cast<const int*>(cam + kGuiCamType);
    float* a = reinterpret_cast<float*>(cam + kGuiCamA);
    float* b = reinterpret_cast<float*>(cam + kGuiCamB);
    float* cc = reinterpret_cast<float*>(cam + kGuiCamC);
    float* d = reinterpret_cast<float*>(cam + kGuiCamD);
    bool changed = false;
    if (type == 0) changed = hud_perspective(b, disp, a);                    // fov (degrees), aspect, near, far
    else if (type == 1) changed = hud_ortho(a, b, cc, d, disp);              // left, right, then the two vertical extents, near, far
    if (changed) s.calls.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.gui_logged < 24) {
            ++s.gui_logged;
            NXLOG_I(kTag, "GUI camera %llx: type %d, fields %.4f %.4f %.4f %.4f, display aspect %.4f%s", static_cast<unsigned long long>(cam), type, *a, *b, *cc, *d,
                    disp, changed ? " (adjusted)" : "");
        }
    }
    return 0;
}

int hook_vertical_fov(::Ctx* c, uint64_t, void*) {
    State& s = st();
    const double off = s.fov_offset.load(std::memory_order_relaxed);
    if (off == 0.0) return 0;
    const uint64_t cam = c->x[0];
    if (!cam) return 0;
    const float base = *reinterpret_cast<const float*>(cam + kCameraVerticalFov);
    if (!(base > 0.05f && base < 3.0f)) return 0;                // not an angle in radians: leave it alone
    c->v[0].f32[0] = fov_with_offset(base, off);
    s.calls.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

int hook_bloom(::Ctx* c, uint64_t, void*) {
    if (st().bloom.load(std::memory_order_relaxed)) return 0;
    c->x[0] = 0;
    st().calls.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

int hook_chromatic(::Ctx* c, uint64_t, void*) {
    if (st().chroma.load(std::memory_order_relaxed)) return 0;
    const uint64_t params = c->x[2];                             // const SChromaticAberrationParams&: amount, offset, scale, flags
    if (params && mem::is_mapped(params, 16)) {
        float* f = reinterpret_cast<float*>(params);
        f[0] = 0.0f;
        f[1] = 0.0f;
        st().calls.fetch_add(1, std::memory_order_relaxed);
    }
    return 0;
}

}  // namespace

void display_apply(const DisplaySettings& d) {
    State& s = st();
    s.aspect.store(d.aspect > 0 ? std::clamp(d.aspect, kMinAspect, kMaxAspect) : 0.0);
    s.fov_offset.store(std::clamp(d.fov_offset, -30.0, 40.0));
    s.dynres.store(d.dynamic_resolution);
    s.bloom.store(d.bloom);
    s.chroma.store(d.chromatic_aberration);
}

void display_install(uint64_t module_base, const DisplaySettings& d, int window_w, int window_h) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.installed) return;
    s.base = module_base;
    int w = 1600, h = 900;
    internal_size(d, window_w, window_h, &w, &h);
    s.w.store(w);
    s.h.store(h);
    display_apply(d);
    const struct { uint64_t off; SiteHookFn fn; } table[] = {
        {kGetScreenWidth, hook_width},           {kGetScreenHeight, hook_height},       {kGetResolutionScale, hook_resolution_scale},
        {kUpdateCameraNodes, hook_camera_nodes}, {kGuiProjectionState, hook_gui_projection}, {kGetVerticalFov, hook_vertical_fov},
        {kBloomRender, hook_bloom},              {kChromaticRender, hook_chromatic},    {kUiUpdateCameraNodes, hook_ui_camera_nodes},
    };
    for (const auto& t : table) s.hooks.push_back(hook_add_entry(module_base + t.off, t.fn));
    s.installed = true;
    NXLOG_I(kTag, "internal resolution %dx%d (window %dx%d, scale %d%%%s), aspect %s, FOV offset %.1f deg, dynamic resolution %s, anisotropy %d, LOD bias %.2f, bloom %s, chromatic aberration %s",
            w, h, window_w, window_h, d.render_scale, d.render_width > 0 && d.render_height > 0 ? ", fixed size" : "",
            d.aspect > 0 ? std::to_string(d.aspect).c_str() : "auto", d.fov_offset, d.dynamic_resolution ? "on" : "off", d.anisotropy, d.lod_bias,
            d.bloom ? "on" : "off", d.chromatic_aberration ? "on" : "off");
}

void display_remove() {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    for (uint32_t id : s.hooks) hook_remove(id);
    s.hooks.clear();
    s.installed = false;
}

DisplayLive display_live() {
    State& s = st();
    DisplayLive l;
    l.render_w = s.w.load();
    l.render_h = s.h.load();
    l.aspect = s.aspect.load();
    l.fov_offset = s.fov_offset.load();
    l.dynamic_resolution = s.dynres.load();
    l.bloom = s.bloom.load();
    l.chromatic_aberration = s.chroma.load();
    return l;
}

uint32_t display_hook_calls() { return st().calls.load(); }

}  // namespace nxrt::play
