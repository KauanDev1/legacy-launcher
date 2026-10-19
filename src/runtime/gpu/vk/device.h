// SPDX-License-Identifier: MIT
// Vulkan device wrapper for the emulated GPU's backend: instance, physical device, logical device, one graphics
// queue (also used for present and transfer), and the VMA allocator. Entry points are loaded by volk from the
// system libvulkan.so.1; no loader or driver is ever shipped.
#pragma once

#include <cstdint>
#include <string>

#include "common/result.h"
#include "volk.h"

struct VmaAllocator_T;  // same declaration as VK_DEFINE_HANDLE(VmaAllocator) in vk_mem_alloc.h
using VmaAllocator = VmaAllocator_T*;

namespace lr::gpu::vk {

struct DeviceOptions {
  bool validation = false;     // enable VK_LAYER_KHRONOS_validation when installed (warns and continues if not)
  bool headless = true;        // no surface; VK_KHR_swapchain is still required of the device (same floor everywhere)
  void* sdl_window = nullptr;  // SDL_Window* created with SDL_WINDOW_VULKAN; used only when !headless
};

class Device {
 public:
  // Fails with ErrorCode::ToolMissing when there is no Vulkan driver or the device lacks a required feature.
  static Result<Device> create(const DeviceOptions& options);

  Device(Device&& other) noexcept;
  Device& operator=(Device&& other) noexcept;
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;
  ~Device();

  VkInstance instance() const { return instance_; }
  VkPhysicalDevice physical_device() const { return physical_; }
  VkDevice device() const { return device_; }
  VkQueue graphics_queue() const { return queue_; }
  std::uint32_t graphics_family() const { return family_; }
  VmaAllocator allocator() const { return allocator_; }
  VkSurfaceKHR surface() const { return surface_; }  // VK_NULL_HANDLE when headless
  const std::string& device_name() const { return name_; }

 private:
  Device() = default;
  void destroy();

  VkInstance instance_ = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  std::uint32_t family_ = 0;
  VmaAllocator allocator_ = nullptr;
  std::string name_;
};

}  // namespace lr::gpu::vk
