// SPDX-License-Identifier: GPL-3.0-or-later
// Internal interfaces of the Vulkan side of the NVN back end (render thread only). Modules:
//   nvn_gpu.cpp          the Gpu singleton: device access, render thread, submission slots, timeline, deferred destruction, uploads
//   nvn_gpu_pool.cpp     memory pools imported as Vulkan memory, GPU address -> VkBuffer
//   nvn_gpu_texture.cpp  texture images, uploads, views, samplers, layout tracking
//   nvn_replay.cpp       command list interpreter (state, passes, draws, clears, copies)
//   nvn_pipeline.cpp     pipeline layouts, graphics/compute pipelines, descriptor sets
//   nvn_present.cpp      window texture -> swapchain
// Everything declared here must be called on the render thread (gpu::on_render_thread()) unless it says otherwise.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <volk.h>

#include "nxrt/gfx.h"
#include "nvn_present_pass.h"

namespace nxrt::extras { class OutputPass; }
#include "nvn_core.h"
#include "nvn_gpu.h"

namespace nxrt {
class FrameLimiter;
}

namespace nxrt::nvn::be::gpu {

bool on_render_thread();
void flush_caches();              // writes the pipeline cache to disk now (the key file is appended as pipelines are created)
gfx::Device& device();
VkDevice vkdev();

struct Counters {
    std::atomic<uint64_t> submits{0}, lists_replayed{0}, draws{0}, draws_skipped{0}, dispatches{0}, clears{0}, copies{0}, presents{0};
    std::atomic<uint64_t> passes{0}, pass_breaks{0}, pipelines_created{0}, pipeline_cache_hits{0}, descriptor_sets{0};
    std::atomic<uint64_t> textures_created{0}, texture_uploads{0}, texture_upload_bytes{0}, bc7_transcodes{0}, textures_failed{0};
    std::atomic<uint64_t> pools_imported{0}, unresolved_addresses{0}, unsupported_ops{0}, submit_replay_ns{0};
};
Counters& counters();
// An operation the back end does not implement was skipped: logged the first time per name, counted for the report.
void note_unsupported(const char* what);
std::string unsupported_report();

// Runs fn on the render thread and waits for it. From the render thread itself it just calls fn.
void run_on_render_thread(const std::function<void()>& fn);
// Test and one-shot hook: runs fn(Frame&) on the render thread with a fresh command buffer, submits it and waits for the GPU.
// Returns the serial of the submission.
uint64_t run_frame_sync(const std::function<void(struct Frame&)>& fn);
// Brings the Vulkan layer up on a device without touching the NVN entry points (tests of nvn_gpu_*.cpp).
bool init_for_test(gfx::Device& dev, gfx::Renderer* renderer = nullptr);
void shutdown_for_test();

// ---- the recording context of one submission ---------------------------------------------------------------------------
struct UploadAlloc {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    uint8_t* ptr = nullptr;       // host pointer to the allocation (host visible, coherent)
    VkDeviceSize size = 0;
    explicit operator bool() const { return buffer != VK_NULL_HANDLE; }
};

struct Frame {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    uint64_t serial = 0;          // the timeline value this submission signals when the GPU has run it
    void* slot = nullptr;         // nvn_gpu.cpp: the submission slot (command pool, descriptor pools, upload memory)
    // Host-visible memory valid until the submission has completed: small per-draw data (support buffer, driver constants,
    // inline uniform data). align is a power of two.
    UploadAlloc upload(VkDeviceSize size, VkDeviceSize align);
    // A staging buffer for texture data (own buffer when large). Freed when the submission has completed.
    UploadAlloc staging(VkDeviceSize size);
    // Runs fn on the render thread after the submission has completed on the GPU (and, for objects the replay used, no
    // later submission can still be using them if they were last used in this one).
    void defer(std::function<void()> fn);
    // A descriptor set of this submission's pool (freed when the slot is reused). Null on failure.
    VkDescriptorSet alloc_set(VkDescriptorSetLayout layout);
};

// Destruction: fn runs once every submission made so far (and the one being recorded) has completed.
void defer_destroy(std::function<void()> fn);
uint64_t completed_serial();      // cached timeline value; call poll_completed() for a fresh read
uint64_t poll_completed();
uint64_t current_serial();        // the serial of the submission being recorded (or the last one)

// ---- barriers ---------------------------------------------------------------------------------------------------------------
void image_barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to, VkImageAspectFlags aspect,
                   uint32_t base_level, uint32_t level_count, uint32_t base_layer, uint32_t layer_count);
