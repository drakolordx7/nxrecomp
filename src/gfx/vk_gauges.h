// SPDX-License-Identifier: GPL-3.0-or-later
// Live-object gauges of the Vulkan layer (leak monitor, see nxrt/gauge.h). Each counter is defined once, in vk_device.cpp, as a
// function-local static. Convention: ++ right after a successful create, -- at the matching destroy (only when the handle is non-null).
#pragma once
#include "nxrt/gauge.h"

namespace nxrt {

nxrt::GaugeCounter& vk_gauge_images();                  // vk.images            VkImage (Device::create_image / destroy_image)
nxrt::GaugeCounter& vk_gauge_image_views();             // vk.image_views       VkImageView (every owner)
nxrt::GaugeCounter& vk_gauge_buffers();                 // vk.buffers           VkBuffer (Device::create_buffer / destroy_buffer, pools)
nxrt::GaugeCounter& vk_gauge_samplers();                // vk.samplers          VkSampler
nxrt::GaugeCounter& vk_gauge_shader_modules();          // vk.shader_modules    VkShaderModule
nxrt::GaugeCounter& vk_gauge_pipelines();               // vk.pipelines         VkPipeline (graphics)
nxrt::GaugeCounter& vk_gauge_pipeline_layouts();        // vk.pipeline_layouts  VkPipelineLayout
nxrt::GaugeCounter& vk_gauge_descriptor_set_layouts();  // vk.descriptor_set_layouts
nxrt::GaugeCounter& vk_gauge_descriptor_pools();        // vk.descriptor_pools  VkDescriptorPool
nxrt::GaugeCounter& vk_gauge_descriptor_sets();         // vk.descriptor_sets   live sets (freed with their pool or a pool reset)
nxrt::GaugeCounter& vk_gauge_command_pools();           // vk.command_pools     VkCommandPool
nxrt::GaugeCounter& vk_gauge_command_buffers();         // vk.command_buffers   VkCommandBuffer (allocate / free / pool destroy)
nxrt::GaugeCounter& vk_gauge_fences();                  // vk.fences            VkFence
nxrt::GaugeCounter& vk_gauge_semaphores();              // vk.semaphores        VkSemaphore
nxrt::GaugeCounter& vk_gauge_query_pools();             // vk.query_pools       VkQueryPool
nxrt::GaugeCounter& vk_gauge_swapchains();              // vk.swapchains        VkSwapchainKHR
nxrt::GaugeCounter& vk_gauge_memory_allocs();           // vk.memory_allocs     VkDeviceMemory from vkAllocateMemory (not VMA's own blocks)
nxrt::GaugeCounter& vk_gauge_memory_bytes();            // vk.memory_bytes      bytes of those allocations

}  // namespace nxrt
