// SPDX-License-Identifier: GPL-3.0-or-later
// nvnQueuePresentTexture: the window texture is drawn into the Renderer's frame (swapchain or headless image) and presented.
// The shell's FrameLimiter, when given, paces the frame (begin_frame before the image is acquired).
#include "nxrt/diag.h"
#include "nxrt/extras.h"
#include "nxrt/lowlat.h"
#include "extras/output_pass.h"
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
    // HDR scene / HDR swapchain / FSR 1 go through the output pass (S14); everything else is the plain present pass of the SDR path
    const extras::HdrState hs = extras::hdr_state();
    extras::OutputPass* op = output_pass();
    if (op && (hs.scene_hdr || hs.output != extras::OutputKind::Sdr8 || extras::upscale_mode() == extras::UpscaleMode::Fsr1)) {
        extras::OutputParams prm;
        prm.src = src;
        prm.src_extent = VkExtent2D{e.width, e.height};
        for (int i = 0; i < 4; ++i) prm.crop[i] = crop[i];
        prm.keep_aspect = keep_aspect();
        prm.flip_y = window_origin_mode() == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT;
        prm.src_linear = hs.scene_hdr;
        prm.out = hs.output;
        prm.paper_white_nits = hs.paper_white_nits;
        prm.peak_nits = hs.peak_nits;
        prm.fsr1 = extras::upscale_mode() == extras::UpscaleMode::Fsr1;
        prm.sharpness = extras::upscale_sharpness();
        op->record(fi.cmd, fi.view, rend->format(), fi.extent, prm);
    } else {
        pp->record(fi.cmd, fi.view, fi.extent, src, VkExtent2D{e.width, e.height}, crop, keep_aspect());
    }
    { NXFT("present.end"); rend->end_frame(); }
}

}  // namespace nxrt::nvn::be::gpu