// All commands before, all commands after, all memory (used after passes, dispatches, copies and fills).
void memory_barrier(VkCommandBuffer cb);

// ---- pools ---------------------------------------------------------------------------------------------------------------
struct BufRef {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize avail = 0;       // bytes from offset to the end of the pool buffer
    bool ok = false;
};
// The buffer and offset of `size` bytes at a GPU address. When the address is in no pool (or size runs past the pool), ok is
// false and the buffer is the dummy buffer (a zeroed 64 KiB buffer) so the draw can go on; this is logged once per address.
BufRef resolve_buffer(uint64_t gpu_address, uint64_t size);
VkBuffer dummy_buffer();
// The last 32 bytes of the dummy buffer hold the default of an unset vertex attribute: float (0,0,0,1) and integer (0,0,0,1).
constexpr VkDeviceSize kDummyAttrFloatOffset = 65504, kDummyAttrIntOffset = 65520;
uint64_t pool_imports();          // statistics
// Called from destroy_pool_gpu on the render thread.
void pool_gpu_destroy(PoolGpu* p);
void pool_shutdown();             // destroys every imported pool still alive and the dummy buffer (device idle)

// ---- textures --------------------------------------------------------------------------------------------------------------
// What a binding site needs to see of a texture.
struct ViewRequest {
    VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;     // the shader's image dimension (from the SPIR-V binding)
    bool depth_compare = false;
    uint32_t base_level = 0, level_count = 0;         // 0 = all levels from base
    uint32_t base_layer = 0, layer_count = 0;         // 0 = all layers from base
    VkFormat format = VK_FORMAT_UNDEFINED;            // UNDEFINED: the image's own format
    VkComponentMapping swizzle = {};                  // identity when all IDENTITY
    bool depth_aspect = true;                         // depth/stencil images: sample depth (true) or stencil (false)
    uint8_t component = 0;                            // the shader's sampled type: 0 unknown, 1 float, 2 sint, 3 uint (raw-bit aliases of 32-bit formats)
};
// Creates the VkImage if needed and returns it (null when the format cannot be created: logged once; callers then bind
// the dummy). Does not upload.
TexGpu* tex_gpu(Texture* t);
// Records an upload when the texture's memory was not yet uploaded or the CPU wrote it since (and the GPU did not render it).
// Must be called with no render pass open on `f.cb`. Leaves the image in SHADER_READ_ONLY_OPTIMAL after an upload.
void tex_ensure_current(Texture* t, Frame& f);
// Moves the whole image to `layout` (no-op if already there). Call with no render pass open.
void tex_transition(Texture* t, Frame& f, VkImageLayout layout);
VkImageLayout tex_layout(Texture* t);
// The texture has been (or is about to be) written by the GPU: no more uploads from memory until the CPU writes it.
void tex_mark_gpu_written(Texture* t);
VkImage tex_image(Texture* t);
VkFormat tex_vk_format(Texture* t);
VkImageAspectFlags tex_aspect(Texture* t);
VkExtent3D tex_extent(Texture* t, uint32_t level = 0);
uint32_t tex_levels(Texture* t);
uint32_t tex_layers(Texture* t);
bool tex_is_depth(Texture* t);
// Sampled image view for a binding (cached). Null when impossible (the caller binds the dummy view).
VkImageView tex_sampled_view(Texture* t, const ViewRequest& req);
// Single-level, single-layer (or all layers when layer_count > 1) view for use as an attachment.
VkImageView tex_attachment_view(Texture* t, uint32_t level, uint32_t layer, uint32_t layer_count, VkFormat view_format);
// The format colour attachments of this texture are rendered through (integer 32-bit formats as their float alias).
VkFormat tex_attachment_format(Texture* t, VkFormat view_format);
// The image can be bound as a storage image (created with NVN_TEXTURE_FLAGS_IMAGE).
bool tex_has_storage(Texture* t);
// The layout a texture is sampled in: SHADER_READ_ONLY_OPTIMAL, or DEPTH_STENCIL_READ_ONLY_OPTIMAL for depth and stencil formats (so
// the same layout serves a depth texture that is also the read-only depth attachment of the pass).
VkImageLayout tex_sampled_layout(Texture* t);
// A 1x1 storage image view in GENERAL layout for descriptors that cannot be resolved (UNDEFINED format: RGBA8 UNORM).
VkImageView dummy_storage_view(VkImageViewType type, VkFormat format);
// A UNORM (non-sRGB) view of the whole texture for the present pass.
VkImageView tex_present_view(Texture* t);
// Dummy 1x1 views used when a binding cannot be resolved: black texture of the requested type.
VkImageView dummy_view(VkImageViewType type, bool depth_compare);
VkSampler dummy_sampler();
// Sampler for the NVN sampler state (cached forever). Never null.
VkSampler sampler_for(const SamplerInfo& s);
// Buffer <-> image copies with the NVN conventions (used by the replay for CopyBufferToTexture etc.).
// Debug: the content of level 0 / layer 0 of a texture the GPU has written, as a PNG (HDR values squashed with x / (1 + x), integer
// formats show their low bytes as colour) and its per-channel statistics. Render thread; waits for the GPU. False: not dumpable.
bool dump_texture(Texture* t, const std::string& png_path, std::string* stats);
// Debug: records, into the frame being replayed, a copy of level 0 / layer 0 of the texture (no pass may be open) and keeps it until
// snapshot_write(): returns an index (-1: not copyable). snapshot_write writes it as dump_texture does and frees the buffer.
int snapshot_record(Texture* t, Frame& f);
bool snapshot_write(int index, const std::string& png_path, std::string* stats);
void tex_destroy(TexGpu* g);      // render thread
void tex_shutdown();              // frees samplers and dummy resources (device idle)
// Image formats chosen: bool descriptor for tests and logs
struct TexStats { uint64_t images = 0, uploads = 0, upload_bytes = 0, transcodes = 0, failed = 0; };
TexStats tex_stats();

// ---- replay (nvn_replay.cpp) ----------------------------------------------------------------------------------------------
// Called by nvn_gpu.cpp inside a submission: begin_submit resets per-submit tracking, replay_lists interprets the lists in
// order (state persists across lists and submissions), end_submit ends the open render pass and emits the closing barrier.
void replay_begin_submit(Frame& f);
void replay_lists(Frame& f, const std::vector<Ref<CmdList>>& lists);
void replay_end_submit(Frame& f);
void replay_shutdown();           // destroys pipelines and layouts (render thread, device idle)

// ---- shared facts (nvn_gpu.cpp) ----------------------------------------------------------------------------------------
int window_origin_mode();
int depth_mode();
gfx::Renderer* renderer();
FrameLimiter* limiter();
PresentPass* present_pass();
extras::OutputPass* output_pass();
bool keep_aspect();

// After a traced frame ([nvn] trace_frame): dumps the targets it drew to (PNG next to the log: trace_<texture id>_<format>.png) and logs their statistics.
void trace_dump_targets();

// ---- present -----------------------------------------------------------------------------------------------------------
void present_on_render_thread(Window* w, int index);

}  // namespace nxrt::nvn::be::gpu
