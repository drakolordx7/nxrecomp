// SPDX-License-Identifier: GPL-3.0-or-later
// Texture images of the NVN back end (render thread only). Each be::Texture gets one VkImage on first use (tex_gpu). The image is
// created without a view: every view (sampled, attachment, present alias) is made here and cached on the TexGpu. Formats: the NVN
// format when the GPU has it, its vk_fallback when the GPU refuses it, ASTC as BC7 when the GPU has no ASTC.
// Uploads: prepare_texture (nvn_texdata.cpp) gives the bytes, they go through a staging buffer into one buffer-to-image copy per
// level. Layouts are tracked per image (all levels and layers share one layout). Samplers are cached by their NVN state.
// Readback (download_texture) copies a render target back and tiles it into the game's storage.
#include <mutex>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "nxrt/log.h"
#include "nvn_enums.h"
#include "nvn_format.h"
#include "nvn_gpu_internal.h"
#include "nvn_texdata.h"
#include "nvn_texture_layout.h"
#include "tegra_tiling.h"

namespace nxrt::nvn::be {

// The Vulkan side of a texture: Texture::gpu points to it (created by gpu::tex_gpu, freed by gpu::tex_destroy).
static std::mutex g_live_mu;
static std::set<struct TexGpu*> g_live_set;

struct TexGpu {
    gfx::Image img;                                     // the image; created without a view (views are cached below)
    const FormatInfo* fmt = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;              // the image format: the NVN one, its fallback, or BC7 for ASTC
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkExtent3D base = {1, 1, 1};                        // level 0 extent (depth for 3D images)
    uint32_t levels = 1, layers = 1;
    VkImageUsageFlags usage = 0;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;   // tracked layout (the same for every level and layer)
    bool failed = false;                                // no image: the texture is not usable on the GPU (logged once)
    bool transcoded = false;                            // ASTC stored as BC7
    bool mutable_fmt = false;                           // sRGB and UNORM views of the image are allowed
    bool cube = false;                                  // created with CUBE_COMPATIBLE
    bool depth = false;                                 // depth and/or stencil aspect
    bool uploaded = false;                              // the image holds the storage's content (or the black fallback)
    bool gpu_written = false;                           // rendered by the GPU since the CPU last wrote the storage
    std::map<std::array<uint32_t, 12>, VkImageView> views;
};

Texture::~Texture() {
    if (gpu) gpu::destroy_texture_gpu(gpu);
    gpu = nullptr;
}

namespace gpu {
namespace {

constexpr const char* kTag = "nvn.tex";
std::atomic<uint64_t> g_live_images{0};

// Logs a message only the first time its key is seen (render thread only).
bool first_time(const std::string& key) {
    static std::set<std::string> seen;
    return seen.insert(key).second;
}

uint64_t div_up(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

// ---- formats -------------------------------------------------------------------------------------------------------------
bool is_block_format(VkFormat f) {
    return (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK) ||
           (f >= VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK && f <= VK_FORMAT_EAC_R11G11_SNORM_BLOCK) ||
           (f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
}

bool is_srgb(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
            return true;
        default:
            break;
    }
    if (f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK)
        return ((static_cast<int>(f) - static_cast<int>(VK_FORMAT_ASTC_4x4_UNORM_BLOCK)) & 1) != 0;
    return false;
}

// The sRGB / UNORM partner of a format (the two may view the same image when it is mutable), UNDEFINED when there is none.
VkFormat srgb_partner(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
        case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
        case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case VK_FORMAT_BC2_UNORM_BLOCK: return VK_FORMAT_BC2_SRGB_BLOCK;
        case VK_FORMAT_BC2_SRGB_BLOCK: return VK_FORMAT_BC2_UNORM_BLOCK;
        case VK_FORMAT_BC3_UNORM_BLOCK: return VK_FORMAT_BC3_SRGB_BLOCK;
        case VK_FORMAT_BC3_SRGB_BLOCK: return VK_FORMAT_BC3_UNORM_BLOCK;
        case VK_FORMAT_BC7_UNORM_BLOCK: return VK_FORMAT_BC7_SRGB_BLOCK;
        case VK_FORMAT_BC7_SRGB_BLOCK: return VK_FORMAT_BC7_UNORM_BLOCK;
        default: break;
    }
    if (f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) {
        const int base = static_cast<int>(VK_FORMAT_ASTC_4x4_UNORM_BLOCK);
        return static_cast<VkFormat>(((static_cast<int>(f) - base) ^ 1) + base);   // the ASTC UNORM/SRGB pairs are consecutive
    }
    return VK_FORMAT_UNDEFINED;
}

// The float member of a 32/64/128-bit integer format's class and back: Maxwell moves the raw bits, whatever the shader's declared
// type (the translated shaders declare every sampled value and colour output as float).
VkFormat float_alias(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: return VK_FORMAT_R32_SFLOAT;
        case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: return VK_FORMAT_R32G32_SFLOAT;
        case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        default: return VK_FORMAT_UNDEFINED;
    }
}
VkFormat uint_alias(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_SINT: return VK_FORMAT_R32_UINT;
        case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_SINT: return VK_FORMAT_R32G32_UINT;
        case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_SINT: return VK_FORMAT_R32G32B32A32_UINT;
        default: return VK_FORMAT_UNDEFINED;
    }
}
VkFormat sint_alias(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_UINT: return VK_FORMAT_R32_SINT;
        case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_UINT: return VK_FORMAT_R32G32_SINT;
        case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: return VK_FORMAT_R32G32B32A32_SINT;
        default: return VK_FORMAT_UNDEFINED;
    }
}
bool bit_alias_class(VkFormat f) { return float_alias(f) != VK_FORMAT_UNDEFINED || uint_alias(f) != VK_FORMAT_UNDEFINED; }

// Same image family: the format itself, its sRGB / UNORM partner, or a raw-bit alias of the same size, on a mutable image.
bool format_view_ok(const TexGpu& g, VkFormat fmt) {
    if (fmt == g.format) return true;
    if (!g.mutable_fmt) return false;
    if (srgb_partner(g.format) == fmt) return true;
    return bit_alias_class(g.format) && (fmt == float_alias(g.format) || fmt == uint_alias(g.format) || fmt == sint_alias(g.format));
}

