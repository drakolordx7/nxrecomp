// SPDX-License-Identifier: GPL-3.0-or-later
// Pipeline layouts, shader modules and pipelines (see nvn_pipeline.h). Pipeline creation is synchronous on the render thread; a
// VkPipelineCache persists between runs ([nvn] pipeline_cache, default "pipeline_cache.bin").
#include "nvn_pipeline.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nvn_gpu_internal.h"

namespace nxrt::nvn::be::gpu {
namespace {

constexpr const char* kTag = "nvn.pipe";

struct LayoutKey {
    const ShaderInfo* s[6];
    bool operator<(const LayoutKey& o) const { return std::memcmp(s, o.s, sizeof s) < 0; }
};

std::map<LayoutKey, ProgLayout> g_layouts;
std::unordered_map<const ShaderInfo*, VkShaderModule> g_modules;
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

void ensure_cache() {
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
}

void save_cache() {
    if (!g_cache || g_cache_path.empty()) return;
    size_t n = 0;
    if (vkGetPipelineCacheData(vkdev(), g_cache, &n, nullptr) != VK_SUCCESS || !n) return;
    std::vector<char> data(n);
    if (vkGetPipelineCacheData(vkdev(), g_cache, &n, data.data()) != VK_SUCCESS) return;
    std::ofstream f(g_cache_path, std::ios::binary | std::ios::trunc);
    if (f) f.write(data.data(), static_cast<std::streamsize>(n));
}

}  // namespace

bool GraphicsKey::operator==(const GraphicsKey& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
uint64_t GraphicsKey::hash() const {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(this);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof *this; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

VkShaderModule shader_module(const ShaderInfo* s) {
    if (!s) return VK_NULL_HANDLE;
    auto it = g_modules.find(s);
    if (it != g_modules.end()) return it->second;
    // The translator declares OpCapability Float64 in every module although no module uses a 64-bit float; devices without
    // shaderFloat64 (Intel iGPUs) reject that. Drop the capability when no OpTypeFloat 64 exists.
    std::vector<uint32_t> fixed;
    const std::vector<uint32_t>* code = &s->spirv;
    {
        const auto& w = s->spirv;
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
    if (r != VK_SUCCESS) NXLOG_E(kTag, "vkCreateShaderModule failed (%d) for %s", static_cast<int>(r), s->hash.c_str());
    g_modules[s] = m;
    return m;
}

const ProgLayout* prog_layout(const ShaderInfo* const stages[6]) {
    LayoutKey key;
    for (int i = 0; i < 6; ++i) key.s[i] = stages[i];
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

VkPipeline graphics_pipeline(const GraphicsKey& key, const ProgLayout& layout) {
    ensure_cache();
    const uint64_t h = key.hash();
    auto& bucket = g_gfx[h];
    for (auto& e : bucket)
        if (e.first == key) {
            counters().pipeline_cache_hits.fetch_add(1, std::memory_order_relaxed);
            return e.second;
        }
    const auto t0 = std::chrono::steady_clock::now();

    VkPipelineShaderStageCreateInfo stages[3] = {};
    uint32_t ns = 0;
    const VkShaderStageFlagBits bits[3] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT};
    for (int i = 0; i < 3; ++i) {
        if (!key.stage[i]) continue;
        VkShaderModule m = shader_module(key.stage[i]);
        if (!m) { bucket.emplace_back(key, VK_NULL_HANDLE); return VK_NULL_HANDLE; }
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
        ++g_pipeline_count;
        NXLOG_D(kTag, "graphics pipeline #%zu created in %.1f ms (vs %s fs %s)", g_pipeline_count, ms_taken, key.stage[0] ? key.stage[0]->hash.c_str() : "-",
                key.stage[1] ? key.stage[1]->hash.c_str() : "-");
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
    for (auto& [k, b] : g_const_bufs) device().destroy_buffer(b.first);
    g_const_bufs.clear();
    if (g_driver_const_made) device().destroy_buffer(g_driver_const);
    g_driver_const_made = false;
    if (g_cache) vkDestroyPipelineCache(d, g_cache, nullptr);
    g_cache = VK_NULL_HANDLE;
    g_cache_tried = false;
    g_pipeline_count = 0;
}

}  // namespace nxrt::nvn::be::gpu
