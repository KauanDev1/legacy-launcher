// SPDX-License-Identifier: MIT
// The single translation unit holding the Vulkan Memory Allocator implementation. Compiled in the lr_vulkan_vma
// library WITHOUT our strict flags (third-party code, see src/runtime/CMakeLists.txt).
// Vulkan entry points come from volk (VK_NO_PROTOTYPES); VMA resolves them from vkGetInstanceProcAddr and
// vkGetDeviceProcAddr, which Device::create passes in VmaAllocatorCreateInfo::pVulkanFunctions.
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_VULKAN_VERSION 1002000
#define VMA_IMPLEMENTATION
#include "volk.h"
#include "vk_mem_alloc.h"
