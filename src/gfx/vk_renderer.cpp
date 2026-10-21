// SPDX-License-Identifier: GPL-3.0-or-later
// Renderer (declarations in include/nxrt/gfx.h): swapchain or offscreen image ring, present modes, frames in flight, GPU
// timestamps, resize and minimise handling, screenshots, overlay hook.
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_surface.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "nxrt/config.h"
#include "nxrt/gfx.h"
#include "nxrt/log.h"
#include "nxrt/platform.h"

namespace nxrt::gfx {
namespace {
constexpr const char* kTag = "gfx";

struct ImageState {   // a barrier helper description
    VkImageLayout layout;
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
};

void barrier(VkCommandBuffer cmd, VkImage img, const ImageState& from, const ImageState& to) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = from.stage;
    b.srcAccessMask = from.access;
    b.dstStageMask = to.stage;
    b.dstAccessMask = to.access;
    b.oldLayout = from.layout;
    b.newLayout = to.layout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

const ImageState kUndefined = {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0};
const ImageState kColorAtt = {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT};
const ImageState kTransferSrc = {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
const ImageState kPresent = {VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_NONE, 0};

}  // namespace

// ---- names ------------------------------------------------------------------------------------------------------------
const char* present_mode_name(PresentMode m) {
    switch (m) {
        case PresentMode::Fifo: return "fifo";
        case PresentMode::FifoRelaxed: return "fifo_relaxed";
        case PresentMode::Mailbox: return "mailbox";
        case PresentMode::Immediate: return "immediate";
    }
    return "?";
}

bool parse_present_mode(const char* s, PresentMode* out) {
    if (!s) return false;
    std::string v;
    for (const char* p = s; *p; ++p) v += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    for (PresentMode m : {PresentMode::Fifo, PresentMode::FifoRelaxed, PresentMode::Mailbox, PresentMode::Immediate})
        if (v == present_mode_name(m)) { *out = m; return true; }
    return false;
}

VkPresentModeKHR to_vk(PresentMode m) {
    switch (m) {
        case PresentMode::Fifo: return VK_PRESENT_MODE_FIFO_KHR;
        case PresentMode::FifoRelaxed: return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
        case PresentMode::Mailbox: return VK_PRESENT_MODE_MAILBOX_KHR;
        case PresentMode::Immediate: return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

// ---- PNG helpers ------------------------------------------------------------------------------------------------------
bool save_png_rgba(const std::string& path, const uint8_t* rgba, uint32_t w, uint32_t h) {
    SDL_Surface* s = SDL_CreateSurfaceFrom(static_cast<int>(w), static_cast<int>(h), SDL_PIXELFORMAT_RGBA32, const_cast<uint8_t*>(rgba),
                                           static_cast<int>(w * 4));
    if (!s) return false;
    const bool ok = SDL_SavePNG(s, path.c_str());
    SDL_DestroySurface(s);
    if (!ok) NXLOG_E(kTag, "could not write %s: %s", path.c_str(), SDL_GetError());
    return ok;
}

bool load_png_rgba(const std::string& path, std::vector<uint8_t>* rgba, uint32_t* w, uint32_t* h) {
    SDL_Surface* s = SDL_LoadPNG(path.c_str());
    if (!s) return false;
    SDL_Surface* c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(s);
    if (!c) return false;
    *w = static_cast<uint32_t>(c->w);
    *h = static_cast<uint32_t>(c->h);
    rgba->resize(static_cast<size_t>(c->w) * c->h * 4);
    for (int y = 0; y < c->h; ++y) std::memcpy(rgba->data() + static_cast<size_t>(y) * c->w * 4, static_cast<uint8_t*>(c->pixels) + static_cast<size_t>(y) * c->pitch, static_cast<size_t>(c->w) * 4);
    SDL_DestroySurface(c);
    return true;
}

ImageDiff compare_rgba(const uint8_t* a, uint32_t aw, uint32_t ah, const uint8_t* b, uint32_t bw, uint32_t bh) {
    ImageDiff d;
    d.width = aw;
    d.height = ah;
    d.same_size = aw == bw && ah == bh;
    if (!d.same_size) return d;
    const size_t px = static_cast<size_t>(aw) * ah;
    double sum = 0;
    size_t over = 0;
    for (size_t i = 0; i < px; ++i) {
        int worst = 0;
        for (int c = 0; c < 3; ++c) {
            const int df = std::abs(static_cast<int>(a[i * 4 + c]) - static_cast<int>(b[i * 4 + c]));
            sum += df;
            worst = std::max(worst, df);
        }
        d.max_abs = std::max(d.max_abs, worst);
        if (worst > 8) ++over;
    }
    d.mean_abs = px ? sum / (3.0 * static_cast<double>(px)) : 0;
    d.fraction_over_8 = px ? static_cast<double>(over) / static_cast<double>(px) : 0;
    return d;
}

// ---- renderer ---------------------------------------------------------------------------------------------------------
struct Renderer::Impl {
    Device* dev = nullptr;
    RendererDesc desc;
    bool headless = false;

    PresentMode requested = PresentMode::Fifo, active = PresentMode::Fifo;
    std::vector<VkPresentModeKHR> supported_modes;
    bool capture_supported = true;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent = {0, 0};
    uint32_t headless_w = 0, headless_h = 0;
    struct Target {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSemaphore render_done = VK_NULL_HANDLE;
        Image owned;    // headless images
    };
    std::vector<Target> targets;
    bool dirty = true;
    int last_px_w = -1, last_px_h = -1;

    struct Slot {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquire = VK_NULL_HANDLE;
        VkQueryPool queries = VK_NULL_HANDLE;
        bool ts_pending = false;
    };
    std::vector<Slot> slots;
    uint32_t cur_slot = 0, cur_image = 0;
    uint64_t frame_number = 0;
    bool in_frame = false;
    FrameInfo info;

    std::function<void(const FrameInfo&)> overlay;

    // pending capture
    bool cap_pending = false;
    std::string cap_path;
    std::vector<uint8_t>* cap_out = nullptr;
    uint32_t* cap_w = nullptr;
    uint32_t* cap_h = nullptr;

    RendererStats st;

    VkDevice vk() const { return dev->device(); }

    // -- present mode resolution
    PresentMode resolve(PresentMode want) const {
        if (headless) return want;
        auto has = [&](PresentMode m) { return std::find(supported_modes.begin(), supported_modes.end(), to_vk(m)) != supported_modes.end(); };
        if (has(want)) return want;
        switch (want) {
            case PresentMode::Mailbox: if (has(PresentMode::Immediate)) return PresentMode::Immediate; break;
            case PresentMode::Immediate: if (has(PresentMode::Mailbox)) return PresentMode::Mailbox; break;
            default: break;
        }
        return PresentMode::Fifo;
    }

    void destroy_targets() {
        for (auto& t : targets) {
            if (t.render_done) vkDestroySemaphore(vk(), t.render_done, nullptr);
            if (headless) {
                dev->destroy_image(t.owned);
            } else if (t.view) {
                vkDestroyImageView(vk(), t.view, nullptr);
            }
        }
        targets.clear();
    }

    bool create_semaphore(VkSemaphore* s) {
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        return vkCreateSemaphore(vk(), &si, nullptr, s) == VK_SUCCESS;
    }

    bool recreate(uint32_t want_w, uint32_t want_h) {
        dev->wait_idle();
        if (headless) {
            destroy_targets();
            extent = {want_w, want_h};
            format = VK_FORMAT_B8G8R8A8_UNORM;
            const uint32_t count = desc.frames_in_flight + 1;
            for (uint32_t i = 0; i < count; ++i) {
                ImageDesc id;
                id.format = format;
                id.width = want_w;
                id.height = want_h;
                id.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                id.name = "headless color";
                Target t;
                std::string why;
                if (dev->create_image(id, &t.owned, &why) != VK_SUCCESS) {
                    NXLOG_E(kTag, "offscreen image: %s", why.c_str());
                    return false;
                }
                t.image = t.owned.image;
                t.view = t.owned.view;
                targets.push_back(t);
            }
            active = requested;
            dirty = false;
            st.swapchain_creates++;
            NXLOG_I(kTag, "offscreen targets: %u x %ux%u", count, want_w, want_h);
            return true;
        }

        VkPhysicalDevice pd = dev->physical();
        VkSurfaceKHR surf = dev->surface();
        VkSurfaceCapabilitiesKHR caps;
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps) != VK_SUCCESS) return false;
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &n, nullptr);
        supported_modes.assign(n, VK_PRESENT_MODE_FIFO_KHR);
        vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &n, supported_modes.data());
        active = resolve(requested);

        vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(n);
        vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &n, fmts.data());
        VkSurfaceFormatKHR chosen = fmts.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : fmts[0];
        for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
            bool found = false;
            for (auto& f : fmts) if (f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; found = true; break; }
            if (found) break;
        }
        VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        capture_supported = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
        if (capture_supported) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        {   // the swapchain format goes through the same check as every image
            ImageDesc id;
            id.format = chosen.format;
            id.usage = usage;
            std::string why;
            if (!dev->image_supported(id, &why)) {
                NXLOG_E(kTag, "swapchain format refused: %s", why.c_str());
                return false;
            }
        }

        VkExtent2D ext = caps.currentExtent;
        if (ext.width == 0xFFFFFFFFu) {
            ext.width = std::clamp(want_w, caps.minImageExtent.width, caps.maxImageExtent.width);
            ext.height = std::clamp(want_h, caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        if (ext.width == 0 || ext.height == 0) { extent = {0, 0}; return false; }

        uint32_t images = std::max(caps.minImageCount + (active == PresentMode::Mailbox ? 1u : 0u), desc.frames_in_flight + 1);
        if (caps.maxImageCount) images = std::min(images, caps.maxImageCount);

        VkSwapchainKHR old = swapchain;
        VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface = surf;
        ci.minImageCount = images;
        ci.imageFormat = chosen.format;
        ci.imageColorSpace = chosen.colorSpace;
        ci.imageExtent = ext;
        ci.imageArrayLayers = 1;
        ci.imageUsage = usage;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = caps.currentTransform;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode = to_vk(active);
        ci.clipped = VK_TRUE;
        ci.oldSwapchain = old;
        VkSwapchainKHR fresh = VK_NULL_HANDLE;
        VkResult r = vkCreateSwapchainKHR(vk(), &ci, nullptr, &fresh);
        destroy_targets();
        if (old) vkDestroySwapchainKHR(vk(), old, nullptr);
        swapchain = VK_NULL_HANDLE;
        if (r != VK_SUCCESS) {
            NXLOG_E(kTag, "vkCreateSwapchainKHR failed (%d)", static_cast<int>(r));
            return false;
        }
        swapchain = fresh;
        format = chosen.format;
        extent = ext;
        uint32_t count = 0;
        vkGetSwapchainImagesKHR(vk(), swapchain, &count, nullptr);
        std::vector<VkImage> imgs(count);
        vkGetSwapchainImagesKHR(vk(), swapchain, &count, imgs.data());
        for (VkImage im : imgs) {
            Target t;
            t.image = im;
            VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = im;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (vkCreateImageView(vk(), &vi, nullptr, &t.view) != VK_SUCCESS || !create_semaphore(&t.render_done)) return false;
            targets.push_back(t);
        }
        dirty = false;
        st.swapchain_creates++;
        NXLOG_I(kTag, "swapchain %ux%u, %u images, format %d, present mode %s%s", ext.width, ext.height, count, static_cast<int>(format),
                present_mode_name(active), active != requested ? " (requested mode not available)" : "");
        return true;
    }

    void read_timestamps(Slot& s) {
        if (!s.ts_pending) return;
        s.ts_pending = false;
        uint64_t ts[2] = {};
        if (vkGetQueryPoolResults(vk(), s.queries, 0, 2, sizeof ts, ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return;
        const uint32_t bits = 64;   // timestampValidBits is queried by the device; assume a full counter and mask below
        (void)bits;
        const double ms = static_cast<double>(ts[1] - ts[0]) * static_cast<double>(dev->timestamp_period_ns()) * 1e-6;
        if (ms < 0 || ms > 1000.0) return;
        st.gpu_ms_last = ms;
        st.gpu_ms_avg = st.gpu_ms_avg == 0 ? ms : st.gpu_ms_avg * 0.95 + ms * 0.05;
    }

    bool create_slots() {
        for (uint32_t i = 0; i < desc.frames_in_flight; ++i) {
            Slot s;
            VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pi.queueFamilyIndex = dev->queue_family();
            if (vkCreateCommandPool(vk(), &pi, nullptr, &s.pool) != VK_SUCCESS) return false;
            VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = s.pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(vk(), &ai, &s.cmd) != VK_SUCCESS) return false;
            VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (vkCreateFence(vk(), &fi, nullptr, &s.fence) != VK_SUCCESS) return false;
            if (!create_semaphore(&s.acquire)) return false;
            if (dev->timestamps_supported()) {
                VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
                qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
                qi.queryCount = 2;
                if (vkCreateQueryPool(vk(), &qi, nullptr, &s.queries) != VK_SUCCESS) return false;
            }
            slots.push_back(s);
        }
        return true;
    }

    void destroy_all() {
        dev->wait_idle();
        for (auto& s : slots) {
            if (s.queries) vkDestroyQueryPool(vk(), s.queries, nullptr);
            if (s.acquire) vkDestroySemaphore(vk(), s.acquire, nullptr);
            if (s.fence) vkDestroyFence(vk(), s.fence, nullptr);
            if (s.pool) vkDestroyCommandPool(vk(), s.pool, nullptr);   // frees its command buffer
        }
        slots.clear();
        destroy_targets();
        if (swapchain) vkDestroySwapchainKHR(vk(), swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }
};

std::unique_ptr<Renderer> Renderer::create(Device& dev, const RendererDesc& desc_in, std::string* error) {
    std::unique_ptr<Renderer> r(new Renderer());
    r->impl_ = new Impl();
    Impl& s = *r->impl_;
    s.dev = &dev;
    s.desc = desc_in;
    s.desc.frames_in_flight = static_cast<uint32_t>(std::clamp<int64_t>(config_get_int("gfx", "frames_in_flight", desc_in.frames_in_flight), 1, 3));
    PresentMode pm = desc_in.present_mode;
    const std::string cfg_pm = config_get_str("gfx", "present_mode", "");
    if (!cfg_pm.empty() && !parse_present_mode(cfg_pm.c_str(), &pm)) NXLOG_W(kTag, "unknown [gfx] present_mode '%s'", cfg_pm.c_str());
    s.requested = pm;
    s.headless = dev.headless();
    s.headless_w = desc_in.headless_width;
    s.headless_h = desc_in.headless_height;
    if (!s.create_slots()) {
        if (error) *error = "could not create the frame slots";
        return nullptr;
    }
    int pw = static_cast<int>(s.headless_w), ph = static_cast<int>(s.headless_h);
    if (!s.headless) platform::pixel_size(&pw, &ph);
    if (!s.headless && (pw == 0 || ph == 0)) {
        s.dirty = true;   // created while minimised: the first begin_frame builds the swapchain
    } else if (!s.recreate(static_cast<uint32_t>(pw), static_cast<uint32_t>(ph))) {
        if (error) *error = "could not create the swapchain";
        return nullptr;
    }
    s.last_px_w = pw;
    s.last_px_h = ph;
    return r;
}

Renderer::~Renderer() {
    if (impl_) {
        impl_->destroy_all();
        delete impl_;
    }
}

bool Renderer::begin_frame(FrameInfo* out) {
    Impl& s = *impl_;
    if (s.in_frame) end_frame();   // a forgotten end_frame must not wedge the slot
    int pw = static_cast<int>(s.headless_w), ph = static_cast<int>(s.headless_h);
    if (!s.headless) platform::pixel_size(&pw, &ph);
    if (!s.headless && (pw == 0 || ph == 0)) {
        s.st.skipped++;
        return false;
    }
    if (pw != s.last_px_w || ph != s.last_px_h) {
        s.dirty = true;
        s.last_px_w = pw;
        s.last_px_h = ph;
    }
    if (s.dirty && !s.recreate(static_cast<uint32_t>(pw), static_cast<uint32_t>(ph))) {
        s.st.skipped++;
        return false;
    }
    if (s.extent.width == 0 || s.extent.height == 0) {
        s.st.skipped++;
        return false;
    }

    Impl::Slot& slot = s.slots[s.cur_slot];
    vkWaitForFences(s.vk(), 1, &slot.fence, VK_TRUE, UINT64_MAX);
    s.read_timestamps(slot);

    uint32_t idx = 0;
    if (s.headless) {
        idx = static_cast<uint32_t>(s.frame_number % s.targets.size());
    } else {
        VkResult r = vkAcquireNextImageKHR(s.vk(), s.swapchain, UINT64_MAX, slot.acquire, VK_NULL_HANDLE, &idx);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) {
            s.dirty = true;
            s.st.out_of_date++;
            s.st.skipped++;
            return false;
        }
        if (r == VK_SUBOPTIMAL_KHR) {
            s.dirty = true;     // finish this frame, rebuild before the next
            s.st.out_of_date++;
        } else if (r != VK_SUCCESS) {
            NXLOG_E(kTag, "vkAcquireNextImageKHR failed (%d)", static_cast<int>(r));
            s.st.skipped++;
            return false;
        }
    }
    vkResetFences(s.vk(), 1, &slot.fence);
    vkResetCommandPool(s.vk(), slot.pool, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(slot.cmd, &bi);
    if (slot.queries) {
        vkCmdResetQueryPool(slot.cmd, slot.queries, 0, 2);
        vkCmdWriteTimestamp2(slot.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, slot.queries, 0);
    }
    Impl::Target& t = s.targets[idx];
    barrier(slot.cmd, t.image, kUndefined, kColorAtt);

    s.cur_image = idx;
    s.info = FrameInfo{slot.cmd, t.image, t.view, s.format, s.extent, s.cur_slot, idx, s.frame_number};
    s.in_frame = true;
    if (out) *out = s.info;
    return true;
}

void Renderer::end_frame() {
    Impl& s = *impl_;
    if (!s.in_frame) return;
    s.in_frame = false;
    Impl::Slot& slot = s.slots[s.cur_slot];
    Impl::Target& t = s.targets[s.cur_image];
    if (s.overlay) s.overlay(s.info);

    const bool capturing = s.cap_pending && (s.headless || s.capture_supported);
    Buffer cap;
    const uint32_t w = s.extent.width, h = s.extent.height;
    if (capturing) {
        if (s.dev->create_buffer(static_cast<VkDeviceSize>(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUse::Readback, &cap, "screenshot") != VK_SUCCESS) {
            NXLOG_E(kTag, "screenshot buffer allocation failed");
        }
    }
    if (cap) {
        barrier(slot.cmd, t.image, kColorAtt, kTransferSrc);
        VkBufferImageCopy2 region = {VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {w, h, 1};
        VkCopyImageToBufferInfo2 ci = {VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2};
        ci.srcImage = t.image;
        ci.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        ci.dstBuffer = cap.buffer;
        ci.regionCount = 1;
        ci.pRegions = &region;
        vkCmdCopyImageToBuffer2(slot.cmd, &ci);
        VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(slot.cmd, &di);
        if (!s.headless) barrier(slot.cmd, t.image, kTransferSrc, kPresent);
    } else if (!s.headless) {
        barrier(slot.cmd, t.image, kColorAtt, kPresent);
    }
    if (slot.queries) {
        vkCmdWriteTimestamp2(slot.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, slot.queries, 1);
        slot.ts_pending = true;
    }
    vkEndCommandBuffer(slot.cmd);

    VkSemaphoreSubmitInfo wait = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = slot.acquire;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = t.render_done;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cb = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cb.commandBuffer = slot.cmd;
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cb;
    if (!s.headless) {
        si.waitSemaphoreInfoCount = 1;
        si.pWaitSemaphoreInfos = &wait;
        si.signalSemaphoreInfoCount = 1;
        si.pSignalSemaphoreInfos = &signal;
    }
    {
        std::lock_guard<std::mutex> lk(s.dev->queue_mutex());
        VkResult r = vkQueueSubmit2(s.dev->queue(), 1, &si, slot.fence);
        if (r != VK_SUCCESS) NXLOG_E(kTag, "vkQueueSubmit2 failed (%d)", static_cast<int>(r));
        if (!s.headless) {
            VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &t.render_done;
            pi.swapchainCount = 1;
            pi.pSwapchains = &s.swapchain;
            pi.pImageIndices = &s.cur_image;
            r = vkQueuePresentKHR(s.dev->queue(), &pi);
            s.st.presents++;
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_SURFACE_LOST_KHR) {
                s.dirty = true;
                s.st.out_of_date++;
            } else if (r != VK_SUCCESS) {
                NXLOG_E(kTag, "vkQueuePresentKHR failed (%d)", static_cast<int>(r));
            }
        }
    }
    s.st.frames++;
    s.frame_number++;
    s.cur_slot = (s.cur_slot + 1) % static_cast<uint32_t>(s.slots.size());

    if (cap) {
        vkWaitForFences(s.vk(), 1, &slot.fence, VK_TRUE, UINT64_MAX);
        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
        s.dev->invalidate_buffer(cap);
        std::memcpy(px.data(), cap.mapped, px.size());
        s.dev->destroy_buffer(cap);
        if (s.format == VK_FORMAT_B8G8R8A8_UNORM || s.format == VK_FORMAT_B8G8R8A8_SRGB)
            for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        if (!s.cap_path.empty() && save_png_rgba(s.cap_path, px.data(), w, h)) NXLOG_I(kTag, "screenshot %ux%u written to %s", w, h, s.cap_path.c_str());
        if (s.cap_out) *s.cap_out = std::move(px);
        if (s.cap_w) *s.cap_w = w;
        if (s.cap_h) *s.cap_h = h;
        s.cap_pending = false;
        s.cap_path.clear();
        s.cap_out = nullptr;
        s.cap_w = s.cap_h = nullptr;
        s.st.screenshots++;
    }
}

void Renderer::set_overlay_hook(std::function<void(const FrameInfo&)> hook) { impl_->overlay = std::move(hook); }

bool Renderer::set_present_mode(PresentMode m) {
    Impl& s = *impl_;
    s.requested = m;
    int pw = static_cast<int>(s.headless_w), ph = static_cast<int>(s.headless_h);
    if (!s.headless) platform::pixel_size(&pw, &ph);
    if (!s.headless && (pw == 0 || ph == 0)) {   // minimised: applied when the window is back
        s.dirty = true;
        return true;
    }
    return s.recreate(static_cast<uint32_t>(pw), static_cast<uint32_t>(ph));
}

PresentMode Renderer::requested_present_mode() const { return impl_->requested; }
PresentMode Renderer::present_mode() const { return impl_->active; }

bool Renderer::present_mode_supported(PresentMode m) const {
    if (impl_->headless) return true;
    return std::find(impl_->supported_modes.begin(), impl_->supported_modes.end(), to_vk(m)) != impl_->supported_modes.end();
}

bool Renderer::resize_headless(uint32_t w, uint32_t h) {
    Impl& s = *impl_;
    if (!s.headless || !w || !h) return false;
    s.headless_w = w;
    s.headless_h = h;
    s.dirty = true;
    return true;
}

VkExtent2D Renderer::extent() const { return impl_->extent; }
VkFormat Renderer::format() const { return impl_->format; }
uint32_t Renderer::image_count() const { return static_cast<uint32_t>(impl_->targets.size()); }
uint32_t Renderer::frames_in_flight() const { return impl_->desc.frames_in_flight; }
bool Renderer::headless() const { return impl_->headless; }

void Renderer::request_screenshot(const std::string& png_path) {
    impl_->cap_pending = true;
    impl_->cap_path = png_path;
}

void Renderer::capture_next_frame(std::vector<uint8_t>* rgba, uint32_t* width, uint32_t* height) {
    impl_->cap_pending = true;
    impl_->cap_out = rgba;
    impl_->cap_w = width;
    impl_->cap_h = height;
}

RendererStats Renderer::stats() const { return impl_->st; }
void Renderer::wait_idle() { impl_->dev->wait_idle(); }

}  // namespace nxrt::gfx
