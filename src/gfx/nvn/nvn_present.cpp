// SPDX-License-Identifier: GPL-3.0-or-later
// nvnQueuePresentTexture: the window texture is drawn into the Renderer's frame (swapchain or headless image) and presented.
// The shell's FrameLimiter, when given, paces the frame (begin_frame before the image is acquired).
#include "nxrt/frame_limiter.h"
#include "nxrt/log.h"
#include "nvn_gpu_internal.h"

namespace nxrt::nvn::be::gpu {

void present_on_render_thread(Window* w, int index) {
    gfx::Renderer* rend = renderer();
    PresentPass* pp = present_pass();
    if (!w || index < 0 || static_cast<size_t>(index) >= w->textures.size()) return;
    Texture* t = w->textures[index].get();
    if (!t) return;
    counters().presents.fetch_add(1, std::memory_order_relaxed);
    if (!rend || !pp) return;
    if (!tex_gpu(t)) return;
    if (FrameLimiter* lim = limiter()) lim->begin_frame();
    gfx::FrameInfo fi;
    if (!rend->begin_frame(&fi)) return;
    Frame f;
    f.cb = fi.cmd;
    f.serial = current_serial();
    tex_transition(t, f, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkImageView src = tex_present_view(t);
    const VkExtent3D e = tex_extent(t, 0);
    int32_t crop[4] = {w->crop[0], w->crop[1], w->crop[2], w->crop[3]};
    pp->record(fi.cmd, fi.view, fi.extent, src, VkExtent2D{e.width, e.height}, crop, keep_aspect());
    rend->end_frame();
}

}  // namespace nxrt::nvn::be::gpu
