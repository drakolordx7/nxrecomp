// SPDX-License-Identifier: GPL-3.0-or-later
// The one translation unit that holds the VulkanMemoryAllocator implementation (MIT, pinned in cmake/gfx_deps.cmake).
#include <volk.h>

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>
