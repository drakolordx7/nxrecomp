// SPDX-License-Identifier: GPL-3.0-or-later
// The NVN-on-Vulkan back end (stream S4d). install_backend() replaces the logging stubs of the NVN shell (nvn.h) with real
// implementations and starts the render thread that replays the game's command lists on a gfx::Device.
//   * Memory pools are the game's own memory: the back end imports it as Vulkan memory (VK_EXT_external_memory_host), so a GPU
//     address is a host address and buffers are zero-copy.
//   * Game threads only record. nvnQueueSubmitCommands hands the lists to the render thread and returns when they are recorded
//     into Vulkan (not when the GPU has run them); fences, nvnQueueFinish and the memory a game polls follow the GPU timeline.
//   * nvnQueuePresentTexture draws the window texture into the Renderer's frame (honouring the shell's FrameLimiter when one is
//     given) and presents it.
// Configuration (config.h, environment wins): [nvn] shader_cache (translated SPIR-V folder), bc7_cache (ASTC->BC7 cache folder),
// bc7_quality, log_unimplemented; see docs/CONFIG.md.
#pragma once
#include <cstdint>
#include <string>

#include "nxrt/gfx.h"

namespace nxrt {
class FrameLimiter;
}

namespace nxrt::nvn {

struct BackendDesc {
    gfx::Device* device = nullptr;            // required; must outlive the back end
    gfx::Renderer* renderer = nullptr;        // optional: without it nvnQueuePresentTexture only waits for the GPU
    FrameLimiter* limiter = nullptr;          // optional: begin_frame() is called before each presented frame
    bool keep_aspect = false;                 // present with letterbox bars instead of stretching to the window
    bool register_entry_points = true;        // false for tests that call the Vulkan layer directly
};

bool install_backend(const BackendDesc& desc);   // idempotent per process: a second call with another device fails
void shutdown_backend();                         // waits for the GPU, destroys every Vulkan object of the back end
bool backend_installed();
// Writes the on-disk caches (the pipeline cache) now. For the paths that end the process without shutting the back end down (window closed).
void backend_flush_caches();

struct BackendStats {
    uint64_t submits = 0, lists_replayed = 0, draws = 0, draws_skipped = 0, dispatches = 0, clears = 0, copies = 0, presents = 0;
    uint64_t passes = 0, pass_breaks = 0, pipelines_created = 0, pipeline_cache_hits = 0, descriptor_sets = 0;
    uint64_t textures_created = 0, texture_uploads = 0, texture_upload_bytes = 0, bc7_transcodes = 0, textures_failed = 0;
    uint64_t pools_imported = 0, unresolved_addresses = 0, missing_handles = 0, unsupported_ops = 0;
    uint64_t submit_replay_ns = 0;
};
BackendStats backend_stats();
// Operations that were skipped because they are not implemented, with how often (for the log and docs/S4d.md).
std::string backend_unsupported_report();

}  // namespace nxrt::nvn
