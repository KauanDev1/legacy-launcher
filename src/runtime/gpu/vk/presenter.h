// SPDX-License-Identifier: MIT
// Puts finished frames on screen (an SDL window's swapchain) or, headless, only keeps them offscreen; either way
// the last presented frame stays in an image of its own that capture_png reads back (--screenshot-after).
//
// present(frame, w, h): `frame` is a VK_FORMAT_R8G8B8A8_UNORM image of at least w x h, in
// VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, written by earlier submissions on Device::graphics_queue() (transfer,
// colour attachment or compute writes; present() makes them visible with a barrier of its own). It stays in that
// layout. present() copies it to the presenter's last-frame image and, with a window, blits that image to the
// acquired swapchain image (scaled to fit, aspect kept, black bars) and queues it for presentation (FIFO). Each call
// waits for its own submission to finish, so nothing is in flight between calls. A swapchain that is out of date
// (resized window) is recreated on the next present; a minimised window (extent 0) skips the window part.
//
// Every call comes from one thread at a time (the GPU thread); the Device must outlive the Presenter. Destruction
// waits for the device to be idle first.
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "common/result.h"
#include "runtime/gpu/vk/device.h"

struct VmaAllocation_T;  // same declaration as VK_DEFINE_HANDLE(VmaAllocation) in vk_mem_alloc.h

namespace lr::gpu::vk {

class Presenter {
 public:
  // sdl_window: the SDL_Window* the Device was created for (Device::surface()), or nullptr for headless.
  static Result<Presenter> create(Device& device, void* sdl_window /*nullptr = headless*/);

  Presenter(Presenter&& other) noexcept;
  Presenter& operator=(Presenter&& other) noexcept;
  Presenter(const Presenter&) = delete;
  Presenter& operator=(const Presenter&) = delete;
  ~Presenter();

  void present(VkImage frame, uint32_t w, uint32_t h);
  // The last presented frame as an RGBA PNG (runtime/gpu/screenshot.h). Error before the first present().
  Result<void> capture_png(const std::filesystem::path& path);

  bool headless() const { return window_ == nullptr; }
  uint64_t frames_presented() const { return frames_; }

 private:
  Presenter() = default;
  void destroy();
  Result<void> create_swapchain();
  void destroy_swapchain();
  Result<void> ensure_last_frame(uint32_t w, uint32_t h);

  Device* device_ = nullptr;
  void* window_ = nullptr;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  VkSemaphore acquired_ = VK_NULL_HANDLE;  // one frame in flight at most: one acquire semaphore is enough

  // The last presented frame (R8G8B8A8_UNORM, TRANSFER_SRC_OPTIMAL once written).
  VkImage last_ = VK_NULL_HANDLE;
  VmaAllocation_T* last_alloc_ = nullptr;
  uint32_t last_w_ = 0, last_h_ = 0;
  uint64_t frames_ = 0;

  // Window only.
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
  VkExtent2D extent_{0, 0};
  std::vector<VkImage> images_;
  std::vector<VkSemaphore> rendered_;  // one per swapchain image: presentation may still hold the previous one
  bool recreate_ = false;
};

}  // namespace lr::gpu::vk
