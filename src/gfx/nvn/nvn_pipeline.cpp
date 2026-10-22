// SPDX-License-Identifier: GPL-3.0-or-later
// Pipeline layouts, shader modules and pipelines (see nvn_pipeline.h). Pipeline creation is synchronous on the render thread; a
// VkPipelineCache persists between runs ([nvn] pipeline_cache, default "pipeline_cache.bin").
#include "nvn_pipeline.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>

#include "nxrt/config.h"
#include "nxrt/ftrace.h"
#include "nxrt/log.h"
#include "nvn_gpu_internal.h"
#include "nvn_spirv_patch.h"

namespace nxrt::nvn::be::gpu {
namespace {

constexpr const char* kTag = "nvn.pipe";

struct LayoutKey {
    const ShaderInfo* s[6];
    bool operator<(const LayoutKey& o) const { return std::memcmp(s, o.s, sizeof s) < 0; }
};

// Shader modules and layouts are also made by the prewarm workers (nvn_prewarm.cpp), so they have locks; the pipeline buckets below stay
// render thread only.
std::mutex g_layout_mu;
std::recursive_mutex g_module_mu;   // every module cache below; the variant makers call shader_module
std::map<LayoutKey, ProgLayout> g_layouts;
std::unordered_map<const ShaderInfo*, VkShaderModule> g_modules;
std::atomic<bool> g_cache_dirty{false};
// Producer modules with extra outputs (nvn_spirv_patch.h): (shader, slots as location * 4 + scalar type).
std::map<std::pair<const ShaderInfo*, std::vector<uint32_t>>, VkShaderModule> g_variants;
std::unordered_map<const ShaderInfo*, VkShaderModule> g_dual_src;
std::unordered_map<const ShaderInfo*, std::pair<VkShaderModule, bool>> g_no_out;   // (module, owned by this map)      // fragment shaders with private outputs (passes without colour attachments)
std::unordered_map<const ShaderInfo*, std::pair<VkShaderModule, uint32_t>> g_xfb_mod;   // vertex shaders with the captured outputs, their stride       // fragment shaders with the second blend source at Location 0, Index 1
std::unordered_map<uint64_t, std::vector<std::pair<GraphicsKey, VkPipeline>>> g_gfx;
std::map<std::pair<const ShaderInfo*, VkPipelineLayout>, VkPipeline> g_compute;
VkPipelineCache g_cache = VK_NULL_HANDLE;
bool g_cache_tried = false;
std::string g_cache_path;
size_t g_pipeline_count = 0;
std::unordered_map<const ShaderInfo*, std::pair<gfx::Buffer, VkDeviceSize>> g_const_bufs;
gfx::Buffer g_driver_const;
bool g_driver_const_made = false;

VkShaderStageFlagBits stage_bit(ShaderStage s) {
    switch (s) {
        case ShaderStage::Vertex: return VK_SHADER_STAGE_VERTEX_BIT;
        case ShaderStage::Fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
        case ShaderStage::Geometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
        case ShaderStage::Compute: return VK_SHADER_STAGE_COMPUTE_BIT;
        default: return VK_SHADER_STAGE_VERTEX_BIT;
    }
}

std::mutex g_cache_mu;
std::atomic<bool> g_cache_ready{false};

void ensure_cache() {
    if (g_cache_ready.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(g_cache_mu);       // the prewarm workers and the render thread may get here together
    if (g_cache_tried) return;
    g_cache_tried = true;
    g_cache_path = config_get_str("nvn", "pipeline_cache", "pipeline_cache.bin");
    std::vector<char> data;
    if (!g_cache_path.empty()) {
        std::ifstream f(g_cache_path, std::ios::binary);
        if (f) data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(vkdev(), &ci, nullptr, &g_cache) != VK_SUCCESS) {
        ci.initialDataSize = 0;
        ci.pInitialData = nullptr;
        vkCreatePipelineCache(vkdev(), &ci, nullptr, &g_cache);
    } else if (!data.empty()) {
        NXLOG_I(kTag, "pipeline cache loaded: %zu bytes from %s", data.size(), g_cache_path.c_str());
    }
    g_cache_ready.store(true, std::memory_order_release);
}

void save_cache() {
    if (!g_cache || g_cache_path.empty()) return;
    g_cache_dirty.store(false);
    size_t n = 0;
    if (vkGetPipelineCacheData(vkdev(), g_cache, &n, nullptr) != VK_SUCCESS || !n) return;
    std::vector<char> data(n);
    if (vkGetPipelineCacheData(vkdev(), g_cache, &n, data.data()) != VK_SUCCESS) return;
    // written to a temporary file and renamed, so a process that ends in the middle of it leaves the old cache intact
    const std::string tmp = g_cache_path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write(data.data(), static_cast<std::streamsize>(n));
        if (!f) return;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, g_cache_path, ec);
    if (ec) {
        std::filesystem::remove(g_cache_path, ec);
        std::filesystem::rename(tmp, g_cache_path, ec);
    }
}

}  // namespace

bool pipeline_cache_dirty() { return g_cache_dirty.load(std::memory_order_relaxed); }
void pipeline_cache_save() { save_cache(); }

bool GraphicsKey::operator==(const GraphicsKey& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
uint64_t GraphicsKey::hash() const {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(this);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof *this; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// A module for SPIR-V words. The translator declares OpCapability Float64 in every module although no module uses a 64-bit float;
// devices without shaderFloat64 (Intel iGPUs) reject that, so the capability is dropped when no OpTypeFloat 64 exists.
static VkShaderModule create_module(const std::vector<uint32_t>& words, const char* what) {
    std::vector<uint32_t> fixed;
    const std::vector<uint32_t>* code = &words;
    {
        const auto& w = words;
        size_t cap_at = 0;
        bool uses64 = false;
        for (size_t i = 5; i < w.size();) {
            const uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
            if (len == 0) break;
            if (op == 17 && w[i + 1] == 10) cap_at = i;                       // OpCapability Float64
            if (op == 22 && i + 2 < w.size() && w[i + 2] == 64) uses64 = true;  // OpTypeFloat 64
            i += len;
        }
        if (cap_at && !uses64) {
            fixed = w;
            fixed.erase(fixed.begin() + static_cast<std::ptrdiff_t>(cap_at), fixed.begin() + static_cast<std::ptrdiff_t>(cap_at) + 2);
            code = &fixed;
        }
    }
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code->size() * 4;
    ci.pCode = code->data();
    VkShaderModule m = VK_NULL_HANDLE;
    const VkResult r = vkCreateShaderModule(vkdev(), &ci, nullptr, &m);
    if (r != VK_SUCCESS) NXLOG_E(kTag, "vkCreateShaderModule failed (%d) for %s", static_cast<int>(r), what);
    return m;
}

VkShaderModule shader_module(const ShaderInfo* s) {
    if (!s) return VK_NULL_HANDLE;
    std::lock_guard<std::recursive_mutex> lk(g_module_mu);
    auto it = g_modules.find(s);
    if (it != g_modules.end()) return it->second;
    VkShaderModule m = create_module(s->spirv, s->hash.c_str());
    g_modules[s] = m;
    return m;
}

static const std::pair<VkShaderModule, uint32_t>& xfb_variant(const ShaderInfo* vs) {
    std::lock_guard<std::recursive_mutex> lk(g_module_mu);
    auto it = g_xfb_mod.find(vs);
    if (it != g_xfb_mod.end()) return it->second;
    std::vector<uint32_t> patched;
    std::vector<SpvXfbOutput> lay;
    uint32_t stride = 0;
    VkShaderModule m = VK_NULL_HANDLE;
    if (spirv_add_xfb(vs->spirv, patched, &lay, &stride)) {
        m = create_module(patched, vs->hash.c_str());
        std::string desc;
        for (const SpvXfbOutput& o : lay) desc += " loc" + std::to_string(o.location) + "x" + std::to_string(o.components) + "@" + std::to_string(o.offset);
        NXLOG_I(kTag, "%s: transform feedback variant, %u bytes per vertex:%s", vs->hash.c_str(), stride, desc.c_str());
    } else {
        NXLOG_W(kTag, "%s: no transform feedback variant could be made (no Location outputs?)", vs->hash.c_str());
    }
    return g_xfb_mod.emplace(vs, std::make_pair(m, stride)).first->second;
}

uint32_t xfb_stride(const ShaderInfo* vs) { return vs ? xfb_variant(vs).second : 0; }

static VkShaderModule no_output_module(const ShaderInfo* fs) {
    std::lock_guard<std::recursive_mutex> lk(g_module_mu);
    auto it = g_no_out.find(fs);
    if (it != g_no_out.end()) return it->second.first;
    std::vector<uint32_t> patched;
    std::pair<VkShaderModule, bool> e = {VK_NULL_HANDLE, false};
    if (spirv_outputs_to_private(fs->spirv, patched)) e = {create_module(patched, fs->hash.c_str()), true};
    else e = {shader_module(fs), false};
    g_no_out[fs] = e;
    return e.first;
}

static VkShaderModule dual_source_module(const ShaderInfo* fs) {
    std::lock_guard<std::recursive_mutex> lk(g_module_mu);
    auto it = g_dual_src.find(fs);
    if (it != g_dual_src.end()) return it->second;
    VkShaderModule m = VK_NULL_HANDLE;
    std::vector<uint32_t> patched;
    if (spirv_dual_source_output(fs->spirv, patched)) {
        m = create_module(patched, fs->hash.c_str());
    } else {
        NXLOG_W(kTag, "%s: dual-source blending is on but the fragment shader has no second output; the unpatched module is used", fs->hash.c_str());
        m = shader_module(fs);
    }
    g_dual_src[fs] = m;
    return m;
}

// The slots `consumer` reads that `producer` does not write: the console gives them undefined values, Vulkan needs an output.
static std::vector<SpvOutputSlot> missing_outputs(const ShaderInfo* producer, const ShaderInfo* consumer) {
    std::vector<SpvOutputSlot> missing;
    if (!producer || !consumer) return missing;
    bool covered[64] = {};
    for (const ShaderIo& o : producer->refl.outputs)
        for (uint32_t e = 0; e < (o.array ? o.array : 1); ++e)
            if (o.location + e < 64) covered[o.location + e] = true;
    for (const ShaderIo& in : consumer->refl.inputs) {
        for (uint32_t e = 0; e < (in.array ? in.array : 1); ++e) {
            const uint32_t loc = in.location + e;
            if (loc >= 64 || covered[loc]) continue;
            covered[loc] = true;
            SpvOutputSlot slot;
            slot.location = loc;
            slot.type = in.type == ScalarType::Sint ? SpvScalar::Sint : in.type == ScalarType::Uint ? SpvScalar::Uint : SpvScalar::Float;
            missing.push_back(slot);
        }
    }
    return missing;
}

// The module of `producer` to use in a pipeline whose next stage is `consumer`: the translated one, or a copy with the outputs the
// consumer reads and the producer lacks. Null on a Vulkan error.
static VkShaderModule producer_module(const ShaderInfo* producer, const ShaderInfo* consumer) {
    const std::vector<SpvOutputSlot> missing = missing_outputs(producer, consumer);
    if (missing.empty()) return shader_module(producer);
    std::vector<uint32_t> id;
    for (const SpvOutputSlot& m : missing) id.push_back(m.location * 4 + static_cast<uint32_t>(m.type));
    std::lock_guard<std::recursive_mutex> lk(g_module_mu);
    auto it = g_variants.find({producer, id});
    if (it != g_variants.end()) return it->second;
    VkShaderModule m = VK_NULL_HANDLE;
    std::vector<uint32_t> patched;
    // a passthrough geometry shader: the hardware forwards the attributes of the input vertices, the translation has no code for it
    const bool pass = producer->stage == ShaderStage::Geometry && spirv_gs_passthrough(producer->spirv, missing, patched);
    if (pass || spirv_add_outputs(producer->spirv, missing, patched)) {
        m = create_module(patched, producer->hash.c_str());
        NXLOG_D(kTag, "%s: %zu outputs added for the next stage%s", producer->hash.c_str(), missing.size(), pass ? " (copied from the input vertices)" : "");
    } else {
        NXLOG_W(kTag, "%s: the %zu varyings the next stage reads could not be added; the unpatched module is used", producer->hash.c_str(), missing.size());
        m = shader_module(producer);
    }
    g_variants[{producer, id}] = m;
    return m;
}

const ProgLayout* prog_layout(const ShaderInfo* const stages[6]) {
    LayoutKey key;
    for (int i = 0; i < 6; ++i) key.s[i] = stages[i];
    std::lock_guard<std::mutex> lk(g_layout_mu);
    auto it = g_layouts.find(key);
    if (it != g_layouts.end()) return &it->second;

    ProgLayout pl;
    std::map<std::pair<uint32_t, uint32_t>, size_t> index;
    for (int st = 0; st < 6; ++st) {
        const ShaderInfo* s = stages[st];
        if (!s) continue;
        for (size_t i = 0; i < s->refl.bindings.size(); ++i) {
            const DescriptorBinding& b = s->refl.bindings[i];
            if (!b.used || b.set > 3) continue;
            const auto k = std::make_pair(b.set, b.binding);
            auto f = index.find(k);
            if (f == index.end()) {
                BindingDesc d;
                d.set = b.set;
                d.binding = b.binding;
                d.type = static_cast<VkDescriptorType>(b.type);
                d.stages = stage_bit(s->stage);
                d.shader = s;
                d.index = static_cast<int>(i);
                d.nvn_stage = st;
                index[k] = pl.bindings.size();
                pl.bindings.push_back(d);
            } else {
                pl.bindings[f->second].stages |= stage_bit(s->stage);   // shared binding (the support buffer)
            }
        }
    }
    for (int set = 0; set < 4; ++set) {
        std::vector<VkDescriptorSetLayoutBinding> lb;
        for (const BindingDesc& d : pl.bindings) {
            if (d.set != static_cast<uint32_t>(set)) continue;
            VkDescriptorSetLayoutBinding b = {};
            b.binding = d.binding;
            b.descriptorType = d.type;
            b.descriptorCount = std::max<uint32_t>(1, d.shader->refl.bindings[d.index].count);
            b.stageFlags = d.stages;
            lb.push_back(b);
            pl.has_set[set] = true;
        }
        VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = static_cast<uint32_t>(lb.size());
        ci.pBindings = lb.data();
        if (vkCreateDescriptorSetLayout(vkdev(), &ci, nullptr, &pl.sets[set]) != VK_SUCCESS) {
            NXLOG_E(kTag, "descriptor set layout %d failed", set);
            return nullptr;
        }
    }
    VkPipelineLayoutCreateInfo pci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pci.setLayoutCount = 4;
    pci.pSetLayouts = pl.sets;
    if (vkCreatePipelineLayout(vkdev(), &pci, nullptr, &pl.layout) != VK_SUCCESS) {
        NXLOG_E(kTag, "pipeline layout failed");
        return nullptr;
    }
    return &g_layouts.emplace(key, std::move(pl)).first->second;
}

VkPipeline build_graphics_pipeline(const GraphicsKey& key, const ProgLayout& layout) {
    ensure_cache();
    const auto t0 = std::chrono::steady_clock::now();

    VkPipelineShaderStageCreateInfo stages[3] = {};
    uint32_t ns = 0;
    const VkShaderStageFlagBits bits[3] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT};
    // the stage that feeds the fragment shader gets outputs for the varyings the fragment shader reads and it does not write
    const int feeder = key.stage[2] ? 2 : 0;
    for (int i = 0; i < 3; ++i) {
        if (!key.stage[i]) continue;
        if (key.xfb && i != 0) continue;     // stream-out draws run the vertex stage only
        if (key.xfb) {
            VkShaderModule m = xfb_variant(key.stage[0]).first;
            if (!m) return VK_NULL_HANDLE;
            stages[ns] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stages[ns].stage = bits[0];
            stages[ns].module = m;
            stages[ns].pName = "main";
            ++ns;
            continue;
        }
        VkShaderModule m = (i == feeder && key.stage[1]) ? producer_module(key.stage[i], key.stage[1])
                           : (i == 1 && key.no_out)      ? no_output_module(key.stage[i])
                           : (i == 1 && key.dual_src)    ? dual_source_module(key.stage[i])
                                                         : shader_module(key.stage[i]);
        if (!m) return VK_NULL_HANDLE;
        stages[ns] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[ns].stage = bits[i];
        stages[ns].module = m;
        stages[ns].pName = "main";
        ++ns;
    }

    // vertex input
    VkVertexInputBindingDescription vb[16];
    VkVertexInputAttributeDescription va[16];
    uint32_t nvb = 0, nva = 0;
    bool binding_seen[16] = {};
    for (uint32_t i = 0; i < key.attr_count && i < 16; ++i) {
        const KeyAttr& a = key.attr[i];
        va[nva++] = {a.location, a.binding, static_cast<VkFormat>(a.format), a.offset};
        if (!binding_seen[a.binding]) {
            binding_seen[a.binding] = true;
            vb[nvb++] = {a.binding, 0, key.binding_instance[a.binding] ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        }
    }
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = nvb;
    vi.pVertexBindingDescriptions = vb;
    vi.vertexAttributeDescriptionCount = nva;
    vi.pVertexAttributeDescriptions = va;

    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = static_cast<VkPrimitiveTopology>(key.topology);
    VkPipelineTessellationStateCreateInfo ts = {VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
    ts.patchControlPoints = key.patch_control_points;

    VkPipelineViewportDepthClipControlCreateInfoEXT dcc = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
    dcc.negativeOneToOne = key.depth_neg_one ? VK_TRUE : VK_FALSE;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    if (device().features().depth_clip_control) vp.pNext = &dcc;   // viewport and scissor counts are dynamic

    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.depthClampEnable = key.depth_clamp ? VK_TRUE : VK_FALSE;
    rs.polygonMode = static_cast<VkPolygonMode>(key.polygon_mode);
    rs.lineWidth = 1.0f;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = static_cast<VkSampleCountFlagBits>(key.samples ? key.samples : 1);
    ms.alphaToCoverageEnable = key.alpha_to_coverage ? VK_TRUE : VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.maxDepthBounds = 1.0f;

    VkPipelineColorBlendAttachmentState cba[8];
    for (uint32_t i = 0; i < key.color_count && i < 8; ++i) {
        cba[i] = {};
        cba[i].blendEnable = key.blend_enable[i] ? VK_TRUE : VK_FALSE;
        cba[i].srcColorBlendFactor = static_cast<VkBlendFactor>(key.blend_src_rgb[i]);
        cba[i].dstColorBlendFactor = static_cast<VkBlendFactor>(key.blend_dst_rgb[i]);
        cba[i].colorBlendOp = static_cast<VkBlendOp>(key.blend_op_rgb[i]);
        cba[i].srcAlphaBlendFactor = static_cast<VkBlendFactor>(key.blend_src_a[i]);
        cba[i].dstAlphaBlendFactor = static_cast<VkBlendFactor>(key.blend_dst_a[i]);
        cba[i].alphaBlendOp = static_cast<VkBlendOp>(key.blend_op_a[i]);
        cba[i].colorWriteMask = key.write_mask[i];
    }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.logicOpEnable = key.logic_op_enable ? VK_TRUE : VK_FALSE;
    cb.logicOp = static_cast<VkLogicOp>(key.logic_op);
    cb.attachmentCount = key.color_count;
    cb.pAttachments = cba;

    static const VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
        VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_DEPTH_BOUNDS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_LINE_WIDTH};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = sizeof(dyn) / sizeof(dyn[0]);
    dy.pDynamicStates = dyn;

    VkFormat cf[8];
    for (uint32_t i = 0; i < key.color_count && i < 8; ++i) cf[i] = static_cast<VkFormat>(key.color_format[i]);
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = key.color_count;
    ri.pColorAttachmentFormats = cf;
    ri.depthAttachmentFormat = static_cast<VkFormat>(key.depth_format);
    ri.stencilAttachmentFormat = static_cast<VkFormat>(key.stencil_format);

    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri};
    gi.stageCount = ns;
    gi.pStages = stages;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pTessellationState = key.patch_control_points ? &ts : nullptr;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &dy;
    gi.layout = layout.layout;
    VkPipeline pipe = VK_NULL_HANDLE;
    const VkResult r = vkCreateGraphicsPipelines(vkdev(), g_cache, 1, &gi, nullptr, &pipe);
    const double ms_taken = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (r != VK_SUCCESS) {
        NXLOG_E(kTag, "graphics pipeline failed (%d): vs %s fs %s, %u colour targets", static_cast<int>(r), key.stage[0] ? key.stage[0]->hash.c_str() : "-",
                key.stage[1] ? key.stage[1]->hash.c_str() : "-", key.color_count);
        pipe = VK_NULL_HANDLE;
    } else {
        counters().pipelines_created.fetch_add(1);
        g_cache_dirty.store(true, std::memory_order_relaxed);
        NXLOG_D(kTag, "graphics pipeline created in %.1f ms (vs %s fs %s)", ms_taken, key.stage[0] ? key.stage[0]->hash.c_str() : "-",
                key.stage[1] ? key.stage[1]->hash.c_str() : "-");
    }
    return pipe;
}

VkPipeline graphics_pipeline(const GraphicsKey& key, const ProgLayout& layout) {
    ensure_cache();
    const uint64_t h = key.hash();
    auto& bucket = g_gfx[h];
    for (auto& e : bucket)
        if (e.first == key) {
            counters().pipeline_cache_hits.fetch_add(1, std::memory_order_relaxed);
            return e.second;
        }
    prewarm_init();
    VkPipeline pipe = VK_NULL_HANDLE;
    if (prewarm_take(key, &pipe)) {              // compiled ahead on a worker (or being compiled: waits for it)
        ++g_pipeline_count;
        bucket.emplace_back(key, pipe);
        return pipe;
    }
    NXFT("pipe.create");
    pipe = build_graphics_pipeline(key, layout);
    if (pipe) {
        ++g_pipeline_count;
        prewarm_record(key);
    }
    bucket.emplace_back(key, pipe);
    return pipe;
}

VkPipeline compute_pipeline(const ShaderInfo* cs, const ProgLayout& layout) {
    ensure_cache();
    const auto k = std::make_pair(cs, layout.layout);
    auto it = g_compute.find(k);
    if (it != g_compute.end()) return it->second;
    VkShaderModule m = shader_module(cs);
    VkPipeline pipe = VK_NULL_HANDLE;
    if (m) {
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = m;
        ci.stage.pName = "main";
        ci.layout = layout.layout;
        if (vkCreateComputePipelines(vkdev(), g_cache, 1, &ci, nullptr, &pipe) != VK_SUCCESS) {
            NXLOG_E(kTag, "compute pipeline failed for %s", cs->hash.c_str());
            pipe = VK_NULL_HANDLE;
        } else {
            counters().pipelines_created.fetch_add(1);
            ++g_pipeline_count;
            g_cache_dirty.store(true, std::memory_order_relaxed);
        }
    }
    g_compute[k] = pipe;
    return pipe;
}

VkBuffer constant_data_buffer(const ShaderInfo* s, VkDeviceSize* range) {
    auto it = g_const_bufs.find(s);
    if (it == g_const_bufs.end()) {
        const VkDeviceSize size = std::max<VkDeviceSize>(256, (s->constants.size() + 15) & ~15ull);
        gfx::Buffer b;
        if (device().create_buffer(size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, gfx::MemoryUse::Upload, &b, "nvn c1") != VK_SUCCESS) {
            *range = 0;
            return VK_NULL_HANDLE;
        }
        std::memset(b.mapped, 0, size);
        if (!s->constants.empty()) std::memcpy(b.mapped, s->constants.data(), s->constants.size());
        it = g_const_bufs.emplace(s, std::make_pair(b, size)).first;
    }
    *range = it->second.second;
    return it->second.first.buffer;
}

VkBuffer driver_constants_buffer(VkDeviceSize* range) {
    if (!g_driver_const_made) {
        g_driver_const_made = true;
        if (device().create_buffer(65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, gfx::MemoryUse::Upload, &g_driver_const, "nvn c0") == VK_SUCCESS) {
            std::memset(g_driver_const.mapped, 0, 65536);
            // c[0]+0x610: maximum point size read by the agl vertex shaders (gl_PointSize = min(size, c0[0x610]))
            const float max_point = 64.0f;
            std::memcpy(static_cast<uint8_t*>(g_driver_const.mapped) + 0x610, &max_point, 4);
        }
    }
    *range = 65536;
    return g_driver_const.buffer;
}

size_t pipeline_count() { return g_pipeline_count; }

void pipeline_shutdown() {
    VkDevice d = vkdev();
    prewarm_shutdown();
    save_cache();
    for (auto& [h, bucket] : g_gfx)
        for (auto& e : bucket)
            if (e.second) vkDestroyPipeline(d, e.second, nullptr);
    g_gfx.clear();
    for (auto& [k, p] : g_compute)
        if (p) vkDestroyPipeline(d, p, nullptr);
    g_compute.clear();
    for (auto& [k, l] : g_layouts) {
        if (l.layout) vkDestroyPipelineLayout(d, l.layout, nullptr);
        for (VkDescriptorSetLayout s : l.sets)
            if (s) vkDestroyDescriptorSetLayout(d, s, nullptr);
    }
    g_layouts.clear();
    for (auto& [k, m] : g_modules)
        if (m) vkDestroyShaderModule(d, m, nullptr);
    g_modules.clear();
    for (auto& [k, m] : g_variants)
        if (m) vkDestroyShaderModule(d, m, nullptr);
    g_variants.clear();
    for (auto& [k, m] : g_dual_src)
        if (m) vkDestroyShaderModule(d, m, nullptr);
    g_dual_src.clear();
    for (auto& [k, e] : g_no_out)
        if (e.first && e.second) vkDestroyShaderModule(d, e.first, nullptr);
    g_no_out.clear();
    for (auto& [k, v] : g_xfb_mod)
        if (v.first) vkDestroyShaderModule(d, v.first, nullptr);
    g_xfb_mod.clear();
    for (auto& [k, b] : g_const_bufs) device().destroy_buffer(b.first);
    g_const_bufs.clear();
    if (g_driver_const_made) device().destroy_buffer(g_driver_const);
    g_driver_const_made = false;
    if (g_cache) vkDestroyPipelineCache(d, g_cache, nullptr);
    g_cache = VK_NULL_HANDLE;
    g_cache_tried = false;
    g_cache_ready.store(false);
    g_cache_dirty.store(false);
    g_pipeline_count = 0;
}

}  // namespace nxrt::nvn::be::gpu
