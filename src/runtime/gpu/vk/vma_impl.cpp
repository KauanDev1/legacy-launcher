// SPDX-License-Identifier: MIT
// The single translation unit holding the Vulkan Memory Allocator implementation. Compiled in the lr_vulkan_vma
// library WITHOUT our strict flags (third-party code, see the root CMakeLists.txt). The VMA_* configuration macros
// are PUBLIC compile definitions of that target, so every TU including vk_mem_alloc.h sees the same layout.
// Vulkan entry points come from volk (VK_NO_PROTOTYPES); VMA resolves them from vkGetInstanceProcAddr and
// vkGetDeviceProcAddr, which Device::create passes in VmaAllocatorCreateInfo::pVulkanFunctions.
#define VMA_IMPLEMENTATION
#include "volk.h"
#include "vk_mem_alloc.h"
