// SPDX-License-Identifier: GPL-3.0-or-later
// Display options of play mode, the Age of Calamity side (hwaoc_display.h).
#include "hwaoc_display.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "hwaoc_addrs.h"
#include "hwaoc_settings.h"
#include "nxrt/abi.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::play {

namespace {

using namespace nxrt::games::hwaoc::addr;
constexpr const char* kTag = "display";

// KTGL camera (IDA, 1.4.1): projection inputs and output of the function at kCameraProjection.
constexpr size_t kCamProj = 192, kCamNear = 320, kCamFar = 324, kCamFov = 328, kCamAspect = 332, kCamFlags = 344;
constexpr uint32_t kConsoleSceneHeight = 810;   // the dynamic resolution's full height (its scale is height / 810.0)

struct State {
    uint64_t base = 0;
    HwaocSizes sz;
    std::atomic<double> fov_offset{0.0};
    std::atomic<bool> dynres{false};
    std::atomic<bool> hud_centered{true};   // [display] hud
    int ortho_logged = 0;
    std::atomic<uint32_t> calls{0};
    std::vector<uint32_t> hooks;
    bool installed = false;
    std::mutex mu;
    int cam_logged = 0;
};

State& st() {
    static State s;
    return s;
}

bool near_ratio(double a, double b) { return std::fabs(a - b) < 0.02; }

int even_dim(double v) { return std::clamp(static_cast<int>(std::lround(v)) & ~1, kMinRenderDim, kMaxRenderDim); }

// x1 = the device config: u32 [0] width, [1] height, ... [8] width, [9] height (0 = the first pair).
int hook_device_create(::Ctx* c, uint64_t, void*) {
    State& s = st();
    const uint64_t cfg = c->x[1];
    if (!cfg || !mem::is_mapped(cfg, 40)) return 0;
    uint32_t* p = reinterpret_cast<uint32_t*>(cfg);
    NXLOG_I(kTag, "device config %ux%u (second pair %ux%u) -> %dx%d", p[0], p[1], p[8], p[9], s.sz.display_w, s.sz.display_h);
    p[0] = static_cast<uint32_t>(s.sz.display_w);
    p[1] = static_cast<uint32_t>(s.sz.display_h);
    if (p[8] != 0 || p[9] != 0) {
        p[8] = static_cast<uint32_t>(s.sz.display_w);
        p[9] = static_cast<uint32_t>(s.sz.display_h);
    }
    s.calls.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

// before `str x8, [x20, #0x368]`: x8 = scene width | height << 32
int hook_scene_size(::Ctx* c, uint64_t, void*) {
    State& s = st();
    NXLOG_I(kTag, "scene size %llux%llu -> %dx%d", static_cast<unsigned long long>(c->x[8] & 0xffffffffu), static_cast<unsigned long long>(c->x[8] >> 32),
            s.sz.scene_w, s.sz.scene_h);
    c->x[8] = static_cast<uint64_t>(static_cast<uint32_t>(s.sz.scene_w)) | (static_cast<uint64_t>(static_cast<uint32_t>(s.sz.scene_h)) << 32);
    s.calls.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

// before `ucvtf s0, s0`: s0 = the dynamic scene height; the next instructions divide it by 810.0
int hook_dynres(::Ctx* c, uint64_t, void*) {
    if (st().dynres.load(std::memory_order_relaxed)) return 0;
    c->v[0].u32[0] = kConsoleSceneHeight;
    return 0;
}

// The perspective matrix of the game's own function (IDA 0x18a1080) with the aspect and field of view given; xoff goes to m[8] (a shift of the picture
// in normalised device x, -xoff).
void write_projection(float* f, uint32_t flags, float fov, float aspect, float xoff) {
    const bool reversed = (flags & 2) != 0;
    const float n = reversed ? f[kCamFar / 4] : f[kCamNear / 4];
    const float fr = reversed ? f[kCamNear / 4] : f[kCamFar / 4];
    const float cot = 1.0f / std::tan(fov * 0.5f);
    const float q = fr / (n - fr);
    float* m = f + kCamProj / 4;                                    // 4x4, rows of 4 floats at +192
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = cot / aspect;
    m[5] = cot;
    m[8] = xoff;
    m[10] = q;
    m[11] = -1.0f;
    m[14] = n * q;
}

// The 2D side of the game (HUD, menus, banners, dialogue) is drawn through perspective cameras of a 45 degree field of view authored for 16:9 with
// a near plane under 1 (the 3D scene's are at 64 and up). Measured on the real game: with the display r times wider than 16:9, a layout x (0..1920 at
// 1080 lines) lands on screen at pivot + (x - 960) * r * r * (16/9 / camera aspect) in 1080-line pixels, with the pivot at r * width / 2 instead of
// width / 2: the layout is stretched by r, then again by r through the camera, and sits r - 1 of a half width to the right. A camera aspect of
// 16/9 * r * r / f gives a scale f, and an x offset of r - 1 in the matrix puts the pivot in the middle of the screen: f = 1 is the layout at its own
// proportions in the middle (hud centered), f = r fills the width (hud stretched).
bool is_ui_camera(const float* f, uint32_t flags) {
    (void)flags;
    return near_ratio(f[kCamAspect / 4], kHudAspect) && std::fabs(f[kCamFov / 4] - 0.7854f) < 0.001f && f[kCamNear / 4] < 1.0f;
}

int ui_projection(State& s, float* f, uint32_t flags, uint64_t cam, double disp) {
    const double r = disp / kHudAspect;
    if (r < 1.01) return 0;                                        // 16:9 and narrower: the game's own
    const bool centered = s.hud_centered.load(std::memory_order_relaxed);
    const double scale = centered ? 1.0 : r;
    const double aspect = f[kCamAspect / 4] * r * r / scale;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.ortho_logged < 6) {
            ++s.ortho_logged;
            NXLOG_I(kTag, "2D camera %llx (flags %x, near %.3f, far %.0f): aspect %.4f -> %.4f, x offset %.4f", static_cast<unsigned long long>(cam), flags,
                    f[kCamNear / 4], f[kCamFar / 4], f[kCamAspect / 4], aspect, r - 1.0);
        }
    }
    write_projection(f, flags, f[kCamFov / 4], static_cast<float>(aspect), static_cast<float>(1.0 - 1.0 / r));
    s.calls.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

// The 2D layers drawn through orthographic cameras (near plane under 1: the minimap's markers) are laid out across the full width like the
// perspective ones before the fix: seen at 1080 lines, a layout x lands on screen at x * r. For hud centered the picture is scaled by 1 / r about the
// middle of the screen (the 16:9 layout then sits in the middle at its own proportions). The game's ortho branch (IDA 0x18a1080) with the width
// divided by r in place of the aspect.
int ortho_projection(State& s, float* f, uint32_t flags, double disp) {
    const double r = disp / kHudAspect;
    if (r < 1.01 || !s.hud_centered.load(std::memory_order_relaxed)) return 0;
    if (!near_ratio(f[kCamAspect / 4], kHudAspect) || !(f[kCamNear / 4] < 1.0f) || !(f[kCamFov / 4] > 0.01f && f[kCamFov / 4] < 3.14159f)) return 0;
    const float ex = f[32 / 4] - f[16 / 4], ey = f[36 / 4] - f[20 / 4], ez = f[40 / 4] - f[24 / 4];
    const float dist = std::sqrt(ex * ex + ey * ey + ez * ez);
    const float h = 2.0f * dist * std::tan(f[kCamFov / 4] * 0.5f);
    const bool reversed = (flags & 2) != 0;
    const float a = reversed ? f[kCamFar / 4] : f[kCamNear / 4];
    const float b = reversed ? f[kCamNear / 4] : f[kCamFar / 4];
    const float d = 1.0f / (a - b);
    const float zoom = f[340 / 4];
    float* m = f + kCamProj / 4;
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = static_cast<float>(2.0 / (h * f[kCamAspect / 4] * r));
    m[5] = 2.0f / h;
    m[10] = d;
    m[14] = a * d;
    m[15] = zoom != 1.0f ? 1.0f / zoom : 1.0f;
    s.calls.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

// The camera projection, done here for perspective cameras (the original's perspective branch with the aspect and field of view replaced).
int hook_projection(::Ctx* c, uint64_t, void*) {
    State& s = st();
    const uint64_t cam = c->x[0];
    if (!cam || !mem::is_mapped(cam, kCamFlags + 4)) return 0;
    const uint32_t flags = *reinterpret_cast<const uint32_t*>(cam + kCamFlags);
    float* f = reinterpret_cast<float*>(cam);
    const double disp = s.sz.aspect;
    if (flags & 1) return ortho_projection(s, f, flags, disp);   // orthographic
    if (is_ui_camera(f, flags)) return ui_projection(s, f, flags, cam, disp);
    const double off = s.fov_offset.load(std::memory_order_relaxed);
    float aspect = f[kCamAspect / 4];
    float fov = f[kCamFov / 4];
    const bool authored_16_9 = near_ratio(aspect, kHudAspect) || near_ratio(aspect, disp);
    if (!authored_16_9 && off == 0.0) return 0;
    if (authored_16_9) {
        if (!near_ratio(aspect, disp)) {
            std::lock_guard<std::mutex> lk(s.mu);
            if (s.cam_logged < 8) {
                ++s.cam_logged;
                NXLOG_I(kTag, "camera %llx: aspect %.4f -> %.4f, fov %.4f rad", static_cast<unsigned long long>(cam), aspect, disp, fov);
            }
        }
        aspect = static_cast<float>(disp);
        f[kCamAspect / 4] = aspect;                                 // kept: the game's culling reads the camera's aspect too (inferred)
        // narrower than 16:9: keep the 16:9 width in view, show more above and below (the stored field of view is left alone)
        if (disp < kHudAspect - 0.02 && fov > 0.01f && fov < 3.0f)
            fov = static_cast<float>(2.0 * std::atan(std::tan(fov * 0.5) * (kHudAspect / disp)));
    }
    if (off != 0.0) fov = fov_with_offset(fov, off);
    write_projection(f, flags, fov, aspect, 0.0f);
    s.calls.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

}  // namespace

HwaocSizes hwaoc_sizes(const DisplaySettings& d, int window_w, int window_h) {
    HwaocSizes z;
    double w = std::max(window_w, 1), h = std::max(window_h, 1);
    if (d.aspect > 0) {                                             // a forced aspect: the largest such picture inside the window (letterboxed)
        const double a = std::clamp(d.aspect, kMinAspect, kMaxAspect);
        if (w / h > a) w = h * a;
        else h = w / a;
    }
    z.display_w = even_dim(w);
    z.display_h = even_dim(h);
    internal_size(d, z.display_w, z.display_h, &z.scene_w, &z.scene_h);   // render scale of the display size, or the fixed render size
    z.aspect = d.aspect > 0 ? std::clamp(d.aspect, kMinAspect, kMaxAspect) : static_cast<double>(z.display_w) / z.display_h;
    return z;
}

void display_apply(const Settings& settings) {
    State& s = st();
    s.fov_offset.store(std::clamp(settings.display.fov_offset, -30.0, 40.0));
    s.dynres.store(hwaoc_settings(settings).dynamic_resolution);
    s.hud_centered.store(hwaoc_settings(settings).hud == HudMode::Centered);
}

void display_install(uint64_t module_base, const Settings& settings, int window_w, int window_h) {
    State& s = st();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.installed) return;
        s.base = module_base;
        s.sz = hwaoc_sizes(settings.display, window_w, window_h);
    }
    display_apply(settings);
    const struct {
        uint64_t off;
        SiteHookFn fn;
        bool mid;
    } table[] = {
        {kDeviceCreate, hook_device_create, false}, {kSceneSize, hook_scene_size, true},          {kDynResScaleA, hook_dynres, true},
        {kDynResScaleB, hook_dynres, true},         {kCameraProjection, hook_projection, false},
    };
    std::lock_guard<std::mutex> lk(s.mu);
    for (const auto& t : table) s.hooks.push_back(t.mid ? hook_add_mid(module_base + t.off, t.fn) : hook_add_entry(module_base + t.off, t.fn));
    s.installed = true;
    NXLOG_I(kTag, "display %dx%d, 3D scene %dx%d (window %dx%d, render scale %d%%%s), aspect %.4f, FOV offset %.1f deg, dynamic resolution %s",
            s.sz.display_w, s.sz.display_h, s.sz.scene_w, s.sz.scene_h, window_w, window_h, settings.display.render_scale,
            settings.display.render_width > 0 && settings.display.render_height > 0 ? ", fixed size" : "", s.sz.aspect, settings.display.fov_offset,
            hwaoc_settings(settings).dynamic_resolution ? "on" : "off");
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
    l.render_w = s.sz.display_w;
    l.render_h = s.sz.display_h;
    l.aspect = s.sz.aspect;
    l.fov_offset = s.fov_offset.load();
    return l;
}

uint32_t display_hook_calls() { return st().calls.load(); }

}  // namespace nxrt::play
