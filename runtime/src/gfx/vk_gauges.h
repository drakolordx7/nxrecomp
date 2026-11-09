// SPDX-License-Identifier: GPL-3.0-or-later
// Live-object gauges of the Vulkan layer (leak monitor, see nxrt/gauge.h). Each counter is defined once, in vk_device.cpp, as a
// function-local static. Convention: ++ right after a successful create, -- at the matching destroy (only when the handle is non-null).
#pragma once
#include <volk.h>

#include <cstdint>

#include "nxrt/gauge.h"

namespace nxrt {

nxrt::GaugeCounter& vk_gauge_images();
nxrt::GaugeCounter& vk_gauge_image_views();
nxrt::GaugeCounter& vk_gauge_buffers();
nxrt::GaugeCounter& vk_gauge_samplers();
nxrt::GaugeCounter& vk_gauge_shader_modules();
nxrt::GaugeCounter& vk_gauge_pipelines();
nxrt::GaugeCounter& vk_gauge_pipeline_layouts();
nxrt::GaugeCounter& vk_gauge_descriptor_set_layouts();
nxrt::GaugeCounter& vk_gauge_descriptor_pools();
nxrt::GaugeCounter& vk_gauge_descriptor_sets();         // vk.descriptor_sets   live sets (freed with their pool or a pool reset)
nxrt::GaugeCounter& vk_gauge_command_pools();
nxrt::GaugeCounter& vk_gauge_command_buffers();         // vk.command_buffers   VkCommandBuffer (allocate / free / pool destroy)
nxrt::GaugeCounter& vk_gauge_fences();
nxrt::GaugeCounter& vk_gauge_semaphores();
nxrt::GaugeCounter& vk_gauge_query_pools();
nxrt::GaugeCounter& vk_gauge_swapchains();
nxrt::GaugeCounter& vk_gauge_memory_allocs();           // vk.memory_allocs     VkDeviceMemory from vkAllocateMemory (not VMA's own blocks)
nxrt::GaugeCounter& vk_gauge_memory_bytes();

namespace gfx {
// Memory guard discount (vk_device.cpp). The heap whose usage the guard subtracts from the process commit: the largest device-local heap of a
// discrete GPU; on other GPUs only a device-local heap that no host-visible memory type uses. -1: subtract nothing.
int vk_guard_discount_heap(const VkPhysicalDeviceMemoryProperties& mp, VkPhysicalDeviceType type);
uint64_t vk_guard_gpu_bytes();   // what the guard subtracts now: 0 without a device, with an integrated GPU, or once the device is gone
}  // namespace gfx

}  // namespace nxrt
