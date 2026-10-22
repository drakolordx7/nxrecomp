// SPDX-License-Identifier: GPL-3.0-or-later
// PLACEHOLDER: replaced by the texture GPU module (images, uploads, views, samplers).
#include "nvn_gpu_internal.h"

namespace nxrt::nvn::be {
Texture::~Texture() {}
namespace gpu {
TexGpu* tex_gpu(Texture*) { return nullptr; }
void tex_ensure_current(Texture*, Frame&) {}
void tex_transition(Texture*, Frame&, VkImageLayout) {}
VkImageLayout tex_layout(Texture*) { return VK_IMAGE_LAYOUT_UNDEFINED; }
void tex_mark_gpu_written(Texture*) {}
VkImage tex_image(Texture*) { return VK_NULL_HANDLE; }
VkFormat tex_vk_format(Texture*) { return VK_FORMAT_UNDEFINED; }
VkImageAspectFlags tex_aspect(Texture*) { return VK_IMAGE_ASPECT_COLOR_BIT; }
VkExtent3D tex_extent(Texture*, uint32_t) { return {1, 1, 1}; }
uint32_t tex_levels(Texture*) { return 1; }
uint32_t tex_layers(Texture*) { return 1; }
bool tex_is_depth(Texture*) { return false; }
VkImageView tex_sampled_view(Texture*, const ViewRequest&) { return VK_NULL_HANDLE; }
VkImageView tex_attachment_view(Texture*, uint32_t, uint32_t, uint32_t, VkFormat) { return VK_NULL_HANDLE; }
VkImageView tex_present_view(Texture*) { return VK_NULL_HANDLE; }
VkImageView dummy_view(VkImageViewType, bool) { return VK_NULL_HANDLE; }
VkSampler dummy_sampler() { return VK_NULL_HANDLE; }
VkSampler sampler_for(const SamplerInfo&) { return VK_NULL_HANDLE; }
void tex_destroy(TexGpu*) {}
TexStats tex_stats() { return {}; }
bool download_texture(Texture*) { return false; }
void mark_texture_cpu_dirty(Texture*) {}
}  // namespace gpu
}  // namespace nxrt::nvn::be
