// SPDX-License-Identifier: GPL-3.0-or-later
// nvnQueuePresentTexture: the window texture is drawn into the Renderer's frame (swapchain or headless image) and presented.
// The shell's FrameLimiter, when given, paces the frame (begin_frame before the image is acquired).
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/ftrace.h"
#include "nxrt/log.h"
#include "nvn_gpu_internal.h"

namespace nxrt::nvn::be::gpu {

namespace {
struct CaptureEnd {
    ~CaptureEnd() {
        trace_dump_targets();
    }
};

// [nvn] shot_frames = "300,1000:2000:50" (NXRT_NVN_SHOT_FRAMES): the presented frames (counted from 1 like the trace) whose picture is written as
// <shot_dir>/<number, 6 digits>.png (default shot_dir "shots"). A range is from:to:step. Lets a run be compared frame by frame against a reference capture.
struct ShotSpec { int64_t from, to, step; };
std::vector<ShotSpec> g_shots;
bool g_shots_read = false;
std::string g_shot_dir;

void read_shots() {
    g_shots_read = true;
    g_shot_dir = nxrt::config_get_str("nvn", "shot_dir", "shots");
    const std::string spec = nxrt::config_get_str("nvn", "shot_frames", "");
    size_t i = 0;
    while (i < spec.size()) {
        size_t j = spec.find(',', i);
        if (j == std::string::npos) j = spec.size();
        const std::string item = spec.substr(i, j - i);
        long long a = 0, b = 0, c = 1;
        const int n = std::sscanf(item.c_str(), "%lld:%lld:%lld", &a, &b, &c);
        if (n == 1) g_shots.push_back({a, a, 1});
        else if (n >= 2 && c > 0) g_shots.push_back({a, b, c});
        i = j + 1;
    }
}

bool shot_wanted(int64_t n) {
    if (!g_shots_read) read_shots();
    for (const ShotSpec& s : g_shots)
        if (n >= s.from && n <= s.to && (n - s.from) % s.step == 0) return true;
    return false;
}
}  // namespace

void present_on_render_thread(Window* w, int index) {
    NXFT("present");
    CaptureEnd capture_end;
    gfx::Renderer* rend = renderer();
    PresentPass* pp = present_pass();
    if (!w || index < 0 || static_cast<size_t>(index) >= w->textures.size()) return;
    Texture* t = w->textures[index].get();
    if (!t) return;
    counters().presents.fetch_add(1, std::memory_order_relaxed);
    diag_frame_tick();                       // the freeze watchdog counts presented frames
    if (!rend || !pp) return;
    if (!tex_gpu(t)) return;
    if (FrameLimiter* lim = limiter()) { NXFT("limiter"); lim->begin_frame(); }
    {
        const int64_t n = static_cast<int64_t>(counters().presents.load());
        if (shot_wanted(n)) {
            char name[64];
            std::snprintf(name, sizeof name, "/%06lld.png", static_cast<long long>(n));
            rend->request_screenshot(g_shot_dir + name);
        }
    }
    gfx::FrameInfo fi;
    { NXFT("present.acquire"); if (!rend->begin_frame(&fi)) return; }
    ft::frame();
    Frame f;
    f.cb = fi.cmd;
    f.serial = current_serial();
    tex_transition(t, f, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkImageView src = tex_present_view(t);
    const VkExtent3D e = tex_extent(t, 0);
    int32_t crop[4] = {w->crop[0], w->crop[1], w->crop[2], w->crop[3]};
    pp->set_flip_y(window_origin_mode() == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT);   // the game runs with the lower-left origin (mode 0)
    pp->record(fi.cmd, fi.view, fi.extent, src, VkExtent2D{e.width, e.height}, crop, keep_aspect());
    { NXFT("present.end"); rend->end_frame(); }
}

}  // namespace nxrt::nvn::be::gpu
