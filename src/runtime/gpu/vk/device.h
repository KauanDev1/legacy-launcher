// SPDX-License-Identifier: MIT
// Vulkan device wrapper for the emulated GPU's backend: instance, physical device, logical device, one graphics
// queue (also used for present and transfer), and the VMA allocator. Entry points are loaded by volk from the
// system libvulkan.so.1; no loader or driver is ever shipped.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "common/result.h"
#include "volk.h"

struct VmaAllocator_T;  // same declaration as VK_DEFINE_HANDLE(VmaAllocator) in vk_mem_alloc.h
using VmaAllocator = VmaAllocator_T*;

namespace lr::gpu::vk {

// The device limits the emulated GPU needs, a pure function over the reported limits: empty when they are met,
// otherwise the first one that is not ("maxVertexOutputComponents >= 64 (device has 32)"). Plan B passes up to 16
// interpolators of 4 components between the stages, the Vulkan guaranteed minimum of 64 (carried Task 13 item).
std::string check_device_limits(const VkPhysicalDeviceLimits& limits);

// Waits for `fence` in 100 ms slices instead of forever: true once it is signalled; false when `stop` (may be null)
// was raised first, or the wait failed (device lost). Used by every per-frame wait, so an orderly exit
// (VkBackend::request_stop) never hangs on a GPU that stopped answering.
bool wait_fence(VkDevice device, VkFence fence, const std::atomic<bool>* stop);

struct DeviceOptions {
  bool validation = false;     // enable VK_LAYER_KHRONOS_validation when installed (warns and continues if not)
  bool headless = true;        // no surface; VK_KHR_swapchain is still required of the device (same floor everywhere)
  void* sdl_window = nullptr;  // SDL_Window* created with SDL_WINDOW_VULKAN; used only when !headless
};

class Device {
 public:
  // Fails with ErrorCode::ToolMissing when there is no Vulkan driver or the device lacks a required feature or
  // limit (check_device_limits).
  static Result<Device> create(const DeviceOptions& options);
  // Destruction waits for the device to be idle (vkDeviceWaitIdle) before destroying anything.

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
  // The physical device's limits (texture and sampler caches: image sizes, anisotropy, LOD bias, heap sizes).
  const VkPhysicalDeviceLimits& limits() const { return limits_; }
  // Optional features, enabled when the device has them (not part of the floor): samplerAnisotropy (without it the
  // sampler cache never enables anisotropy) and samplerMirrorClampToEdge (without it the Xenos mirror-once clamp
  // modes fall back to mirrored repeat).
  bool sampler_anisotropy() const { return sampler_anisotropy_; }
  bool mirror_clamp_to_edge() const { return mirror_clamp_to_edge_; }
  // Optional features of the draw path (Task 20), enabled when present: geometryShader (rect lists expand in a
  // built-in geometry shader; without it a rect-list draw is fatal), fillModeNonSolid (PA_SU_SC_MODE_CNTL dual
  // polygon modes; without it they draw filled), independentBlend (per-target blend state; without it every target
  // takes target 0's) and VK_EXT_provoking_vertex's provokingVertexLast (flat interpolants from the last vertex).
  bool geometry_shader() const { return geometry_shader_; }
  bool fill_mode_non_solid() const { return fill_mode_non_solid_; }
  bool independent_blend() const { return independent_blend_; }
  bool provoking_vertex_last() const { return provoking_vertex_last_; }

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
  VkPhysicalDeviceLimits limits_{};
  bool sampler_anisotropy_ = false;
  bool mirror_clamp_to_edge_ = false;
  bool geometry_shader_ = false;
  bool fill_mode_non_solid_ = false;
  bool independent_blend_ = false;
  bool provoking_vertex_last_ = false;
};

}  // namespace lr::gpu::vk
