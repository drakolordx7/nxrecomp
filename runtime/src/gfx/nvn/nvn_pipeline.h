// SPDX-License-Identifier: GPL-3.0-or-later
// Pipeline layouts and pipelines of the NVN back end (render thread only). A program (one translated shader per stage) has
// one pipeline layout of four descriptor sets with the Ryujinx binding scheme (set 0 uniform buffers, 1 storage buffers,
// 2 textures, 3 images; see docs/S4d.md). Graphics pipelines use dynamic rendering and as much dynamic state as Vulkan 1.3
// offers (viewports, scissors, cull mode, front face, topology, depth/stencil tests and ops, depth bias, stencil references
// and masks, blend constants, line width, vertex strides); everything else is part of the key.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <volk.h>

#include "nxrt/nvn_backend.h"

#include "nvn_core.h"
#include "nvn_shaders.h"
#include "nvn_spirv_patch.h"

namespace nxrt::nvn::be::gpu {

struct BindingDesc {
    uint32_t set = 0, binding = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    VkShaderStageFlags stages = 0;
    const ShaderInfo* shader = nullptr;         // the stage module that declares it
    int index = -1;                             // into shader->refl.bindings
    int nvn_stage = 0;                          // NVNshaderStage of that module (0 vertex, 1 fragment, 2 geometry, 5 compute)
};

struct ProgLayout {
    VkDescriptorSetLayout sets[4] = {};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::vector<BindingDesc> bindings;          // every used descriptor of every stage, sorted by (set, binding)
    bool has_set[4] = {};
};

// Stages by NVN stage index (0 vertex, 1 fragment, 2 geometry, 5 compute); null = absent.
const ProgLayout* prog_layout(const ShaderInfo* const stages[6]);

// Vertex input for the key.
struct KeyAttr {
    uint8_t location = 0, binding = 0;
    uint16_t offset = 0;
    uint32_t format = 0;                        // VkFormat
};

struct GraphicsKey {
    const ShaderInfo* stage[3] = {};            // vertex, fragment, geometry
    uint32_t color_format[8] = {};              // VkFormat per attachment slot (0 = unused slot)
    uint32_t color_count = 0;
    uint32_t depth_format = 0, stencil_format = 0;
    uint32_t samples = 1;
    uint32_t attr_count = 0;
    KeyAttr attr[16] = {};
    uint8_t binding_instance[16] = {};          // 1 = per-instance rate
    // blend, per colour slot
    uint8_t blend_enable[8] = {};
    uint8_t blend_src_rgb[8] = {}, blend_dst_rgb[8] = {}, blend_src_a[8] = {}, blend_dst_a[8] = {};   // VkBlendFactor
    uint8_t blend_op_rgb[8] = {}, blend_op_a[8] = {};                                                 // VkBlendOp
    uint8_t write_mask[8] = {};
    uint8_t logic_op_enable = 0, logic_op = 0;
    uint8_t topology = 0;                       // VkPrimitiveTopology (class; the exact topology is dynamic)
    uint8_t polygon_mode = 0;                   // VkPolygonMode
    uint8_t depth_clamp = 0, alpha_to_coverage = 0, depth_neg_one = 0;
    uint8_t patch_control_points = 0;
    uint8_t dummy_binding = 0;                  // 1 + the vertex binding that feeds unset attributes (stride 0, defaults buffer); 0 = none
    uint8_t xfb = 0;                            // the vertex shader runs with transform feedback (captured outputs, no fragment stage)
    uint8_t dual_src = 0;                       // a blend factor of an enabled attachment reads the second source (fragment output moved to Location 0, Index 1)
    uint8_t no_out = 0;                         // the pass has no colour attachment: the fragment shader's outputs become private variables
    uint8_t pad[0 + 1] = {};
    bool operator==(const GraphicsKey& o) const;
    uint64_t hash() const;
};

// The pipeline for a key, created on first use (logged; counters). Null on failure (the draw is skipped).
VkPipeline graphics_pipeline(const GraphicsKey& key, const ProgLayout& layout);
// The creation itself: no lookup, no bookkeeping; safe to call from any thread (the prewarm workers). Null on failure.
VkPipeline build_graphics_pipeline(const GraphicsKey& key, const ProgLayout& layout);

// The shader module each stage of a key runs (the translated one or a patched variant: varyings the next stage reads, integer attributes,
// stream-out, dual-source, private outputs, HDR tone map), by stage slot as in GraphicsKey::stage; null = no such stage. False on a Vulkan error.
struct StageModules { VkShaderModule m[3] = {}; };
bool resolve_stage_modules(const GraphicsKey& key, StageModules* out);
// The vertex inputs the vertex shader declares as floats that this key feeds with integer attributes (the shader module is then a patched variant).
std::vector<SpvIntSlot> key_int_slots(const GraphicsKey& key);
// The dynamic state list every graphics pipeline and pipeline library uses.
const VkPipelineDynamicStateCreateInfo* pipeline_dynamic_state();
VkPipelineCache pipeline_cache_handle();
void note_pipeline_created(double ms, const char* what);   // counters and the dirty flag of the cache file

// Pipeline cache on disk ([nvn] pipeline_cache): written by a background thread while new pipelines appear and at shutdown.
bool pipeline_cache_dirty();
void pipeline_cache_save();
void pipeline_cache_flush();

// ---- prewarm (nvn_prewarm.cpp) ----------------------------------------------------------------------------------------------
// Every pipeline that had to be created at a draw is recorded in a key file ([nvn] pipeline_keys). The next run compiles the recorded
// pipelines of a program on worker threads as soon as the game sets the program's shaders, long before its first draw.
void prewarm_init();                                                      // loads the key file, starts the cache saver (idempotent)
void prewarm_record(const GraphicsKey& key);                              // a pipeline was created for this key at draw time
size_t prewarm_program(const ShaderInfo* const stages[6]);                // a program got its shaders: queue its recorded pipelines (how many)
bool prewarm_take(const GraphicsKey& key, VkPipeline* out);               // true when a prewarm job owns the key; waits for a running one
void prewarm_shutdown();                                                  // stops the workers (before the device is torn down)
struct PrewarmStats { uint64_t recorded = 0, loaded = 0, queued = 0, built = 0, taken = 0, waited = 0, claimed = 0; double wait_ms = 0; };
PrewarmStats prewarm_stats();
VkPipeline compute_pipeline(const ShaderInfo* cs, const ProgLayout& layout);
VkShaderModule shader_module(const ShaderInfo* s);
// Bytes of one captured vertex of the vertex shader's transform feedback variant (0: none could be made).
uint32_t xfb_stride(const ShaderInfo* vs);

// Persistent buffers every draw may bind: the driver constants (c[0]) image and a shader's constant data (c[1]).
VkBuffer constant_data_buffer(const ShaderInfo* s, VkDeviceSize* range);
VkBuffer driver_constants_buffer(VkDeviceSize* range);

// ---- pipeline libraries (nvn_gpl.cpp, VK_EXT_graphics_pipeline_library) ------------------------------------------------------
// A pipeline is linked from four parts: vertex input, pre-rasterisation shaders (vertex/geometry code + raster state), fragment shader
// (fragment code), fragment output (blend and attachment formats). The two shader parts hold all the compile time and depend on very little
// state (the shaders of a program and three raster flags), so they are compiled by background threads when the game sets a program's
// shaders, long before the draw whose blend state and formats are only known then. The draw links the parts.
bool gpl_active();                                                   // enabled and supported (first call reads the settings)
// The optimised pipeline that replaces a linked one: made by a background thread, taken by the draw side on a later lookup (state 1 = ready;
// pipe may be null when the build failed). The linked pipeline stays in use until then.
struct PipeUpgrade {
    std::atomic<int> state{0};
    VkPipeline pipe = VK_NULL_HANDLE;
};
// null: not possible for this key, build it whole. *up (when given) gets the upgrade the linked pipeline will be replaced by (null: none queued).
VkPipeline gpl_link(const GraphicsKey& key, const ProgLayout& layout, std::shared_ptr<PipeUpgrade>* up = nullptr);
bool gpl_ab_whole();                                                 // measurement only ([nvn] gpl_ab): true in the windows that run the optimised pipelines
void gpl_take_upgrade(VkPipeline& current, VkPipeline& alt, std::shared_ptr<PipeUpgrade>& up);   // render thread: swaps in the finished optimised pipeline (the old one is destroyed once the GPU is done with it)
void gpl_program(const ShaderInfo* const stages[6], bool covered);   // a program got its shaders: queue its shader parts (covered: the key file has pipelines for it, skip)
void gpl_program_bound(const ShaderInfo* const stages[6]);           // a command buffer was recorded with the program bound: its parts go first
void gpl_progress_log();                                             // one log line when something changed since the last call
void gpl_stop();                                                     // stops the workers (before modules and layouts go)
void gpl_shutdown();                                                 // destroys the parts (after the pipelines)
struct GplStats {
    uint64_t upgrades_queued = 0, upgrades_done = 0, upgrades_failed = 0, upgrades_taken = 0;
    double upgrade_ms = 0;
    uint64_t linked = 0, link_failed = 0, parts_ahead = 0, parts_at_draw = 0, parts_waited = 0, parts_hit = 0, jobs = 0, jobs_skipped = 0;
    uint64_t jobs_pending = 0;                       // shader-part jobs queued or running at this moment (0 once every job queued so far has finished)
    double link_ms = 0, draw_part_ms = 0, wait_ms = 0, worker_ms = 0;
};
GplStats gpl_stats();
void set_compile_thread_priority(bool background = true);                                  // [nvn] compile_priority for the threads that build pipelines in the background

// ---- shader preparation (nvn_gpl.cpp, S24) ---------------------------------------------------------------------------------
// Compiles the shader parts of every program in a list (setup writes it from the player's own game files: <shader cache>/../programs.bin)
// before the game runs, so the driver has them in its cache and the game's own requests are fast. The parts are thrown away as soon as they exist.
using PrepareOptions = nxrt::nvn::ShaderPrepareOptions;
using PrepareResult = nxrt::nvn::ShaderPrepareResult;
bool gpl_prepare(const PrepareOptions& o, PrepareResult* r);       // false: nothing could be done (see r->error)
void module_cache_clear();                                          // destroys every cached shader module and variant (nothing may be creating pipelines)

void pipeline_shutdown();
size_t pipeline_count();

}  // namespace nxrt::nvn::be::gpu
