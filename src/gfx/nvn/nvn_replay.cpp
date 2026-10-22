// SPDX-License-Identifier: GPL-3.0-or-later
// The command list interpreter of the NVN back end (render thread). Walks recorded lists in order, keeps the GPU state the
// console keeps (it persists across lists and submissions), and turns it into Vulkan: dynamic-rendering passes opened lazily on the
// first draw or clear of a render target set, graphics pipelines keyed by the baked state, descriptor sets built from the
// translated shaders' binding tables (nvn_shaders.h), vertex and index data straight from the imported pool memory.
// Lessons applied from SuperMarioWonderRecomp docs/nvn/DEBUG.md and DEBUG-2.md (see work notes in docs/S4d.md): viewport flip by a
// negative-height viewport, a memory barrier after every pass, textures sampled by a draw are made current before the pass,
// handles never dropped by count, deferred bind arrays read at record time.
#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "nxrt/log.h"
#include "nvn_decoded.h"
#include "nvn_enums.h"
#include "nvn_format.h"
#include "nvn_mask_clear.h"
#include "nvn_gpu_internal.h"
#include "nvn_pipeline.h"
#include "nvn_shaders.h"

namespace nxrt::nvn::be::gpu {
namespace {

constexpr const char* kTag = "nvn.replay";
constexpr int kMaxDepth = 8;

enum Stage { kVertex = 0, kFragment = 1, kGeometry = 2, kTessCtl = 3, kTessEval = 4, kCompute = 5 };

struct GState {
    const ShaderInfo* stage[6] = {};
    struct Bind { uint64_t addr = 0, size = 0; };
    Bind vtx[16], ubo[6][16], ssbo[6][16];
    uint64_t tex[6][64] = {};
    uint64_t img[6][16] = {};
    Ref<TexturePool> tpool;
    Ref<SamplerPool> spool;
    VertexAttribInfo attr[16];
    uint32_t attr_count = 0;
    VertexStreamInfo stream[16];
    uint32_t stream_count = 0;
    BlendInfo blend[8];
    ColorInfo color;
    ChannelMaskInfo chmask;
    DepthStencilInfo ds;
    PolygonInfo poly;
    MultisampleInfo ms;
    float vp[16][4] = {};
    uint32_t nvp = 0;
    int32_t sc[16][4] = {};
    uint32_t nsc = 0;
    float dr[16][2] = {};
    bool depth_bounds = false;
    float db_near = 0, db_far = 1;
    uint32_t stencil_ref[2] = {0, 0}, stencil_vmask[2] = {0xff, 0xff}, stencil_wmask[2] = {0xff, 0xff};
    float blend_color[4] = {};
    float line_width = 1, point_size = 1, alpha_ref = 0;
    float bias_factor = 0, bias_units = 0, bias_clamp = 0;
    uint32_t sample_mask = 0xffffffff;
    bool rasterizer_discard = false, depth_clamp = false, prim_restart = false, render_enable = true;
    uint32_t restart_index = 0;
    // render targets
    uint32_t ncolor = 0;
    Ref<Texture> color_tex[8];
    TextureViewInfo color_view[8];
    bool color_has_view[8] = {};
    Ref<Texture> depth_tex;
    TextureViewInfo depth_view;
    bool depth_has_view = false;
    int window_origin = NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT;
    int depth_mode = NVN_DEPTH_MODE_NEAR_IS_MINUS_W;
};

GState S;

struct Target {
    Texture* tex = nullptr;
    uint32_t level = 0, layer = 0, layers = 1;
    VkFormat view_format = VK_FORMAT_UNDEFINED;
    bool operator==(const Target& o) const { return tex == o.tex && level == o.level && layer == o.layer && layers == o.layers && view_format == o.view_format; }
};

struct Pass {
    bool open = false;
    uint32_t ncolor = 0;
    Target color[8];
    Target depth;
    VkExtent2D extent = {0, 0};
    VkFormat color_fmt[8] = {};
    VkFormat depth_fmt = VK_FORMAT_UNDEFINED;
    bool has_stencil = false;
    uint32_t samples = 1;
};

Pass P;
Frame* F = nullptr;
std::unordered_map<uint64_t, VkDescriptorSet> g_set_cache;
VkPipeline g_bound_pipeline = VK_NULL_HANDLE;
UploadAlloc g_support;
MaskClear g_mask;
bool g_mask_ready = false, g_mask_ok = false;

void log_once(const char* key, const char* msg) {
    static std::unordered_map<std::string, bool> seen;
    if (seen.emplace(key, true).second) NXLOG_W(kTag, "%s", msg);
}

// ---- helpers ----------------------------------------------------------------------------------------------------------------
VkFormat scaled_for(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_UINT: return VK_FORMAT_R8_USCALED;
        case VK_FORMAT_R8G8_UINT: return VK_FORMAT_R8G8_USCALED;
        case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8_USCALED;
        case VK_FORMAT_R8G8B8A8_UINT: return VK_FORMAT_R8G8B8A8_USCALED;
        case VK_FORMAT_R16_UINT: return VK_FORMAT_R16_USCALED;
        case VK_FORMAT_R16G16_UINT: return VK_FORMAT_R16G16_USCALED;
        case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16_USCALED;
        case VK_FORMAT_R16G16B16A16_UINT: return VK_FORMAT_R16G16B16A16_USCALED;
        case VK_FORMAT_A2B10G10R10_UINT_PACK32: return VK_FORMAT_A2B10G10R10_USCALED_PACK32;
        case VK_FORMAT_R8_SINT: return VK_FORMAT_R8_SSCALED;
        case VK_FORMAT_R8G8_SINT: return VK_FORMAT_R8G8_SSCALED;
        case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8_SSCALED;
        case VK_FORMAT_R8G8B8A8_SINT: return VK_FORMAT_R8G8B8A8_SSCALED;
        case VK_FORMAT_R16_SINT: return VK_FORMAT_R16_SSCALED;
        case VK_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SSCALED;
        case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16_SSCALED;
        case VK_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SSCALED;
        case VK_FORMAT_A2B10G10R10_SINT_PACK32: return VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
        default: return f;
    }
}

bool is_int_format(VkFormat f) { return scaled_for(f) != f; }

VkImageViewType view_type_for(const DescriptorBinding& b) {
    const ImageShape& s = b.image;
    switch (s.dim) {
        case 0: return s.arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
        case 1: return s.arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        case 2: return VK_IMAGE_VIEW_TYPE_3D;
        case 3: return s.arrayed ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
        default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}

// ---- passes ----------------------------------------------------------------------------------------------------------------
void end_pass() {
    if (!P.open) return;
    vkCmdEndRendering(F->cb);
    P.open = false;
    memory_barrier(F->cb);
    g_bound_pipeline = VK_NULL_HANDLE;
}

Target make_target(Texture* t, const TextureViewInfo& v, bool has_view) {
    Target tg;
    tg.tex = t;
    if (has_view) {
        if (v.has_levels) tg.level = static_cast<uint32_t>(v.min_level);
        if (v.has_layers) {
            tg.layer = static_cast<uint32_t>(v.min_layer);
            tg.layers = std::max(1, v.num_layers);
        }
        if (v.has_format) {
            const FormatInfo* fi = format_info(v.format);
            if (fi && fi->vk != VK_FORMAT_UNDEFINED) tg.view_format = fi->vk;
        }
    }
    return tg;
}

bool targets_match() {
    if (P.ncolor != S.ncolor) return false;
    for (uint32_t i = 0; i < S.ncolor; ++i)
        if (!(P.color[i] == make_target(S.color_tex[i].get(), S.color_view[i], S.color_has_view[i]))) return false;
    return P.depth == make_target(S.depth_tex.get(), S.depth_view, S.depth_has_view);
}

// Starts a pass on the current render targets if none is open for them. False when there is nothing to render to.
bool ensure_pass() {
    if (P.open && targets_match()) return true;
    end_pass();
    P = Pass();
    VkRenderingAttachmentInfo colors[8] = {};
    VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo stencil = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    bool any = false;
    VkExtent2D extent = {UINT32_MAX, UINT32_MAX};
    P.ncolor = S.ncolor;
    for (uint32_t i = 0; i < S.ncolor; ++i) {
        colors[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        Texture* t = S.color_tex[i].get();
        P.color[i] = make_target(t, S.color_view[i], S.color_has_view[i]);
        if (!t || !tex_gpu(t)) continue;
        const bool fresh = tex_layout(t) == VK_IMAGE_LAYOUT_UNDEFINED;
        tex_transition(t, *F, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        tex_mark_gpu_written(t);
        VkImageView v = tex_attachment_view(t, P.color[i].level, P.color[i].layer, P.color[i].layers, P.color[i].view_format);
        if (!v) continue;
        colors[i].imageView = v;
        colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[i].loadOp = fresh ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        P.color_fmt[i] = P.color[i].view_format != VK_FORMAT_UNDEFINED ? P.color[i].view_format : tex_vk_format(t);
        const VkExtent3D e = tex_extent(t, P.color[i].level);
        extent.width = std::min(extent.width, e.width);
        extent.height = std::min(extent.height, e.height);
        any = true;
    }
    Texture* dt = S.depth_tex.get();
    P.depth = make_target(dt, S.depth_view, S.depth_has_view);
    bool have_depth = false;
    if (dt && tex_gpu(dt)) {
        const bool fresh = tex_layout(dt) == VK_IMAGE_LAYOUT_UNDEFINED;
        tex_transition(dt, *F, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        tex_mark_gpu_written(dt);
        VkImageView v = tex_attachment_view(dt, P.depth.level, P.depth.layer, P.depth.layers, P.depth.view_format);
        if (v) {
            depth.imageView = stencil.imageView = v;
            depth.imageLayout = stencil.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth.loadOp = stencil.loadOp = fresh ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            depth.storeOp = stencil.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depth.clearValue.depthStencil = {1.0f, 0};
            stencil.clearValue.depthStencil = {1.0f, 0};
            P.depth_fmt = tex_vk_format(dt);
            P.has_stencil = (tex_aspect(dt) & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
            const VkExtent3D e = tex_extent(dt, P.depth.level);
            extent.width = std::min(extent.width, e.width);
            extent.height = std::min(extent.height, e.height);
            have_depth = true;
            any = true;
        }
    }
    if (!any || extent.width == UINT32_MAX) return false;
    P.extent = extent;
    P.samples = std::max<uint32_t>(1, S.color_tex[0] ? static_cast<uint32_t>(S.color_tex[0]->b.samples) : 1);
    if (P.samples == 0) P.samples = 1;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    uint32_t last = 0;
    for (uint32_t i = 0; i < S.ncolor; ++i)
        if (colors[i].imageView) last = i + 1;
    ri.colorAttachmentCount = S.ncolor;
    ri.pColorAttachments = colors;
    if (have_depth) {
        ri.pDepthAttachment = &depth;
        if (P.has_stencil) ri.pStencilAttachment = &stencil;
    }
    (void)last;
    vkCmdBeginRendering(F->cb, &ri);
    P.open = true;
    counters().passes.fetch_add(1, std::memory_order_relaxed);
    g_bound_pipeline = VK_NULL_HANDLE;
    return true;
}

// ---- viewport conversion -------------------------------------------------------------------------------------------------
VkRect2D scissor_rect(const int32_t* r) {
    int32_t x = r[0], y = r[1], w = r[2], h = r[3];
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    w = std::max(0, std::min<int32_t>(w, static_cast<int32_t>(P.extent.width) - x));
    h = std::max(0, std::min<int32_t>(h, static_cast<int32_t>(P.extent.height) - y));
    return {{x, y}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
}

void set_viewports_and_scissors() {
    VkCommandBuffer cb = F->cb;
    VkViewport vp[16];
    VkRect2D sc[16];
    const uint32_t nvp = S.nvp ? S.nvp : 1;
    for (uint32_t i = 0; i < nvp; ++i) {
        float x = S.vp[i][0], y = S.vp[i][1], w = S.vp[i][2], h = S.vp[i][3];
        if (!S.nvp) { x = 0; y = 0; w = static_cast<float>(P.extent.width); h = static_cast<float>(P.extent.height); }
        // Upper-left origin: window y is the image row from the top and NDC +y is the top edge of the rectangle (negative height).
        // Lower-left origin (the GL convention): window y 0 is memory row 0 and NDC -y is the first row, so no flip.
        const bool lower_left = S.window_origin == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT;
        vp[i] = lower_left ? VkViewport{x, y, w, h, S.dr[i][0], S.dr[i][1] == 0.0f && S.dr[i][0] == 0.0f ? 1.0f : S.dr[i][1]}
                           : VkViewport{x, y + h, w, -h, S.dr[i][0], S.dr[i][1] == 0.0f && S.dr[i][0] == 0.0f ? 1.0f : S.dr[i][1]};
        if (i < S.nsc) sc[i] = scissor_rect(S.sc[i]);
        else sc[i] = {{0, 0}, P.extent};
    }
    vkCmdSetViewportWithCount(cb, nvp, vp);
    vkCmdSetScissorWithCount(cb, nvp, sc);
}

// ---- descriptor sets ---------------------------------------------------------------------------------------------------------
struct TexResolved {
    Texture* tex = nullptr;
    TextureViewInfo view;
    bool has_view = false;
    SamplerInfo smp;
    bool has_sampler = false;
};

uint32_t read_handle_word(int stage, int bank, int word) {
    // a texture handle the game wrote into uniform block (bank - 3)
    const int ub = bank - 3;
    if (ub < 0 || ub >= 16) return 0;
    const GState::Bind& b = S.ubo[stage][ub];
    if (!b.addr || static_cast<uint64_t>(word) * 4 + 4 > b.size) return 0;
    uint32_t v = 0;
    std::memcpy(&v, host_ptr(b.addr) + static_cast<size_t>(word) * 4, 4);
    return v;
}

TexResolved resolve_texture(const BindingDesc& d) {
    TexResolved r;
    const DescriptorBinding& b = d.shader->refl.bindings[d.index];
    uint64_t handle = 0;
    if (b.nvn.source == NvnSource::SamplerBinding) {
        const int idx = b.nvn.index;
        if (idx >= 0 && idx < 64) handle = S.tex[d.nvn_stage][idx];
    } else if (b.nvn.source == NvnSource::HandleInUniform) {
        handle = read_handle_word(d.nvn_stage, b.nvn.bank, b.nvn.word);
    }
    const uint32_t tid = handle_texture_id(handle), sid = handle_sampler_id(handle);
    if (S.tpool && static_cast<int>(tid) < S.tpool->count) {
        std::shared_lock<std::shared_mutex> lk(S.tpool->lock);
        const TexturePool::Entry& e = S.tpool->entries[tid];
        if (e.used && e.tex) {
            r.tex = e.tex.get();
            r.view = e.view;
            r.has_view = e.has_view;
        }
    }
    if (S.spool && static_cast<int>(sid) < S.spool->count) {
        std::shared_lock<std::shared_mutex> lk(S.spool->lock);
        if (S.spool->used[sid]) {
            r.smp = S.spool->entries[sid];
            r.has_sampler = true;
        }
    }
    return r;
}

ViewRequest make_request(const DescriptorBinding& b, const TexResolved& r) {
    ViewRequest q;
    q.type = view_type_for(b);
    q.depth_compare = b.image.depth;
    const Texture& t = *r.tex;
    int sw[4] = {t.b.swizzle[0], t.b.swizzle[1], t.b.swizzle[2], t.b.swizzle[3]};
    int dsmode = t.b.depth_stencil_mode;
    if (r.has_view) {
        const TextureViewInfo& v = r.view;
        if (v.has_levels) { q.base_level = static_cast<uint32_t>(v.min_level); q.level_count = static_cast<uint32_t>(v.num_levels); }
        if (v.has_layers) { q.base_layer = static_cast<uint32_t>(v.min_layer); q.layer_count = static_cast<uint32_t>(v.num_layers); }
        if (v.has_format) {
            const FormatInfo* fi = format_info(v.format);
            if (fi) q.format = fi->vk;
        }
        if (v.has_swizzle) for (int i = 0; i < 4; ++i) sw[i] = v.swizzle[i];
        if (v.has_depth_stencil_mode) dsmode = v.depth_stencil_mode;
    }
    q.swizzle.r = vk_swizzle(sw[0]);
    q.swizzle.g = vk_swizzle(sw[1]);
    q.swizzle.b = vk_swizzle(sw[2]);
    q.swizzle.a = vk_swizzle(sw[3]);
    q.depth_aspect = dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH;
    return q;
}

// Makes the textures a draw samples current (uploaded, in SHADER_READ_ONLY layout). Ends the pass when it has to touch images.
void prepare_textures(const ProgLayout& pl) {
    std::vector<Texture*> need;
    for (const BindingDesc& d : pl.bindings) {
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (b.nvn.source != NvnSource::SamplerBinding && b.nvn.source != NvnSource::HandleInUniform) continue;
        TexResolved r = resolve_texture(d);
        if (!r.tex) continue;
        if (!tex_gpu(r.tex)) continue;
        if (r.tex->cpu_dirty.load() || tex_layout(r.tex) != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) need.push_back(r.tex);
    }
    if (need.empty()) return;
    end_pass();
    counters().pass_breaks.fetch_add(1, std::memory_order_relaxed);
    for (Texture* t : need) {
        tex_ensure_current(t, *F);
        tex_transition(t, *F, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

int storage_slot_for(int nvn_stage, int c0_offset) {
    // c[0] storage buffer descriptors (byte offsets, as the translator reports them): 0x110 + 0x100 * hardware stage + 0x10 * binding
    // with hardware stage 0 vertex, 3 geometry, 4 fragment; compute: 0x310 + 0x10 * binding. Checked against the translated shaders of the
    // game (vertex 0x110, geometry 0x410, fragment 0x510, compute 0x310 and 0x320).
    int base = 0;
    switch (nvn_stage) {
        case kVertex: base = 0x110; break;
        case kGeometry: base = 0x410; break;
        case kFragment: base = 0x510; break;
        case kCompute: base = 0x310; break;
        default: return 0;
    }
    if (c0_offset < base) return 0;
    return (c0_offset - base) / 0x10;
}

VkDescriptorSet build_set(const ProgLayout& pl, uint32_t set) {
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> binfo;
    std::vector<VkDescriptorImageInfo> iinfo;
    const size_t nb = pl.bindings.size();
    binfo.reserve(nb);
    iinfo.reserve(nb);
    uint64_t key = 1469598103934665603ull + set;
    auto mix = [&](uint64_t v) { key = (key ^ v) * 1099511628211ull; };
    mix(reinterpret_cast<uint64_t>(pl.layout));
    for (const BindingDesc& d : pl.bindings) {
        if (d.set != set) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = d.binding;
        w.descriptorCount = 1;
        w.descriptorType = d.type;
        if (d.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || d.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            VkDescriptorBufferInfo bi = {dummy_buffer(), 0, 65536};
            switch (b.nvn.source) {
                case NvnSource::SupportBuffer:
                    bi = {g_support.buffer, g_support.offset, sizeof(SupportBuffer)};
                    break;
                case NvnSource::DriverConstants: {
                    VkDeviceSize range = 0;
                    VkBuffer buf = driver_constants_buffer(&range);
                    if (buf) bi = {buf, 0, range};
                    break;
                }
                case NvnSource::ConstantData: {
                    VkDeviceSize range = 0;
                    VkBuffer buf = constant_data_buffer(d.shader, &range);
                    if (buf) bi = {buf, 0, range};
                    break;
                }
                case NvnSource::UniformBlock: {
                    const int idx = b.nvn.index;
                    if (idx >= 0 && idx < 16 && S.ubo[d.nvn_stage][idx].addr) {
                        const GState::Bind& bind = S.ubo[d.nvn_stage][idx];
                        BufRef br = resolve_buffer(bind.addr, bind.size);
                        const VkDeviceSize align = device().properties().limits.minUniformBufferOffsetAlignment;
                        if (br.ok && (br.offset % align) == 0) {
                            bi = {br.buffer, br.offset, std::min<VkDeviceSize>({bind.size ? bind.size : 65536, br.avail, 65536})};
                        } else if (br.ok) {
                            // unaligned binding: copy into the upload ring
                            const VkDeviceSize n = std::min<VkDeviceSize>(bind.size ? bind.size : 65536, 65536);
                            UploadAlloc u = F->upload(n, std::max<VkDeviceSize>(align, 256));
                            if (u) {
                                std::memcpy(u.ptr, host_ptr(bind.addr), n);
                                bi = {u.buffer, u.offset, n};
                            }
                        }
                    }
                    break;
                }
                case NvnSource::StorageBuffer: {
                    const int slot = storage_slot_for(d.nvn_stage, b.nvn.c0_offset);
                    if (slot >= 0 && slot < 16 && S.ssbo[d.nvn_stage][slot].addr) {
                        const GState::Bind& bind = S.ssbo[d.nvn_stage][slot];
                        BufRef br = resolve_buffer(bind.addr, bind.size);
                        bi = {br.buffer, br.offset, std::min<VkDeviceSize>(bind.size ? bind.size : br.avail, br.avail)};
                    }
                    break;
                }
                default:
                    note_unsupported("shader buffer binding of an unknown NVN source");
                    break;
            }
            binfo.push_back(bi);
            w.pBufferInfo = &binfo.back();
            mix(reinterpret_cast<uint64_t>(bi.buffer)); mix(bi.offset); mix(bi.range);
        } else if (d.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            VkDescriptorImageInfo ii = {dummy_sampler(), dummy_view(view_type_for(b), b.image.depth), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            TexResolved r = resolve_texture(d);
            if (r.tex && tex_gpu(r.tex)) {
                VkImageView v = tex_sampled_view(r.tex, make_request(b, r));
                if (v) ii.imageView = v;
                if (r.has_sampler) ii.sampler = sampler_for(r.smp);
            }
            iinfo.push_back(ii);
            w.pImageInfo = &iinfo.back();
            mix(reinterpret_cast<uint64_t>(ii.imageView)); mix(reinterpret_cast<uint64_t>(ii.sampler));
        } else {
            note_unsupported("storage image / texel buffer descriptors");
            VkDescriptorImageInfo ii = {VK_NULL_HANDLE, dummy_view(VK_IMAGE_VIEW_TYPE_2D, false), VK_IMAGE_LAYOUT_GENERAL};
            iinfo.push_back(ii);
            w.pImageInfo = &iinfo.back();
            continue;
        }
        writes.push_back(w);
    }
    auto it = g_set_cache.find(key);
    if (it != g_set_cache.end()) return it->second;
    VkDescriptorSet ds = F->alloc_set(pl.sets[set]);
    if (!ds) return VK_NULL_HANDLE;
    for (auto& w : writes) w.dstSet = ds;
    vkUpdateDescriptorSets(vkdev(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    g_set_cache[key] = ds;
    return ds;
}

void bind_sets(const ProgLayout& pl, VkPipelineBindPoint bp) {
    for (uint32_t set = 0; set < 4; ++set) {
        if (!pl.has_set[set]) continue;
        VkDescriptorSet ds = build_set(pl, set);
        if (ds) vkCmdBindDescriptorSets(F->cb, bp, pl.layout, set, 1, &ds, 0, nullptr);
    }
}

// ---- draws -------------------------------------------------------------------------------------------------------------------
bool build_key(GraphicsKey& key, VkPrimitiveTopology topo) {
    std::memset(&key, 0, sizeof key);
    key.stage[0] = S.stage[kVertex];
    key.stage[1] = S.stage[kFragment];
    key.stage[2] = S.stage[kGeometry];
    const ShaderInfo* vs = key.stage[0];
    // attributes: those the vertex shader reads
    for (const ShaderIo& in : vs->refl.inputs) {
        if (in.location >= 16 || in.location >= S.attr_count) continue;
        const VertexAttribInfo& a = S.attr[in.location];
        VkFormat f = vertex_format(a.format);
        if (f == VK_FORMAT_UNDEFINED) continue;
        if (in.type == ScalarType::Float && is_int_format(f)) f = scaled_for(f);
        else if ((in.type == ScalarType::Uint || in.type == ScalarType::Sint) && !is_int_format(f))
            log_once("attr-type-mismatch", "a vertex attribute with a float format feeds an integer shader input (needs a SPIR-V variant)");
        if (key.attr_count >= 16) break;
        KeyAttr& k = key.attr[key.attr_count++];
        k.location = static_cast<uint8_t>(in.location);
        k.binding = static_cast<uint8_t>(a.stream & 15);
        k.offset = static_cast<uint16_t>(a.relative_offset);
        k.format = f;
        key.binding_instance[k.binding] = S.stream[k.binding].divisor > 0 ? 1 : 0;
    }
    key.color_count = P.ncolor;
    for (uint32_t i = 0; i < P.ncolor && i < 8; ++i) {
        key.color_format[i] = P.color_fmt[i];
        key.blend_enable[i] = (S.color.blend_enable_mask >> i) & 1;
        const BlendInfo& b = S.blend[i];
        key.blend_src_rgb[i] = b.src_rgb ? vk_blend_factor(b.src_rgb) : VK_BLEND_FACTOR_ONE;
        key.blend_dst_rgb[i] = b.dst_rgb ? vk_blend_factor(b.dst_rgb) : VK_BLEND_FACTOR_ZERO;
        key.blend_src_a[i] = b.src_a ? vk_blend_factor(b.src_a) : VK_BLEND_FACTOR_ONE;
        key.blend_dst_a[i] = b.dst_a ? vk_blend_factor(b.dst_a) : VK_BLEND_FACTOR_ZERO;
        key.blend_op_rgb[i] = b.eq_rgb ? vk_blend_op(b.eq_rgb) : VK_BLEND_OP_ADD;
        key.blend_op_a[i] = b.eq_a ? vk_blend_op(b.eq_a) : VK_BLEND_OP_ADD;
        key.write_mask[i] = S.chmask.mask[i] & 0xf;
        if (P.color_fmt[i] == VK_FORMAT_UNDEFINED) key.write_mask[i] = 0;
    }
    key.depth_format = P.depth_fmt;
    key.stencil_format = P.has_stencil ? P.depth_fmt : VK_FORMAT_UNDEFINED;
    key.samples = P.samples;
    key.topology = static_cast<uint8_t>(topo);
    key.polygon_mode = static_cast<uint8_t>(vk_polygon_mode(S.poly.polygon_mode));
    if (key.polygon_mode != VK_POLYGON_MODE_FILL && !device().features().fill_mode_non_solid) key.polygon_mode = VK_POLYGON_MODE_FILL;
    key.depth_clamp = S.depth_clamp && device().features().depth_clamp;
    key.alpha_to_coverage = S.ms.alpha_to_coverage && S.ms.enable ? 1 : 0;
    key.depth_neg_one = S.depth_mode == NVN_DEPTH_MODE_NEAR_IS_MINUS_W ? 1 : 0;
    if (S.color.logic_op != 0 && S.color.logic_op != NVN_LOGIC_OP_COPY && device().features().logic_op) {
        key.logic_op_enable = 1;
        key.logic_op = static_cast<uint8_t>(vk_logic_op(S.color.logic_op));
        for (uint32_t i = 0; i < 8; ++i) key.blend_enable[i] = 0;
    }
    if (S.color.alpha_test != 0 && S.color.alpha_test != NVN_ALPHA_FUNC_ALWAYS) log_once("alpha-test", "alpha test is enabled by the game: not implemented (needs a shader variant)");
    return true;
}

void set_dynamic_state(VkPrimitiveTopology topo, bool indexed, int index_type) {
    VkCommandBuffer cb = F->cb;
    set_viewports_and_scissors();
    vkCmdSetCullMode(cb, vk_cull_mode(S.poly.cull_face));
    {
        VkFrontFace ff = vk_front_face(S.poly.front_face);
        if (S.window_origin == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT) ff = ff == VK_FRONT_FACE_CLOCKWISE ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;   // unflipped viewport
        vkCmdSetFrontFace(cb, ff);
    }
    vkCmdSetPrimitiveTopology(cb, topo);
    const bool have_depth = P.depth_fmt != VK_FORMAT_UNDEFINED;
    vkCmdSetDepthTestEnable(cb, have_depth && S.ds.depth_test);
    vkCmdSetDepthWriteEnable(cb, have_depth && S.ds.depth_test && S.ds.depth_write);
    vkCmdSetDepthCompareOp(cb, vk_compare_op(S.ds.depth_func ? S.ds.depth_func : NVN_DEPTH_FUNC_LESS));
    vkCmdSetDepthBoundsTestEnable(cb, have_depth && S.depth_bounds && device().features().depth_bounds);
    if (S.depth_bounds && device().features().depth_bounds) vkCmdSetDepthBounds(cb, S.db_near, S.db_far);
    vkCmdSetStencilTestEnable(cb, P.has_stencil && S.ds.stencil_test);
    for (int f = 0; f < 2; ++f) {
        const VkStencilFaceFlags face = f == 0 ? VK_STENCIL_FACE_FRONT_BIT : VK_STENCIL_FACE_BACK_BIT;
        vkCmdSetStencilOp(cb, face, vk_stencil_op(S.ds.stencil_fail[f]), vk_stencil_op(S.ds.depth_pass[f]), vk_stencil_op(S.ds.depth_fail[f]),
                          vk_compare_op(S.ds.stencil_func[f] ? S.ds.stencil_func[f] : NVN_STENCIL_FUNC_ALWAYS));
        vkCmdSetStencilReference(cb, face, S.stencil_ref[f]);
        vkCmdSetStencilCompareMask(cb, face, S.stencil_vmask[f]);
        vkCmdSetStencilWriteMask(cb, face, S.stencil_wmask[f]);
    }
    const int off = S.poly.offset_enables;
    const int mode = S.poly.polygon_mode;
    const bool bias = (mode == NVN_POLYGON_MODE_FILL && (off & NVN_POLYGON_OFFSET_ENABLE_FILL)) ||
                      (mode == NVN_POLYGON_MODE_LINE && (off & NVN_POLYGON_OFFSET_ENABLE_LINE)) ||
                      (mode == NVN_POLYGON_MODE_POINT && (off & NVN_POLYGON_OFFSET_ENABLE_POINT));
    vkCmdSetDepthBiasEnable(cb, bias);
    vkCmdSetDepthBias(cb, S.bias_units, device().features().depth_bias_clamp ? S.bias_clamp : 0.0f, S.bias_factor);
    vkCmdSetRasterizerDiscardEnable(cb, S.rasterizer_discard);
    const bool restart = S.prim_restart && indexed &&
                         ((index_type == NVN_INDEX_TYPE_UNSIGNED_INT && S.restart_index == 0xffffffffu) || (index_type == NVN_INDEX_TYPE_UNSIGNED_SHORT && S.restart_index == 0xffffu));
    vkCmdSetPrimitiveRestartEnable(cb, restart);
    vkCmdSetBlendConstants(cb, S.blend_color);
    vkCmdSetLineWidth(cb, device().features().wide_lines ? std::max(1.0f, S.line_width) : 1.0f);
}

// Prepares everything a draw needs and binds it. False: skip the draw.
bool begin_draw(int mode, bool indexed, int index_type, VkPrimitiveTopology* topo_out) {
    const ShaderInfo* vs = S.stage[kVertex];
    if (!vs) { log_once("draw-no-vs", "draw without a vertex shader skipped"); return false; }
    if (!S.render_enable) return false;
    const ShaderInfo* stages[6] = {vs, S.stage[kFragment], S.stage[kGeometry], nullptr, nullptr, nullptr};
    const ProgLayout* pl = prog_layout(stages);
    if (!pl) return false;
    prepare_textures(*pl);
    if (!ensure_pass()) { log_once("draw-no-target", "draw without a usable render target skipped"); return false; }
    VkPrimitiveTopology topo = vk_topology(mode);
    GraphicsKey key;
    build_key(key, topo);
    VkPipeline pipe = graphics_pipeline(key, *pl);
    if (!pipe) return false;
    if (pipe != g_bound_pipeline) {
        vkCmdBindPipeline(F->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        g_bound_pipeline = pipe;
    }
    set_dynamic_state(topo, indexed, index_type);
    bind_sets(*pl, VK_PIPELINE_BIND_POINT_GRAPHICS);
    // vertex buffers, with the strides of the bound streams
    for (uint32_t i = 0; i < key.attr_count; ++i) {
        const uint32_t binding = key.attr[i].binding;
        bool done = false;
        for (uint32_t j = 0; j < i; ++j) if (key.attr[j].binding == binding) done = true;
        if (done) continue;
        const GState::Bind& vb = S.vtx[binding];
        BufRef br = vb.addr ? resolve_buffer(vb.addr, vb.size) : BufRef{dummy_buffer(), 0, 65536, false};
        VkDeviceSize size = vb.size ? std::min<VkDeviceSize>(vb.size, br.avail) : br.avail;
        VkDeviceSize stride = static_cast<VkDeviceSize>(S.stream[binding].stride);
        vkCmdBindVertexBuffers2(F->cb, binding, 1, &br.buffer, &br.offset, &size, &stride);
    }
    *topo_out = topo;
    return true;
}

void do_draw_arrays(const P_DrawArrays& p) {
    VkPrimitiveTopology topo;
    if (!begin_draw(static_cast<int>(p.mode), false, 0, &topo)) { counters().draws_skipped.fetch_add(1); return; }
    const PrimConversion conv = prim_conversion(static_cast<int>(p.mode));
    if (conv == PrimConversion::None) {
        vkCmdDraw(F->cb, p.count, std::max<uint32_t>(1, p.instance_count), p.first, p.base_instance);
    } else {
        // QUADS / QUAD_STRIP / POLYGON / LINE_LOOP: generate an index buffer
        std::vector<uint32_t> idx;
        const uint32_t n = p.count;
        switch (conv) {
            case PrimConversion::Quads:
                for (uint32_t q = 0; q + 3 < n; q += 4) { const uint32_t b = p.first + q; idx.insert(idx.end(), {b, b + 1, b + 2, b, b + 2, b + 3}); }
                break;
            case PrimConversion::QuadStrip:
                for (uint32_t q = 0; q + 3 < n; q += 2) { const uint32_t b = p.first + q; idx.insert(idx.end(), {b, b + 1, b + 2, b + 2, b + 1, b + 3}); }
                break;
            case PrimConversion::Polygon:
                for (uint32_t k = 1; k + 1 < n; ++k) idx.insert(idx.end(), {p.first, p.first + k, p.first + k + 1});
                break;
            case PrimConversion::LineLoop:
                for (uint32_t k = 0; k < n; ++k) idx.push_back(p.first + k);
                if (n) idx.push_back(p.first);
                break;
            default: break;
        }
        if (idx.empty()) return;
        UploadAlloc u = F->upload(idx.size() * 4, 4);
        if (!u) return;
        std::memcpy(u.ptr, idx.data(), idx.size() * 4);
        vkCmdBindIndexBuffer(F->cb, u.buffer, u.offset, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(F->cb, static_cast<uint32_t>(idx.size()), std::max<uint32_t>(1, p.instance_count), 0, 0, p.base_instance);
    }
    counters().draws.fetch_add(1, std::memory_order_relaxed);
}

void do_draw_elements(const P_DrawElements& p) {
    VkPrimitiveTopology topo;
    if (!begin_draw(static_cast<int>(p.mode), true, static_cast<int>(p.type), &topo)) { counters().draws_skipped.fetch_add(1); return; }
    const uint32_t isz = index_size_bytes(static_cast<int>(p.type));
    const PrimConversion conv = prim_conversion(static_cast<int>(p.mode));
    const uint8_t* src = host_ptr(p.index_address);
    if (conv != PrimConversion::None || isz == 1) {
        // widen to 32 bit and/or convert the primitive
        std::vector<uint32_t> in(p.count);
        for (uint32_t i = 0; i < p.count; ++i) {
            uint32_t v = 0;
            if (isz == 1) v = src[i];
            else if (isz == 2) { uint16_t t; std::memcpy(&t, src + i * 2, 2); v = t; }
            else std::memcpy(&v, src + i * 4, 4);
            in[i] = v;
        }
        std::vector<uint32_t> idx;
        switch (conv) {
            case PrimConversion::Quads:
                for (uint32_t q = 0; q + 3 < p.count; q += 4) idx.insert(idx.end(), {in[q], in[q + 1], in[q + 2], in[q], in[q + 2], in[q + 3]});
                break;
            case PrimConversion::QuadStrip:
                for (uint32_t q = 0; q + 3 < p.count; q += 2) idx.insert(idx.end(), {in[q], in[q + 1], in[q + 2], in[q + 2], in[q + 1], in[q + 3]});
                break;
            case PrimConversion::Polygon:
                for (uint32_t k = 1; k + 1 < p.count; ++k) idx.insert(idx.end(), {in[0], in[k], in[k + 1]});
                break;
            case PrimConversion::LineLoop:
                idx = in;
                if (!in.empty()) idx.push_back(in[0]);
                break;
            default: idx = in; break;
        }
        if (idx.empty()) return;
        UploadAlloc u = F->upload(idx.size() * 4, 4);
        if (!u) return;
        std::memcpy(u.ptr, idx.data(), idx.size() * 4);
        vkCmdBindIndexBuffer(F->cb, u.buffer, u.offset, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(F->cb, static_cast<uint32_t>(idx.size()), std::max<uint32_t>(1, p.instance_count), 0, p.base_vertex, p.base_instance);
    } else {
        BufRef br = resolve_buffer(p.index_address, static_cast<uint64_t>(p.count) * isz);
        vkCmdBindIndexBuffer(F->cb, br.buffer, br.offset, vk_index_type(static_cast<int>(p.type)));
        vkCmdDrawIndexed(F->cb, p.count, std::max<uint32_t>(1, p.instance_count), 0, p.base_vertex, p.base_instance);
    }
    counters().draws.fetch_add(1, std::memory_order_relaxed);
}

void do_draw_indirect(const P_DrawIndirect& p) {
    const bool indexed = p.type != 0xffffffffu;
    VkPrimitiveTopology topo;
    if (!begin_draw(static_cast<int>(p.mode), indexed, indexed ? static_cast<int>(p.type) : 0, &topo)) { counters().draws_skipped.fetch_add(1); return; }
    if (prim_conversion(static_cast<int>(p.mode)) != PrimConversion::None) { note_unsupported("indirect draw of a quad/polygon/line-loop primitive"); return; }
    const uint64_t cmd_size = indexed ? 20 : 16;
    BufRef ib = resolve_buffer(p.indirect_address, cmd_size);
    if (indexed) {
        if (index_size_bytes(static_cast<int>(p.type)) == 1) { note_unsupported("indirect indexed draw with 8-bit indices"); return; }
        BufRef xb = resolve_buffer(p.index_address, 4);
        vkCmdBindIndexBuffer(F->cb, xb.buffer, xb.offset, vk_index_type(static_cast<int>(p.type)));
    }
    if (p.multi && p.count_address) {
        BufRef cb = resolve_buffer(p.count_address, 4);
        const uint32_t stride = static_cast<uint32_t>(p.stride ? p.stride : cmd_size);
        if (indexed) vkCmdDrawIndexedIndirectCount(F->cb, ib.buffer, ib.offset, cb.buffer, cb.offset, p.max_count, stride);
        else vkCmdDrawIndirectCount(F->cb, ib.buffer, ib.offset, cb.buffer, cb.offset, p.max_count, stride);
    } else if (indexed) {
        vkCmdDrawIndexedIndirect(F->cb, ib.buffer, ib.offset, 1, 20);
    } else {
        vkCmdDrawIndirect(F->cb, ib.buffer, ib.offset, 1, 16);
    }
    counters().draws.fetch_add(1, std::memory_order_relaxed);
}

void do_dispatch(uint32_t x, uint32_t y, uint32_t z, uint64_t indirect) {
    const ShaderInfo* cs = S.stage[kCompute];
    if (!cs) return;
    const ShaderInfo* stages[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, cs};
    const ProgLayout* pl = prog_layout(stages);
    if (!pl) return;
    prepare_textures(*pl);
    end_pass();
    VkPipeline pipe = compute_pipeline(cs, *pl);
    if (!pipe) return;
    vkCmdBindPipeline(F->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    g_bound_pipeline = VK_NULL_HANDLE;
    bind_sets(*pl, VK_PIPELINE_BIND_POINT_COMPUTE);
    if (indirect) {
        BufRef b = resolve_buffer(indirect, 12);
        vkCmdDispatchIndirect(F->cb, b.buffer, b.offset);
    } else {
        vkCmdDispatch(F->cb, x, y, z);
    }
    memory_barrier(F->cb);
    counters().dispatches.fetch_add(1, std::memory_order_relaxed);
}

// ---- clears -------------------------------------------------------------------------------------------------------------------
VkClearRect full_rect() {
    VkClearRect r = {};
    r.rect = S.nsc ? scissor_rect(S.sc[0]) : VkRect2D{{0, 0}, P.extent};
    r.baseArrayLayer = 0;
    r.layerCount = 1;
    return r;
}

void do_clear_color(const P_ClearColor& p) {
    if (p.target >= 8 || !S.color_tex[p.target]) return;
    if (p.mask == 0) return;
    if (!ensure_pass()) return;
    if (!P.color_fmt[p.target]) return;
    if (p.mask != 0xf) {
        // vkCmdClearAttachments ignores the colour write mask: a masked clear is a draw with that mask
        if (!g_mask_ready) { g_mask_ok = g_mask.init(device()); g_mask_ready = true; }
        const VkClearRect cr = full_rect();
        if (g_mask_ok && g_mask.record(F->cb, P.color_fmt[p.target], P.ncolor, p.target, p.mask, p.kind, p.value, cr.rect, P.samples)) {
            counters().clears.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        note_unsupported("ClearColor with a partial channel mask (all channels are cleared)");
    }
    VkClearAttachment ca = {};
    ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ca.colorAttachment = p.target;
    std::memcpy(ca.clearValue.color.uint32, p.value, 16);
    VkClearRect r = full_rect();
    vkCmdClearAttachments(F->cb, 1, &ca, 1, &r);
    counters().clears.fetch_add(1, std::memory_order_relaxed);
}

void do_clear_depth_stencil(const P_ClearDepthStencil& p) {
    if (!S.depth_tex) return;
    if (!ensure_pass()) return;
    VkClearAttachment ca = {};
    if (p.depth_mask) ca.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (p.stencil_mask && P.has_stencil) ca.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!ca.aspectMask) return;
    if (p.stencil_mask && p.stencil_mask != 0xff) note_unsupported("ClearDepthStencil with a partial stencil mask");
    ca.clearValue.depthStencil = {p.depth, static_cast<uint32_t>(p.stencil)};
    VkClearRect r = full_rect();
    vkCmdClearAttachments(F->cb, 1, &ca, 1, &r);
    counters().clears.fetch_add(1, std::memory_order_relaxed);
}

// ---- copies ------------------------------------------------------------------------------------------------------------------
struct SubRes { uint32_t level = 0, layer = 0, layers = 1; };
SubRes sub_of(const TextureViewInfo& v, bool has_view) {
    SubRes s;
    if (has_view) {
        if (v.has_levels) s.level = static_cast<uint32_t>(v.min_level);
        if (v.has_layers) { s.layer = static_cast<uint32_t>(v.min_layer); s.layers = 1; }
    }
    return s;
}

void do_clear_texture(const P_ClearTexture& p) {
    Texture* t = p.tex;
    if (!t || !tex_gpu(t)) return;
    end_pass();
    SubRes s = sub_of(p.view, p.has_view);
    tex_ensure_current(t, *F);
    tex_transition(t, *F, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    tex_mark_gpu_written(t);
    if (p.has_region) {
        const VkExtent3D e = tex_extent(t, s.level);
        if (p.region.x != 0 || p.region.y != 0 || (p.region.w && static_cast<uint32_t>(p.region.w) < e.width) || (p.region.h && static_cast<uint32_t>(p.region.h) < e.height))
            note_unsupported("ClearTexture of a sub-region (the whole level is cleared)");
    }
    VkImageSubresourceRange rng = {tex_aspect(t), s.level, 1, s.layer, p.has_view && p.view.has_layers ? static_cast<uint32_t>(std::max(1, p.view.num_layers)) : tex_layers(t)};
    if (tex_is_depth(t)) {
        VkClearDepthStencilValue v = {};
        std::memcpy(&v.depth, &p.value[0], 4);
        vkCmdClearDepthStencilImage(F->cb, tex_image(t), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rng);
    } else {
        VkClearColorValue v = {};
        std::memcpy(v.uint32, p.value, 16);
        vkCmdClearColorImage(F->cb, tex_image(t), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rng);
    }
    counters().clears.fetch_add(1, std::memory_order_relaxed);
}

void do_copy_buffer_texture(const P_CopyBufTex& p, bool to_texture) {
    Texture* t = p.tex;
    if (!t || !tex_gpu(t)) return;
    end_pass();
    SubRes s = sub_of(p.view, p.has_view);
    const VkExtent3D ext = tex_extent(t, s.level);
    P_CopyRegion r = p.has_region ? p.region : P_CopyRegion{0, 0, 0, static_cast<int32_t>(ext.width), static_cast<int32_t>(ext.height), static_cast<int32_t>(ext.depth)};
    const FormatInfo* fi = t->fmt;
    const uint32_t bw = fi ? fi->block_w : 1, bh = fi ? fi->block_h : 1, bpb = fi ? fi->bytes : 4;
    uint32_t rw = static_cast<uint32_t>(r.w), rh = static_cast<uint32_t>(r.h), rd = std::max<int32_t>(1, r.d);
    VkBufferImageCopy c = {};
    const uint64_t row_blocks = (rw + bw - 1) / bw;
    const uint64_t row_bytes = row_blocks * bpb;
    const uint64_t stride = p.row_stride > 0 ? static_cast<uint64_t>(p.row_stride) : row_bytes;
    c.bufferRowLength = static_cast<uint32_t>(stride / bpb * bw);
    c.bufferImageHeight = p.image_stride > 0 ? static_cast<uint32_t>(static_cast<uint64_t>(p.image_stride) / stride * bh) : 0;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, s.level, s.layer, 1};
    if (tex_is_depth(t)) c.imageSubresource.aspectMask = tex_aspect(t) & VK_IMAGE_ASPECT_DEPTH_BIT;
    c.imageOffset = {r.x, r.y, r.z};
    c.imageExtent = {rw, rh, rd};
    if (t->b.target != NVN_TEXTURE_TARGET_3D && t->layers > 1 && !p.has_view) { c.imageSubresource.layerCount = tex_layers(t); c.imageExtent.depth = 1; }
    if (t->b.target != NVN_TEXTURE_TARGET_3D && rd > 1) { c.imageSubresource.layerCount = rd; c.imageSubresource.baseArrayLayer = s.layer + static_cast<uint32_t>(std::max(0, r.z)); c.imageOffset.z = 0; c.imageExtent.depth = 1; }
    BufRef br = resolve_buffer(p.address, stride * ((rh + bh - 1) / bh) * rd);
    c.bufferOffset = br.offset;
    if (to_texture) {
        tex_ensure_current(t, *F);
        tex_transition(t, *F, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        tex_mark_gpu_written(t);
        vkCmdCopyBufferToImage(F->cb, br.buffer, tex_image(t), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    } else {
        tex_ensure_current(t, *F);
        tex_transition(t, *F, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdCopyImageToBuffer(F->cb, tex_image(t), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, br.buffer, 1, &c);
    }
    memory_barrier(F->cb);
    counters().copies.fetch_add(1, std::memory_order_relaxed);
}

void do_copy_texture_texture(const P_CopyTexTex& p) {
    Texture *s = p.src, *d = p.dst;
    if (!s || !d || !tex_gpu(s) || !tex_gpu(d)) return;
    end_pass();
    const SubRes ss = sub_of(p.src_view, p.has_src_view), ds = sub_of(p.dst_view, p.has_dst_view);
    const VkExtent3D se = tex_extent(s, ss.level), de = tex_extent(d, ds.level);
    P_CopyRegion sr = p.has_src_region ? p.src_region : P_CopyRegion{0, 0, 0, static_cast<int32_t>(se.width), static_cast<int32_t>(se.height), static_cast<int32_t>(se.depth)};
    P_CopyRegion dr = p.has_dst_region ? p.dst_region : P_CopyRegion{0, 0, 0, static_cast<int32_t>(de.width), static_cast<int32_t>(de.height), static_cast<int32_t>(de.depth)};
    tex_ensure_current(s, *F);
    if (s != d) tex_ensure_current(d, *F);
    const bool same = s == d;
    tex_transition(s, *F, same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (!same) tex_transition(d, *F, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    tex_mark_gpu_written(d);
    const VkImageLayout sl = same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const VkImageLayout dl = same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const VkImageAspectFlags sa = tex_aspect(s), da = tex_aspect(d);
    const bool same_size = sr.w == dr.w && sr.h == dr.h && sr.d == dr.d;
    const bool same_fmt = tex_vk_format(s) == tex_vk_format(d);
    if (same_size && same_fmt) {
        VkImageCopy c = {};
        c.srcSubresource = {sa, ss.level, ss.layer, 1};
        c.dstSubresource = {da, ds.level, ds.layer, 1};
        c.srcOffset = {sr.x, sr.y, sr.z};
        c.dstOffset = {dr.x, dr.y, dr.z};
        c.extent = {static_cast<uint32_t>(sr.w), static_cast<uint32_t>(sr.h), static_cast<uint32_t>(std::max(1, sr.d))};
        if (s->b.target != NVN_TEXTURE_TARGET_3D && sr.d > 1) { c.srcSubresource.layerCount = c.dstSubresource.layerCount = sr.d; c.extent.depth = 1; c.srcOffset.z = c.dstOffset.z = 0; }
        vkCmdCopyImage(F->cb, tex_image(s), sl, tex_image(d), dl, 1, &c);
    } else {
        VkImageBlit b = {};
        b.srcSubresource = {sa, ss.level, ss.layer, 1};
        b.dstSubresource = {da, ds.level, ds.layer, 1};
        b.srcOffsets[0] = {sr.x, sr.y, sr.z};
        b.srcOffsets[1] = {sr.x + sr.w, sr.y + sr.h, sr.z + std::max(1, sr.d)};
        b.dstOffsets[0] = {dr.x, dr.y, dr.z};
        b.dstOffsets[1] = {dr.x + dr.w, dr.y + dr.h, dr.z + std::max(1, dr.d)};
        vkCmdBlitImage(F->cb, tex_image(s), sl, tex_image(d), dl, 1, &b, (p.flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    memory_barrier(F->cb);
    counters().copies.fetch_add(1, std::memory_order_relaxed);
}

void do_downsample(const P_Downsample& p) {
    Texture *s = p.src, *d = p.dst;
    if (!s || !d || !tex_gpu(s) || !tex_gpu(d)) return;
    end_pass();
    if (tex_is_depth(s)) { note_unsupported("Downsample of a depth texture"); return; }
    tex_transition(s, *F, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    tex_transition(d, *F, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    tex_mark_gpu_written(d);
    VkImageResolve rs = {};
    rs.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rs.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    const VkExtent3D e = tex_extent(d, 0);
    rs.extent = {e.width, e.height, 1};
    vkCmdResolveImage(F->cb, tex_image(s), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex_image(d), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rs);
    memory_barrier(F->cb);
    counters().copies.fetch_add(1, std::memory_order_relaxed);
}

void do_write_memory(uint64_t gpu_addr, const void* data, uint32_t size) {
    // a GPU-ordered write of up to 64 KiB into pool memory (UpdateUniformBuffer, counters, events)
    end_pass();
    memory_barrier(F->cb);
    BufRef b = resolve_buffer(gpu_addr, size);
    if (!b.ok) return;
    vkCmdUpdateBuffer(F->cb, b.buffer, b.offset, size, data);
    memory_barrier(F->cb);
}

// ---- the interpreter ----------------------------------------------------------------------------------------------------------
void apply_bind_program(const P_BindProgram& p) {
    for (int st = 0; st < 6; ++st)
        if (p.stages & (1u << st)) S.stage[st] = p.program ? p.program->stage[st] : nullptr;
}

void run_list(const CmdList& list, int depth) {
    if (depth > kMaxDepth) { log_once("depth", "CallCommands nesting too deep"); return; }
    counters().lists_replayed.fetch_add(1, std::memory_order_relaxed);
    for (RecordReader r(list); r.next();) {
        switch (r.op()) {
            case Op::Nop: break;
            case Op::BindProgram: apply_bind_program(r.payload<P_BindProgram>()); break;
            case Op::BindBuffers: {
                const auto& p = r.payload<P_BindBuffers>();
                const NVNbufferRange* rg = reinterpret_cast<const NVNbufferRange*>(r.blob<P_BindBuffers>());
                for (uint32_t i = 0; i < p.count; ++i) {
                    const uint32_t k = p.first + i;
                    if (p.kind == kBufVertex && k < 16) S.vtx[k] = {rg[i].address, rg[i].size};
                    else if (p.kind == kBufUniform && p.stage < 6 && k < 16) S.ubo[p.stage][k] = {rg[i].address, rg[i].size};
                    else if (p.kind == kBufStorage && p.stage < 6 && k < 16) S.ssbo[p.stage][k] = {rg[i].address, rg[i].size};
                    else if (p.kind == kBufXfb) note_unsupported("transform feedback buffers");
                }
                break;
            }
            case Op::BindTextures: {
                const auto& p = r.payload<P_BindTextures>();
                const uint64_t* h = reinterpret_cast<const uint64_t*>(r.blob<P_BindTextures>());
                for (uint32_t i = 0; i < p.count; ++i)
                    if (p.stage < 6 && p.first + i < 64) S.tex[p.stage][p.first + i] = h[i];
                break;
            }
            case Op::BindImages: {
                const auto& p = r.payload<P_BindTextures>();
                const uint64_t* h = reinterpret_cast<const uint64_t*>(r.blob<P_BindTextures>());
                for (uint32_t i = 0; i < p.count; ++i)
                    if (p.stage < 6 && p.first + i < 16) S.img[p.stage][p.first + i] = h[i];
                break;
            }
            case Op::BindVertexAttribs: {
                const auto& p = r.payload<P_BindVertexAttribs>();
                const NVNvertexAttribState* a = reinterpret_cast<const NVNvertexAttribState*>(r.blob<P_BindVertexAttribs>());
                S.attr_count = std::min<uint32_t>(p.count, 16);
                for (uint32_t i = 0; i < S.attr_count; ++i) S.attr[i] = decode(&a[i]);
                break;
            }
            case Op::BindVertexStreams: {
                const auto& p = r.payload<P_BindVertexStreams>();
                const NVNvertexStreamState* a = reinterpret_cast<const NVNvertexStreamState*>(r.blob<P_BindVertexStreams>());
                S.stream_count = std::min<uint32_t>(p.count, 16);
                for (uint32_t i = 0; i < S.stream_count; ++i) S.stream[i] = decode(&a[i]);
                break;
            }
            case Op::BindBlend: {
                NVNblendState st; std::memcpy(&st, r.payload<P_RawState>().bytes, sizeof st);
                BlendInfo b = decode(&st);
                if (b.target >= 0 && b.target < 8) S.blend[b.target] = b;
                break;
            }
            case Op::BindColor: { NVNcolorState st; std::memcpy(&st, r.payload<P_RawState>().bytes, sizeof st); S.color = decode(&st); break; }
            case Op::BindChannelMask: { NVNchannelMaskState st; std::memcpy(&st, r.payload<P_RawState>().bytes, sizeof st); S.chmask = decode(&st); break; }
            case Op::BindDepthStencil: { NVNdepthStencilState st; std::memcpy(&st, r.payload<P_RawState>().bytes, sizeof st); S.ds = decode(&st); break; }
            case Op::BindPolygon: { NVNpolygonState st; std::memcpy(&st, r.payload<P_RawState>().bytes, sizeof st); S.poly = decode(&st); break; }
            case Op::BindMultisample: { NVNmultisampleState st; std::memcpy(&st, r.payload<P_RawState>().bytes, sizeof st); S.ms = decode(&st); break; }
            case Op::SetTexturePool: S.tpool = Ref<TexturePool>::share(static_cast<TexturePool*>(r.payload<P_Pool>().pool)); break;
            case Op::SetSamplerPool: S.spool = Ref<SamplerPool>::share(static_cast<SamplerPool*>(r.payload<P_Pool>().pool)); break;
            case Op::SetViewports: {
                const auto& p = r.payload<P_Rects>();
                const float* v = reinterpret_cast<const float*>(r.blob<P_Rects>());
                for (uint32_t i = 0; i < p.count && p.first + i < 16; ++i) std::memcpy(S.vp[p.first + i], v + 4 * i, 16);
                S.nvp = std::max(S.nvp, std::min<uint32_t>(p.first + p.count, 16));
                break;
            }
            case Op::SetScissors: {
                const auto& p = r.payload<P_Rects>();
                const int32_t* v = reinterpret_cast<const int32_t*>(r.blob<P_Rects>());
                for (uint32_t i = 0; i < p.count && p.first + i < 16; ++i) std::memcpy(S.sc[p.first + i], v + 4 * i, 16);
                S.nsc = std::max(S.nsc, std::min<uint32_t>(p.first + p.count, 16));
                break;
            }
            case Op::SetDepthRanges: {
                const auto& p = r.payload<P_DepthRanges>();
                const float* v = reinterpret_cast<const float*>(r.blob<P_DepthRanges>());
                for (uint32_t i = 0; i < p.count && p.first + i < 16; ++i) std::memcpy(S.dr[p.first + i], v + 2 * i, 8);
                break;
            }
            case Op::SetDepthBounds: { const auto& p = r.payload<P_DepthBounds>(); S.depth_bounds = p.enable; S.db_near = p.near_; S.db_far = p.far_; break; }
            case Op::SetStencil: {
                const auto& p = r.payload<P_Stencil>();
                for (int f = 0; f < 2; ++f) {
                    const bool hit = p.face == NVN_FACE_FRONT_AND_BACK || (f == 0 && p.face == NVN_FACE_FRONT) || (f == 1 && p.face == NVN_FACE_BACK);
                    if (!hit) continue;
                    if (p.what == 0) S.stencil_ref[f] = p.value;
                    else if (p.what == 1) S.stencil_vmask[f] = p.value;
                    else S.stencil_wmask[f] = p.value;
                }
                break;
            }
            case Op::SetBlendColor: std::memcpy(S.blend_color, r.payload<P_BlendColor>().c, 16); break;
            case Op::SetPointSize: S.point_size = r.payload<P_Float>().v; break;
            case Op::SetLineWidth: S.line_width = r.payload<P_Float>().v; break;
            case Op::SetAlphaRef: S.alpha_ref = r.payload<P_Float>().v; break;
            case Op::SetPolygonOffsetClamp: { const auto& p = r.payload<P_PolygonOffsetClamp>(); S.bias_factor = p.factor; S.bias_units = p.units; S.bias_clamp = p.clamp; break; }
            case Op::SetSampleMask: S.sample_mask = r.payload<P_U32>().v; break;
            case Op::SetRasterizerDiscard: S.rasterizer_discard = r.payload<P_U32>().v != 0; break;
            case Op::SetDepthClamp: S.depth_clamp = r.payload<P_U32>().v != 0; break;
            case Op::SetPrimitiveRestart: { const auto& p = r.payload<P_PrimitiveRestart>(); S.prim_restart = p.enable != 0; S.restart_index = p.index; break; }
            case Op::SetPatchSize: case Op::SetTessLevels: note_unsupported("tessellation state"); break;
            case Op::SetRenderEnable:
                if (r.size() >= sizeof(RecHeader) + sizeof(P_RenderEnableCond)) S.render_enable = true;   // conditional render: treated as visible
                else S.render_enable = r.payload<P_U32>().v != 0;
                break;
            case Op::SetCoverageModulationTable: case Op::SetConservativeRaster: break;
            case Op::SetRenderTargets: {
                const auto& p = r.payload<P_RenderTargets>();
                S.ncolor = p.count;
                for (uint32_t i = 0; i < 8; ++i) {
                    S.color_tex[i] = i < p.count ? Ref<Texture>::share(p.color[i].tex) : Ref<Texture>();
                    if (i < p.count) { S.color_view[i] = p.color[i].view; S.color_has_view[i] = p.color[i].has_view != 0; }
                    else S.color_has_view[i] = false;
                }
                S.depth_tex = Ref<Texture>::share(p.depth.tex);
                S.depth_view = p.depth.view;
                S.depth_has_view = p.depth.has_view != 0;
                break;
            }
            case Op::DrawArrays: do_draw_arrays(r.payload<P_DrawArrays>()); break;
            case Op::DrawElements: do_draw_elements(r.payload<P_DrawElements>()); break;
            case Op::DrawIndirect: do_draw_indirect(r.payload<P_DrawIndirect>()); break;
            case Op::DrawTexture: note_unsupported("DrawTexture"); break;
            case Op::ClearColor: do_clear_color(r.payload<P_ClearColor>()); break;
            case Op::ClearDepthStencil: do_clear_depth_stencil(r.payload<P_ClearDepthStencil>()); break;
            case Op::ClearTexture: do_clear_texture(r.payload<P_ClearTexture>()); break;
            case Op::ClearBuffer: {
                const auto& p = r.payload<P_ClearBuffer>();
                end_pass();
                BufRef b = resolve_buffer(p.address, p.size);
                if (b.ok) vkCmdFillBuffer(F->cb, b.buffer, b.offset, p.size & ~3ull, p.value);
                memory_barrier(F->cb);
                counters().clears.fetch_add(1);
                break;
            }
            case Op::CopyBufferToBuffer: {
                const auto& p = r.payload<P_CopyBufferToBuffer>();
                end_pass();
                BufRef s = resolve_buffer(p.src, p.size), d = resolve_buffer(p.dst, p.size);
                if (s.ok && d.ok) {
                    VkBufferCopy c = {s.offset, d.offset, p.size};
                    vkCmdCopyBuffer(F->cb, s.buffer, d.buffer, 1, &c);
                }
                memory_barrier(F->cb);
                counters().copies.fetch_add(1);
                break;
            }
            case Op::CopyBufferToTexture: do_copy_buffer_texture(r.payload<P_CopyBufTex>(), true); break;
            case Op::CopyTextureToBuffer: do_copy_buffer_texture(r.payload<P_CopyBufTex>(), false); break;
            case Op::CopyTextureToTexture: do_copy_texture_texture(r.payload<P_CopyTexTex>()); break;
            case Op::Downsample: do_downsample(r.payload<P_Downsample>()); break;
            case Op::UpdateUniformBuffer: {
                const auto& p = r.payload<P_UpdateUniformBuffer>();
                const uint8_t* data = r.blob<P_UpdateUniformBuffer>();
                const uint32_t n = static_cast<uint32_t>(p.size & ~3ull);
                if (n) do_write_memory(p.address + static_cast<uint64_t>(p.offset), data, n);
                break;
            }
            case Op::Dispatch: { const auto& p = r.payload<P_Dispatch>(); do_dispatch(p.x, p.y, p.z, 0); break; }
            case Op::DispatchIndirect: do_dispatch(0, 0, 0, r.payload<P_DispatchIndirect>().address); break;
            case Op::ReportCounter: {
                const auto& p = r.payload<P_Counter>();
                struct { uint64_t counter, timestamp; } rep = {1, timestamp_ns()};   // occlusion counters are reported as 'visible'
                if (p.type != NVN_COUNTER_TYPE_TIMESTAMP) note_unsupported("occlusion/statistics counters (reported as non-zero)");
                if (p.type == NVN_COUNTER_TYPE_TIMESTAMP) rep.counter = 0;
                if (p.address) do_write_memory(p.address, &rep, sizeof rep);
                break;
            }
            case Op::ReportValue: {
                const auto& p = r.payload<P_ReportValue>();
                struct { uint64_t counter, timestamp; } rep = {p.value, timestamp_ns()};
                if (p.address) do_write_memory(p.address, &rep, sizeof rep);
                break;
            }
            case Op::ResetCounter: break;
            case Op::Barrier: end_pass(); memory_barrier(F->cb); break;
            case Op::FenceSync: {
                const auto& p = r.payload<P_Sync>();
                if (p.sync) { p.sync->serial.store(F->serial); p.sync->armed.store(true); }
                break;
            }
            case Op::WaitSync: break;
            case Op::SignalEvent: {
                const auto& p = r.payload<P_Event>();
                if (p.event && p.event->pool && p.event->pool->base) {
                    Event* ev = p.event;
                    uint32_t v = p.value;
                    if (p.mode != NVN_EVENT_SIGNAL_MODE_WRITE) {
                        // ADD: the GPU cannot add into memory; the sum is computed here from the value after the signals recorded so far
                        uint32_t base = 0;
                        if (ev->shadow_valid.load()) base = ev->shadow.load();
                        else std::memcpy(&base, ev->pool->base + ev->offset, 4);
                        v = base + p.value;
                    }
                    ev->shadow.store(v);
                    ev->shadow_valid.store(true);
                    do_write_memory(ev->pool->gpu_base + static_cast<uint64_t>(ev->offset), &v, 4);
                }
                break;
            }
            case Op::WaitEvent: note_unsupported("GPU-side WaitEvent"); break;
            case Op::CallCommands: {
                const auto& p = r.payload<P_CallCommands>();
                const uint64_t* h = reinterpret_cast<const uint64_t*>(r.blob<P_CallCommands>());
                for (uint32_t i = 0; i < p.count; ++i) {
                    Ref<CmdList> l = handle_lookup(h[i]);
                    if (l) run_list(*l, depth + 1);
                    else log_once("missing-handle", "CallCommands of an unknown handle skipped");
                }
                break;
            }
            case Op::DiscardColor: case Op::DiscardDepthStencil: break;
            case Op::ResolveDepth: note_unsupported("ResolveDepthBuffer"); break;
            default: note_unsupported(op_name(r.op())); break;
        }
    }
}

}  // namespace

void replay_begin_submit(Frame& f) {
    F = &f;
    S.window_origin = window_origin_mode();
    S.depth_mode = depth_mode();
    P = Pass();
    g_set_cache.clear();
    g_bound_pipeline = VK_NULL_HANDLE;
    // one support buffer per submission (Ryujinx SupportBuffer: render scales 1.0, no BGRA remap)
    g_support = f.upload(sizeof(SupportBuffer), 256);
    if (g_support) {
        SupportBuffer sb = support_buffer_defaults();
        std::memcpy(g_support.ptr, &sb, sizeof sb);
    }
}

void replay_lists(Frame& f, const std::vector<Ref<CmdList>>& lists) {
    F = &f;
    for (const Ref<CmdList>& l : lists)
        if (l) run_list(*l, 0);
}

void replay_end_submit(Frame& f) {
    F = &f;
    end_pass();
    F = nullptr;
}

void replay_shutdown() {
    g_mask.shutdown();
    g_mask_ready = false;
    pipeline_shutdown();
    S = GState();
    P = Pass();
    g_set_cache.clear();
}

}  // namespace nxrt::nvn::be::gpu