// The format a colour attachment of this image is rendered through: integer 32-bit formats as their float alias, because the
// translated fragment shaders write float outputs and the console stores the register bits unconverted.
VkFormat attachment_format_of(const TexGpu& g, VkFormat requested) {
    const VkFormat f = requested == VK_FORMAT_UNDEFINED ? g.format : requested;
    if (g.mutable_fmt && bit_alias_class(f)) {
        const VkFormat a = float_alias(f);
        if (a != VK_FORMAT_UNDEFINED && format_view_ok(g, a)) return a;
    }
    return f;
}

VkImageAspectFlags aspect_bits(const FormatInfo* f) {
    switch (f->aspect) {
        case kAspectDepth: return VK_IMAGE_ASPECT_DEPTH_BIT;
        case kAspectStencil: return VK_IMAGE_ASPECT_STENCIL_BIT;
        case kAspectDepthStencil: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

// ---- shapes --------------------------------------------------------------------------------------------------------------
struct Shape {
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkExtent3D base = {1, 1, 1};
    uint32_t levels = 1, layers = 1;
    bool cube = false;
    uint32_t samples = 1;                      // multisampled render target / resolve source
};

Shape shape_of(const Texture& t) {
    const TextureBuilderInfo& b = t.b;
    Shape s;
    const bool is1d = b.target == NVN_TEXTURE_TARGET_1D || b.target == NVN_TEXTURE_TARGET_1D_ARRAY;
    const bool is3d = b.target == NVN_TEXTURE_TARGET_3D;
    s.type = is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    s.base.width = static_cast<uint32_t>(std::max(1, b.width));
    s.base.height = is1d ? 1u : static_cast<uint32_t>(std::max(1, b.height));
    s.base.depth = is3d ? static_cast<uint32_t>(std::max(1, b.depth)) : 1u;
    s.levels = std::max<uint32_t>(1, t.levels);
    s.layers = std::max<uint32_t>(1, t.layers);
    s.cube = target_is_cube(b.target);
    if (target_is_multisample(b.target) || b.samples > 1) {
        s.samples = static_cast<uint32_t>(vk_sample_count(b.samples));
        s.levels = 1;
    }
    return s;
}

VkExtent3D level_extent(const VkExtent3D& base, uint32_t level) {
    auto shr = [level](uint32_t v) { return level >= 32 ? 1u : std::max<uint32_t>(1, v >> level); };
    return {shr(base.width), shr(base.height), shr(base.depth)};
}

gfx::ImageDesc make_desc(const Shape& s, VkFormat fmt, VkImageUsageFlags usage, VkImageCreateFlags flags) {
    gfx::ImageDesc d;
    d.type = s.type;
    d.format = fmt;
    d.width = s.base.width;
    d.height = s.base.height;
    d.depth = s.base.depth;
    d.mip_levels = s.levels;
    d.array_layers = s.layers;
    d.usage = usage;
    d.flags = flags;
    d.samples = static_cast<VkSampleCountFlagBits>(s.samples);
    d.create_view = false;
    d.name = "nvn texture";
    return d;
}

VkImageCreateFlags base_flags(const Shape& s, VkFormat fmt) {
    VkImageCreateFlags f = 0;
    if (s.cube) f |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (s.type == VK_IMAGE_TYPE_3D) f |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;   // 2D views of the slices
    if (srgb_partner(fmt) != VK_FORMAT_UNDEFINED || bit_alias_class(fmt)) f |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    return f;
}

// ---- creation -------------------------------------------------------------------------------------------------------------
// Makes the image of t (or the failed marker when no format works). Stores the result in t->gpu, which is never null afterwards.
TexGpu* create_texture_gpu(Texture* t) {
    auto* g = new TexGpu();
    { std::lock_guard<std::mutex> lk(g_live_mu); g_live_set.insert(g); }
    t->gpu = g;
    const Shape s = shape_of(*t);
    g->fmt = t->fmt;
    g->type = s.type;
    g->base = s.base;
    g->levels = s.levels;
    g->layers = s.layers;
    g->cube = s.cube;
    auto fail = [&](const std::string& why) -> TexGpu* {
        NXLOG_E(kTag, "texture %u: no image (%s): the texture is not usable on the GPU", t->id, why.c_str());
        g->failed = true;
        counters().textures_failed.fetch_add(1);
        return g;
    };
    if (!g->fmt) return fail("no format");

    const gfx::DeviceFeatures& ft = device().features();
    const bool to_bc7 = g->fmt->astc && !ft.astc_ldr;
    if (to_bc7 && !ft.bc) return fail("ASTC is stored as BC7 and the GPU has no BC compression");
    const VkFormat want = to_bc7 ? (g->fmt->srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK) : g->fmt->vk;
    const VkFormat alt = to_bc7 ? VK_FORMAT_UNDEFINED : g->fmt->vk_fallback;

    std::string why = std::string("no Vulkan format for NVN format ") + g->fmt->name;
    VkFormat chosen = VK_FORMAT_UNDEFINED;
    for (VkFormat c : {want, alt}) {
        if (c == VK_FORMAT_UNDEFINED) continue;
        std::string r;
        if (device().image_supported(make_desc(s, c, VK_IMAGE_USAGE_SAMPLED_BIT, base_flags(s, c)), &r)) {
            chosen = c;
            break;
        }
        why = r;
    }
    if (chosen == VK_FORMAT_UNDEFINED) return fail(why);

    g->format = chosen;
    g->transcoded = to_bc7;
    g->mutable_fmt = srgb_partner(chosen) != VK_FORMAT_UNDEFINED || bit_alias_class(chosen);
    g->aspect = aspect_bits(g->fmt);
    g->depth = (g->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;

    // Required usage is sampling. Each optional usage is kept only when the GPU accepts the image with it.
    const VkImageCreateFlags flags = base_flags(s, chosen);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    const auto add = [&](VkImageUsageFlags bit) {
        if (device().image_supported(make_desc(s, chosen, usage | bit, flags))) usage |= bit;
    };
    add(VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    add(VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!g->depth && !is_block_format(chosen)) add(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (g->depth) add(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    if (t->b.flags & NVN_TEXTURE_FLAGS_IMAGE) add(VK_IMAGE_USAGE_STORAGE_BIT);
    g->usage = usage;

    std::string why2;
    if (device().create_image(make_desc(s, chosen, usage, flags), &g->img, &why2) != VK_SUCCESS)
        return fail("create_image: " + why2);
    g_live_images.fetch_add(1);
    counters().textures_created.fetch_add(1);
    return g;
}

// The texture's image, created on first use. Null when the texture has no usable image (the failure is counted and logged once).
TexGpu* tex_or_null(Texture* t) {
    if (!t || !active()) return nullptr;
    if (t->gpu) return t->gpu->failed ? nullptr : t->gpu;
    TexGpu* g = create_texture_gpu(t);
    return g->failed ? nullptr : g;
}

// ---- barriers and copies ----------------------------------------------------------------------------------------------------
void move_layout(Frame& f, TexGpu& g, VkImageLayout to) {
    if (g.layout == to) return;
    // A 3D image created with 2D_ARRAY_COMPATIBLE: without maintenance9 the layer range of a barrier means all depth slices.
    const uint32_t layer_count = g.type == VK_IMAGE_TYPE_3D ? VK_REMAINING_ARRAY_LAYERS : g.layers;
    image_barrier(f.cb, g.img.image, g.layout, to, g.aspect, 0, g.levels, 0, layer_count);
    g.layout = to;
}

// Copies buffer regions (already offset to the staging buffer) into every level of the image, then leaves it readable by shaders.
void copy_regions_to_image(Frame& f, TexGpu& g, VkBuffer buf, const std::vector<VkBufferImageCopy>& regions) {
    move_layout(f, g, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(f.cb, buf, g.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()), regions.data());
    move_layout(f, g, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// The image gets zeros (transparent black). Block formats are filled from a zeroed staging buffer; uncompressed ones are cleared.
void fill_zero(Frame& f, TexGpu& g) {
    if (!(g.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return;
    if (is_block_format(g.format)) {
        uint32_t bw = 0, bh = 0, bytes = 0;
        if (g.transcoded) {
            bw = 4; bh = 4; bytes = 16;
        } else if (g.fmt && g.format == g.fmt->vk) {
            bw = g.fmt->block_w; bh = g.fmt->block_h; bytes = g.fmt->bytes;
        } else {
            return;
        }
        std::vector<VkBufferImageCopy> regions;
        uint64_t total = 0;
        for (uint32_t l = 0; l < g.levels; ++l) {
            const VkExtent3D e = level_extent(g.base, l);
            const uint64_t size = div_up(e.width, bw) * div_up(e.height, bh) * bytes * e.depth * g.layers;
            VkBufferImageCopy r = {};
            r.bufferOffset = total;
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, g.layers};
            r.imageExtent = e;
            regions.push_back(r);
            total += size;
        }
        UploadAlloc st = f.staging(total);
        if (!st) return;
        std::memset(st.ptr, 0, static_cast<size_t>(total));
        for (VkBufferImageCopy& r : regions) r.bufferOffset += st.offset;
        copy_regions_to_image(f, g, st.buffer, regions);
        return;
    }
    move_layout(f, g, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue zero = {};
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, g.levels, 0, g.layers};
    vkCmdClearColorImage(f.cb, g.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    move_layout(f, g, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// ---- views ----------------------------------------------------------------------------------------------------------------
// The view with these fields, created once and cached on the image.
VkImageView view_for(TexGpu& g, VkImageViewType type, uint32_t base_level, uint32_t level_count, uint32_t base_layer, uint32_t layer_count,
                     VkFormat fmt, VkImageAspectFlags aspect, const VkComponentMapping& swz) {
    const std::array<uint32_t, 12> key = {static_cast<uint32_t>(type), base_level, level_count, base_layer, layer_count,
                                          static_cast<uint32_t>(fmt), static_cast<uint32_t>(aspect), static_cast<uint32_t>(swz.r),
                                          static_cast<uint32_t>(swz.g), static_cast<uint32_t>(swz.b), static_cast<uint32_t>(swz.a), 0u};
    auto it = g.views.find(key);
    if (it != g.views.end()) return it->second;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = g.img.image;
    vi.viewType = type;
    vi.format = fmt;
    vi.components = swz;
    vi.subresourceRange = {aspect, base_level, level_count, base_layer, layer_count};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(vkdev(), &vi, nullptr, &view) != VK_SUCCESS) {
        NXLOG_W(kTag, "vkCreateImageView failed for a view of format %d", static_cast<int>(fmt));
        return VK_NULL_HANDLE;
    }
    g.views.emplace(key, view);
    return view;
}

VkImageView bad_view(const Texture& t, const char* what) {
    if (first_time("view " + std::to_string(t.id) + " " + what))
        NXLOG_W(kTag, "texture %u: view not possible (%s): the binding gets the dummy view", t.id, what);
    return VK_NULL_HANDLE;
}

const VkComponentMapping kIdentity = {};

// Dummy views: 1x1 (x1) textures of each view type, black (or depth 1.0), made the first time they are asked for.
struct Dummy {
    gfx::Image img;
    VkImageView view = VK_NULL_HANDLE;
    bool tried = false;
};
constexpr VkImageViewType kDummyTypes[6] = {VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                            VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, VK_IMAGE_VIEW_TYPE_3D};

int dummy_index(VkImageViewType type) {
    for (int i = 0; i < 6; ++i)
        if (kDummyTypes[i] == type) return i;
    return 1;   // anything else: a 2D dummy
}

struct Shared {
    std::map<uint64_t, std::vector<std::pair<SamplerInfo, VkSampler>>> samplers;
    VkSampler dummy_sampler = VK_NULL_HANDLE;
    Dummy color[6];
    Dummy depth[6];
    std::map<std::pair<int, int>, Dummy> storage;   // (view type, format): GENERAL-layout 1x1 storage images
};
Shared g_sh;

bool make_dummy(Dummy& d, VkImageViewType type, bool depth) {
    d.tried = true;
    const VkFormat fmt = depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    gfx::ImageDesc id;
    id.format = fmt;
    id.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    id.create_view = false;
    id.name = depth ? "nvn dummy depth" : "nvn dummy";
    switch (type) {
        case VK_IMAGE_VIEW_TYPE_1D: id.type = VK_IMAGE_TYPE_1D; break;
        case VK_IMAGE_VIEW_TYPE_3D: id.type = VK_IMAGE_TYPE_3D; break;
        case VK_IMAGE_VIEW_TYPE_CUBE:
        case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
            id.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
            id.array_layers = 6;
            break;
        default: break;
    }
    std::string why;
    if (device().create_image(id, &d.img, &why) != VK_SUCCESS) {
        NXLOG_W(kTag, "dummy view image failed: %s", why.c_str());
        return false;
    }
    const VkImageAspectFlags asp = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    const uint32_t layers = id.array_layers;
    const VkImage img = d.img.image;
    const bool ok = device().submit_once([&](VkCommandBuffer cmd) {
        image_barrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, asp, 0, 1, 0, layers);
        const VkImageSubresourceRange range = {asp, 0, 1, 0, layers};
        if (depth) {
            VkClearDepthStencilValue v = {1.0f, 0};
            vkCmdClearDepthStencilImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
        } else {
            VkClearColorValue c = {};
            vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
        }
        image_barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, asp, 0, 1, 0, layers);
    });
    if (!ok) {
        NXLOG_W(kTag, "dummy view clear failed");
        device().destroy_image(d.img);
        return false;
    }
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img;
    vi.viewType = type;
    vi.format = fmt;
    vi.subresourceRange = {asp, 0, 1, 0, layers};
    if (vkCreateImageView(vkdev(), &vi, nullptr, &d.view) != VK_SUCCESS) {
        d.view = VK_NULL_HANDLE;
        device().destroy_image(d.img);
        return false;
    }
    return true;
}

// ---- samplers -------------------------------------------------------------------------------------------------------------
uint64_t sampler_hash(const SamplerInfo& s) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
        h ^= h >> 29;
    };
    auto bits = [](float f) {
        uint32_t u = 0;
        std::memcpy(&u, &f, 4);
        return static_cast<uint64_t>(u);
    };
    mix(static_cast<uint32_t>(s.min_filter));
    mix(static_cast<uint32_t>(s.mag_filter));
    mix(static_cast<uint32_t>(s.wrap_s));
    mix(static_cast<uint32_t>(s.wrap_t));
    mix(static_cast<uint32_t>(s.wrap_r));
    mix(bits(s.lod_min));
    mix(bits(s.lod_max));
    mix(bits(s.lod_bias));
    mix(bits(s.lod_snap));
    mix(static_cast<uint32_t>(s.compare_mode));
    mix(static_cast<uint32_t>(s.compare_func));
    for (float c : s.border_color) mix(bits(c));
    mix(static_cast<uint32_t>(s.border_kind));
    mix(bits(s.max_anisotropy));
    mix(static_cast<uint32_t>(s.reduction));
    return h;
}

VkSamplerAddressMode wrap_mode(int nvn_wrap) {
    const VkSamplerAddressMode m = vk_address_mode(nvn_wrap);
    if (m == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE && !device().features().sampler_mirror_clamp_to_edge)
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    return m;
}

// Without custom border colours: the nearest of transparent black, opaque black and opaque white.
VkBorderColor standard_border(const SamplerInfo& s) {
    float c[4];
    for (int k = 0; k < 4; ++k) {
        if (s.border_kind == 0) {
            c[k] = s.border_color[k];
        } else {
            uint32_t u = 0;
            std::memcpy(&u, &s.border_color[k], 4);
            c[k] = s.border_kind == 1 ? static_cast<float>(static_cast<int32_t>(u)) : static_cast<float>(u);
        }
    }
    const float cand[3][4] = {{0, 0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1, 1}};
    int best = 0;
    float best_d = 1e30f;
    for (int i = 0; i < 3; ++i) {
        float d = 0;
        for (int k = 0; k < 4; ++k) d += (c[k] - cand[i][k]) * (c[k] - cand[i][k]);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    if (s.border_kind == 0) {
        static const VkBorderColor f[3] = {VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK, VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE};
        return f[best];
    }
    static const VkBorderColor i[3] = {VK_BORDER_COLOR_INT_TRANSPARENT_BLACK, VK_BORDER_COLOR_INT_OPAQUE_BLACK, VK_BORDER_COLOR_INT_OPAQUE_WHITE};
    return i[best];
}

// Null when the device refuses the sampler (the caller then uses the dummy sampler).
VkSampler create_sampler(const SamplerInfo& s) {
    const gfx::DeviceFeatures& ft = device().features();
    const bool mips = min_filter_uses_mips(s.min_filter);
    VkSamplerCustomBorderColorCreateInfoEXT cbc = {VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = vk_mag_filter(s.mag_filter);
    ci.minFilter = vk_min_filter(s.min_filter);
    ci.mipmapMode = mips ? vk_mipmap_mode(s.min_filter) : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = wrap_mode(s.wrap_s);
    ci.addressModeV = wrap_mode(s.wrap_t);
    ci.addressModeW = wrap_mode(s.wrap_r);
    ci.mipLodBias = s.lod_bias;
    ci.minLod = mips ? s.lod_min : 0.0f;
    ci.maxLod = mips ? s.lod_max : 0.0f;
    if (s.max_anisotropy > 1.0f && ft.sampler_anisotropy) {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = std::min(s.max_anisotropy, device().properties().limits.maxSamplerAnisotropy);
    }
    if (s.compare_mode == NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE) {
        ci.compareEnable = VK_TRUE;
        ci.compareOp = vk_compare_op(s.compare_func);
    }
    if (s.reduction != NVN_SAMPLER_REDUCTION_AVERAGE)
        note_unsupported("sampler reduction (min/max filtering): not enabled on the device, ignored");
    if (ft.custom_border_color) {
        // The border words are stored as the game gave them: float bits, or int / uint bits (border_kind).
        if (s.border_kind == 0) {
            ci.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            std::memcpy(cbc.customBorderColor.float32, s.border_color, sizeof(float) * 4);
        } else {
            ci.borderColor = VK_BORDER_COLOR_INT_CUSTOM_EXT;
            std::memcpy(cbc.customBorderColor.uint32, s.border_color, sizeof(uint32_t) * 4);
        }
        cbc.format = VK_FORMAT_UNDEFINED;
        ci.pNext = &cbc;
    } else {
        ci.borderColor = standard_border(s);
    }
    VkSampler out = VK_NULL_HANDLE;
    if (vkCreateSampler(vkdev(), &ci, nullptr, &out) != VK_SUCCESS) {
        NXLOG_W(kTag, "vkCreateSampler failed: the dummy sampler is used");
        return VK_NULL_HANDLE;
    }
    return out;
}

}  // namespace

// ---- texture interface (gpu::*, internal.h) -----------------------------------------------------------------------------
TexGpu* tex_gpu(Texture* t) { return tex_or_null(t); }

void tex_ensure_current(Texture* t, Frame& f) {
    TexGpu* g = tex_or_null(t);
    if (!g || g->depth) return;
    if (t->b.samples > 1 || target_is_multisample(t->b.target)) return;   // multisampled images are only ever rendered
    const bool dirty = t->cpu_dirty.load();
    if (g->uploaded && !dirty) return;           // the image already holds the storage's content
    if (g->gpu_written && !dirty) return;        // the GPU rendered it and the CPU did not write since
    t->cpu_dirty.exchange(false);

    const gfx::DeviceFeatures& ft = device().features();
    TexCaps caps;
    caps.bc = ft.bc;
    caps.astc = ft.astc_ldr;
    caps.bc7_transcode = true;
    PreparedTexture prep;
    const bool ok = prepare_texture(*t, caps, prep);
    if (!ok || prep.format != g->format || !(g->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        NXLOG_E(kTag, "texture %u: not uploaded (%s): the image is cleared", t->id,
                !ok ? prep.error.c_str() : "the image format differs from the storage format or the image cannot be written");
        fill_zero(f, *g);
        g->uploaded = true;
        g->gpu_written = false;
        return;
    }

    const uint64_t bytes = prep.data.size();
    UploadAlloc st = f.staging(bytes);
    if (!st) {
        t->cpu_dirty.store(true);                // try again next time
        return;
    }
    std::memcpy(st.ptr, prep.data.data(), static_cast<size_t>(bytes));
    std::vector<VkBufferImageCopy> regions;
    regions.reserve(prep.level.size());
    for (uint32_t i = 0; i < prep.level.size(); ++i) {
        const PreparedLevel& pl = prep.level[i];
        VkBufferImageCopy r = {};
        r.bufferOffset = st.offset + pl.offset;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, g->layers};
        r.imageExtent = {pl.width, pl.height, pl.depth};
        regions.push_back(r);
    }
    copy_regions_to_image(f, *g, st.buffer, regions);
    g->uploaded = true;
    g->gpu_written = false;
    counters().texture_uploads.fetch_add(1);
    counters().texture_upload_bytes.fetch_add(bytes);
    if (prep.transcoded) counters().bc7_transcodes.fetch_add(1);
}

void tex_transition(Texture* t, Frame& f, VkImageLayout layout) {
    TexGpu* g = tex_or_null(t);
    if (g) move_layout(f, *g, layout);
}

VkImageLayout tex_layout(Texture* t) {
    return (t && t->gpu && !t->gpu->failed) ? t->gpu->layout : VK_IMAGE_LAYOUT_UNDEFINED;
}

void tex_mark_gpu_written(Texture* t) {
    TexGpu* g = tex_or_null(t);
    if (!g) return;
    g->gpu_written = true;
    t->cpu_dirty.store(false);                   // the GPU data is the newer one until the CPU writes again
}

VkImage tex_image(Texture* t) {
    TexGpu* g = tex_or_null(t);
    return g ? g->img.image : VK_NULL_HANDLE;
}

VkFormat tex_vk_format(Texture* t) {
    TexGpu* g = tex_or_null(t);
    return g ? g->format : VK_FORMAT_UNDEFINED;
}

VkImageAspectFlags tex_aspect(Texture* t) {
    TexGpu* g = tex_or_null(t);
    return g ? g->aspect : VK_IMAGE_ASPECT_COLOR_BIT;
}

VkExtent3D tex_extent(Texture* t, uint32_t level) { return level_extent(shape_of(*t).base, level); }

uint32_t tex_levels(Texture* t) { return t->levels; }

uint32_t tex_layers(Texture* t) { return t->layers; }

bool tex_is_depth(Texture* t) { return t->fmt && t->fmt->aspect != kAspectColor; }

VkImageView tex_sampled_view(Texture* t, const ViewRequest& req) {
    TexGpu* g = tex_or_null(t);
    if (!g) return VK_NULL_HANDLE;
    const bool is3d = g->type == VK_IMAGE_TYPE_3D;
    const uint32_t total_layers = is3d ? g->base.depth : g->layers;   // 3D images: the slices are the layers of 2D views
    const uint32_t bl = req.base_level;
    if (bl >= g->levels) return bad_view(*t, "base level out of range");
    const uint32_t nl = req.level_count ? req.level_count : g->levels - bl;
    if (nl == 0 || bl + nl > g->levels) return bad_view(*t, "level range out of range");
    const uint32_t bly = req.base_layer;
    uint32_t nly = 1;
    switch (req.type) {
        case VK_IMAGE_VIEW_TYPE_2D:
            if (req.layer_count > 1 || bly >= total_layers) return bad_view(*t, "2D view of a layer out of range");
            break;
        case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
            nly = req.layer_count ? req.layer_count : total_layers - bly;
            if (nly == 0 || bly + nly > total_layers) return bad_view(*t, "2D array layer range out of range");
            break;
        case VK_IMAGE_VIEW_TYPE_CUBE:
            if (!g->cube || is3d || bly % 6 != 0 || bly + 6 > g->layers) return bad_view(*t, "cube needs a cube-compatible image of 6 layers");
            nly = 6;
            break;
        case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
            nly = req.layer_count ? req.layer_count : g->layers - bly;
            if (!g->cube || is3d || nly == 0 || nly % 6 != 0 || bly % 6 != 0 || bly + nly > g->layers)
                return bad_view(*t, "cube array needs a cube-compatible image with a multiple of 6 layers");
            break;
        case VK_IMAGE_VIEW_TYPE_3D:
            if (!is3d || bly != 0) return bad_view(*t, "3D view of an image that is not 3D");
            break;
        default:
            return bad_view(*t, "1D views are not supported (the image is 2D with height 1)");
    }
    VkFormat fmt = req.format == VK_FORMAT_UNDEFINED ? g->format : req.format;
    if (req.component != 0 && g->mutable_fmt && fmt == g->format) {
        // The shader's sampled type decides how the bits read: float shaders over an integer texture (and the reverse) see the raw bits.
        const VkFormat a = req.component == 1 ? float_alias(fmt) : req.component == 2 ? sint_alias(fmt) : uint_alias(fmt);
        if (a != VK_FORMAT_UNDEFINED && format_view_ok(*g, a)) fmt = a;
    }
    if (!format_view_ok(*g, fmt)) return bad_view(*t, "view format is not compatible with the image");
    VkImageAspectFlags asp = g->aspect;
    if (g->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) asp = req.depth_aspect ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT;
    return view_for(*g, req.type, bl, nl, bly, nly, fmt, asp, req.swizzle);
}

VkImageView tex_attachment_view(Texture* t, uint32_t level, uint32_t layer, uint32_t layer_count, VkFormat view_format) {
    TexGpu* g = tex_or_null(t);
    if (!g) return VK_NULL_HANDLE;
    if (level >= g->levels) return bad_view(*t, "attachment level out of range");
    const uint32_t total_layers = g->type == VK_IMAGE_TYPE_3D ? g->base.depth : g->layers;
    const uint32_t n = layer_count > 1 ? layer_count : 1;
    if (layer >= total_layers || layer + n > total_layers) return bad_view(*t, "attachment layers out of range");
    const VkFormat fmt = attachment_format_of(*g, view_format);
    if (!format_view_ok(*g, fmt)) return bad_view(*t, "attachment format is not compatible with the image");
    const VkImageViewType type = n > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    return view_for(*g, type, level, 1, layer, n, fmt, g->aspect, kIdentity);
}

VkFormat tex_attachment_format(Texture* t, VkFormat view_format) {
    TexGpu* g = tex_or_null(t);
    return g ? attachment_format_of(*g, view_format) : VK_FORMAT_UNDEFINED;
}

bool tex_has_storage(Texture* t) {
    TexGpu* g = tex_or_null(t);
    return g && (g->usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
}

VkImageLayout tex_sampled_layout(Texture* t) {
    return (t && t->fmt && t->fmt->aspect != kAspectColor) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

VkImageView tex_present_view(Texture* t) {
    TexGpu* g = tex_or_null(t);
    if (!g) return VK_NULL_HANDLE;
    // The UNORM alias of an sRGB image; the plain view when the image cannot be viewed as UNORM.
    const VkFormat fmt = (g->mutable_fmt && is_srgb(g->format)) ? srgb_partner(g->format) : g->format;
    return view_for(*g, VK_IMAGE_VIEW_TYPE_2D, 0, 1, 0, 1, fmt, g->aspect, kIdentity);
}

VkImageView dummy_view(VkImageViewType type, bool depth_compare) {
    const int idx = dummy_index(type);
    Dummy& d = (depth_compare ? g_sh.depth : g_sh.color)[idx];
    if (!d.tried) make_dummy(d, kDummyTypes[idx], depth_compare);
    return d.view;
}

VkImageView dummy_storage_view(VkImageViewType type, VkFormat format) {
    if (format == VK_FORMAT_UNDEFINED) format = VK_FORMAT_R8G8B8A8_UNORM;
    if (type == VK_IMAGE_VIEW_TYPE_CUBE || type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    const std::pair<int, int> key = {static_cast<int>(type), static_cast<int>(format)};
    auto& d = g_sh.storage[key];
    if (d.tried) return d.view;
    d.tried = true;
    gfx::ImageDesc id;
    id.format = format;
    id.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    id.create_view = false;
    id.name = "nvn dummy storage";
    if (type == VK_IMAGE_VIEW_TYPE_1D || type == VK_IMAGE_VIEW_TYPE_1D_ARRAY) id.type = VK_IMAGE_TYPE_1D;
    if (type == VK_IMAGE_VIEW_TYPE_3D) id.type = VK_IMAGE_TYPE_3D;
    if (type == VK_IMAGE_VIEW_TYPE_2D_ARRAY || type == VK_IMAGE_VIEW_TYPE_1D_ARRAY) id.array_layers = 6;
    std::string why;
    if (device().create_image(id, &d.img, &why) != VK_SUCCESS) {
        NXLOG_W(kTag, "dummy storage image failed: %s", why.c_str());
        return d.view;
    }
    const VkImage img = d.img.image;
    const uint32_t layers = id.array_layers;
    const bool ok = device().submit_once([&](VkCommandBuffer cmd) {
        image_barrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers);
        const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        VkClearColorValue c = {};
        vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
        image_barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers);
    });
    if (!ok) {
        device().destroy_image(d.img);
        return d.view;
    }
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img;
    vi.viewType = type;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    if (vkCreateImageView(vkdev(), &vi, nullptr, &d.view) != VK_SUCCESS) {
        d.view = VK_NULL_HANDLE;
        device().destroy_image(d.img);
    }
    return d.view;
}

VkSampler dummy_sampler() {
    if (g_sh.dummy_sampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.magFilter = VK_FILTER_NEAREST;
        ci.minFilter = VK_FILTER_NEAREST;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        if (vkCreateSampler(vkdev(), &ci, nullptr, &g_sh.dummy_sampler) != VK_SUCCESS) g_sh.dummy_sampler = VK_NULL_HANDLE;
    }
    return g_sh.dummy_sampler;
}

VkSampler sampler_for(const SamplerInfo& s) {
    auto& bucket = g_sh.samplers[sampler_hash(s)];
    for (const auto& e : bucket)
        if (e.first == s) return e.second;
    const VkSampler v = create_sampler(s);
    if (v == VK_NULL_HANDLE) return dummy_sampler();
    bucket.emplace_back(s, v);
    return v;
}

void tex_destroy(TexGpu* g) {
    if (!g) return;
    {
        std::lock_guard<std::mutex> lk(g_live_mu);
        if (!g_live_set.erase(g)) return;   // already destroyed by tex_shutdown
    }
    for (auto& kv : g->views) vkDestroyImageView(vkdev(), kv.second, nullptr);
    g->views.clear();
    if (g->img) g_live_images.fetch_sub(1);
    device().destroy_image(g->img);
    delete g;
}

TexStats tex_stats() {
    TexStats s;
    s.images = g_live_images.load();
    s.uploads = counters().texture_uploads.load();
    s.upload_bytes = counters().texture_upload_bytes.load();
    s.transcodes = counters().bc7_transcodes.load();
    s.failed = counters().textures_failed.load();
    return s;
}

// Frees every cached object this module made (samplers, dummy views and images). Render thread, device idle, no texture alive:
// called by the tests before shutdown_for_test (the back end's teardown does not call it yet).
void tex_shutdown() {
    std::vector<TexGpu*> left;
    {
        std::lock_guard<std::mutex> lk(g_live_mu);
        left.assign(g_live_set.begin(), g_live_set.end());
    }
    for (TexGpu* g : left) tex_destroy(g);
    for (auto& kv : g_sh.samplers)
        for (auto& e : kv.second) vkDestroySampler(vkdev(), e.second, nullptr);
    g_sh.samplers.clear();
    if (g_sh.dummy_sampler != VK_NULL_HANDLE) vkDestroySampler(vkdev(), g_sh.dummy_sampler, nullptr);
    for (auto& kv : g_sh.storage) {
        if (kv.second.view != VK_NULL_HANDLE) vkDestroyImageView(vkdev(), kv.second.view, nullptr);
        device().destroy_image(kv.second.img);
    }
    for (Dummy* set : {g_sh.color, g_sh.depth}) {
        for (int i = 0; i < 6; ++i) {
            if (set[i].view != VK_NULL_HANDLE) vkDestroyImageView(vkdev(), set[i].view, nullptr);
            device().destroy_image(set[i].img);
        }
    }
    g_sh = Shared();
}

namespace {
float half_to_float(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp(static_cast<float>(m + 1024), static_cast<int>(e) - 25);
    return s ? -v : v;
}
float small_float(uint32_t bits, int mant_bits) {   // unsigned 11 / 10 bit float (5 bit exponent)
    const uint32_t e = bits >> mant_bits, m = bits & ((1u << mant_bits) - 1);
    if (e == 0) return std::ldexp(static_cast<float>(m), -14 - mant_bits);
    if (e == 31) return m ? NAN : INFINITY;
    return std::ldexp(1.0f + static_cast<float>(m) / static_cast<float>(1u << mant_bits), static_cast<int>(e) - 15);
}
uint8_t to8(float v, bool hdr) {
    if (!(v == v)) return 255;
    if (hdr) v = v / (1.0f + v);          // display of a high dynamic range value
    return static_cast<uint8_t>(std::min(1.0f, std::max(0.0f, v)) * 255.0f + 0.5f);
}
}  // namespace

bool dump_texture(Texture* t, const std::string& path, std::string* stats) {
    TexGpu* g = t ? t->gpu : nullptr;
    if (!g || g->failed || g->layout == VK_IMAGE_LAYOUT_UNDEFINED) return false;
    const VkFormat f = g->format;
    uint32_t bpp = 0;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            bpp = 4; break;
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_UINT: bpp = 8; break;
        case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: bpp = 16; break;
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: bpp = 2; break;
        case VK_FORMAT_R8_UNORM: bpp = 1; break;
        case VK_FORMAT_D16_UNORM: bpp = 2; aspect = VK_IMAGE_ASPECT_DEPTH_BIT; break;
        case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: bpp = 4; aspect = VK_IMAGE_ASPECT_DEPTH_BIT; break;
        case VK_FORMAT_D24_UNORM_S8_UINT: bpp = 4; aspect = VK_IMAGE_ASPECT_DEPTH_BIT; break;
        default: return false;
    }
    const VkExtent3D e = level_extent(g->base, 0);
    const uint64_t size = static_cast<uint64_t>(e.width) * e.height * bpp;
    finish();
    gfx::Buffer rb;
    run_frame_sync([&](Frame& fr) {
        if (device().create_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, gfx::MemoryUse::Readback, &rb, "nvn texture dump") != VK_SUCCESS) return;
        const VkImageLayout prev = g->layout;
        move_layout(fr, *g, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy r = {};
        r.imageSubresource = {aspect, 0, 0, 1};
        r.imageExtent = {e.width, e.height, 1};
        vkCmdCopyImageToBuffer(fr.cb, g->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, 1, &r);
        move_layout(fr, *g, prev);
    });
    if (!rb) return false;
    device().invalidate_buffer(rb);
    const uint8_t* src = static_cast<const uint8_t*>(rb.mapped);
    std::vector<uint8_t> rgba(static_cast<size_t>(e.width) * e.height * 4, 255);
    double sum[4] = {0, 0, 0, 0};
    float mn[4] = {1e30f, 1e30f, 1e30f, 1e30f}, mx[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
    uint64_t nonzero = 0;
    for (uint32_t i = 0; i < e.width * e.height; ++i) {
        const uint8_t* p = src + static_cast<size_t>(i) * bpp;
        float c[4] = {0, 0, 0, 1};
        bool hdr = false;
        auto u32 = [&](int k) { uint32_t v; std::memcpy(&v, p + 4 * k, 4); return v; };
        auto f32 = [&](int k) { float v; std::memcpy(&v, p + 4 * k, 4); return v; };
        auto u16 = [&](int k) { uint16_t v; std::memcpy(&v, p + 2 * k, 2); return v; };
        switch (f) {
            case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: for (int k = 0; k < 4; ++k) c[k] = p[k] / 255.0f; break;
            case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: c[0] = p[2] / 255.0f; c[1] = p[1] / 255.0f; c[2] = p[0] / 255.0f; c[3] = p[3] / 255.0f; break;
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32: { const uint32_t v = u32(0); c[0] = (v & 1023) / 1023.0f; c[1] = ((v >> 10) & 1023) / 1023.0f; c[2] = ((v >> 20) & 1023) / 1023.0f; c[3] = (v >> 30) / 3.0f; break; }
            case VK_FORMAT_R8G8_UNORM: c[0] = p[0] / 255.0f; c[1] = p[1] / 255.0f; c[2] = 0; break;
            case VK_FORMAT_R8_UNORM: c[0] = c[1] = c[2] = p[0] / 255.0f; break;
            case VK_FORMAT_R16_UNORM: c[0] = c[1] = c[2] = u16(0) / 65535.0f; break;
            case VK_FORMAT_R16_SFLOAT: c[0] = c[1] = c[2] = half_to_float(u16(0)); hdr = true; break;
            case VK_FORMAT_R16G16B16A16_SFLOAT: for (int k = 0; k < 4; ++k) c[k] = half_to_float(u16(k)); hdr = true; break;
            case VK_FORMAT_R32_SFLOAT: c[0] = c[1] = c[2] = f32(0); hdr = true; break;
            case VK_FORMAT_R32G32_SFLOAT: c[0] = f32(0); c[1] = f32(1); c[2] = 0; hdr = true; break;
            case VK_FORMAT_R32G32B32A32_SFLOAT: for (int k = 0; k < 4; ++k) c[k] = f32(k); hdr = true; break;
            case VK_FORMAT_B10G11R11_UFLOAT_PACK32: { const uint32_t v = u32(0); c[0] = small_float(v & 2047, 6); c[1] = small_float((v >> 11) & 2047, 6); c[2] = small_float(v >> 22, 5); hdr = true; break; }
            case VK_FORMAT_R32_UINT: { const uint32_t v = u32(0); c[0] = (v & 255) / 255.0f; c[1] = ((v >> 8) & 255) / 255.0f; c[2] = ((v >> 16) & 255) / 255.0f; c[3] = (v >> 24) / 255.0f; break; }
            case VK_FORMAT_R32G32_UINT: { const uint32_t v = u32(0), w2 = u32(1); c[0] = (v & 255) / 255.0f; c[1] = ((v >> 8) & 255) / 255.0f; c[2] = (w2 & 255) / 255.0f; c[3] = ((w2 >> 8) & 255) / 255.0f; break; }
            case VK_FORMAT_R32G32B32A32_UINT: for (int k = 0; k < 4; ++k) c[k] = (u32(k) & 255) / 255.0f; break;
            case VK_FORMAT_D16_UNORM: c[0] = c[1] = c[2] = u16(0) / 65535.0f; break;
            case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: c[0] = c[1] = c[2] = f32(0); break;
            case VK_FORMAT_D24_UNORM_S8_UINT: c[0] = c[1] = c[2] = (u32(0) & 0xffffff) / 16777215.0f; break;
            default: break;
        }
        bool any = false;
        for (int k = 0; k < 4; ++k) {
            if (c[k] == c[k]) {
                sum[k] += c[k];
                mn[k] = std::min(mn[k], c[k]);
                mx[k] = std::max(mx[k], c[k]);
            }
            if (c[k] != 0.0f) any = true;
            rgba[static_cast<size_t>(i) * 4 + k] = (k == 3 && (aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) ? 255 : to8(c[k], hdr);
        }
        if (any) ++nonzero;
    }
    device().destroy_buffer(rb);
    if (stats) {
        char b[400];
        const double n = static_cast<double>(e.width) * e.height;
        std::snprintf(b, sizeof b, "min %g %g %g %g max %g %g %g %g mean %g %g %g %g nonzero %.1f%%", mn[0], mn[1], mn[2], mn[3], mx[0], mx[1], mx[2], mx[3],
                      sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n, 100.0 * static_cast<double>(nonzero) / n);
        *stats = b;
    }
    return path.empty() || gfx::save_png_rgba(path, rgba.data(), e.width, e.height);
}

void mark_texture_cpu_dirty(Texture* t) {
    if (t) t->cpu_dirty.store(true);
}

// Copies every level of a render target back into the game's storage (block-linear, tiled with tegra::tile). Only when the GPU
// image holds the newest data: it exists, it was written by the GPU (or uploaded) and the CPU did not write since. Pitch-linear,
// transcoded, depth and format-fallback textures are not read back.
bool download_texture(Texture* t) {
    if (!t || !active() || !t->mem || !t->fmt || !t->block_linear) return false;
    TexGpu* g = t->gpu;
    if (!g || g->failed || g->depth || g->transcoded || g->format != t->fmt->vk) return false;
    if (t->cpu_dirty.load()) return false;
    if (!(g->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || g->layout == VK_IMAGE_LAYOUT_UNDEFINED) return false;
    const TexLayout L = compute_texture_layout(t->b);
    if (!L.valid || !L.block_linear) return false;
    const uint64_t size = L.layout.linear_size;

    finish();   // earlier submissions must be done before the copy reads the image
    gfx::Buffer rb;
    run_frame_sync([&](Frame& f) {
        if (device().create_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, gfx::MemoryUse::Readback, &rb, "nvn texture readback") != VK_SUCCESS)
            return;
        const VkImageLayout prev = g->layout;
        move_layout(f, *g, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        std::vector<VkBufferImageCopy> regions;
        for (uint32_t l = 0; l < L.levels; ++l) {
            const tegra::LevelLayout& lv = L.layout.levels[l];
            VkBufferImageCopy r = {};
            r.bufferOffset = lv.linear_offset;
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, g->layers};
            r.imageExtent = {lv.width, lv.height, lv.depth};
            regions.push_back(r);
        }
        vkCmdCopyImageToBuffer(f.cb, g->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, static_cast<uint32_t>(regions.size()),
                               regions.data());
        move_layout(f, *g, prev);
    });
    if (!rb) return false;
    device().invalidate_buffer(rb);
    std::vector<uint8_t> lin(static_cast<size_t>(size));
    std::memcpy(lin.data(), rb.mapped, static_cast<size_t>(size));
    device().destroy_buffer(rb);
    tegra::tile(L.desc, L.layout, lin.data(), t->mem);
    return true;
}

}  // namespace gpu
}  // namespace nxrt::nvn::be
