// SPDX-License-Identifier: MIT
// The Vulkan renderer behind gpu::Backend (plan B). Task 17 skeleton: a Device, a Presenter (SDL window swapchain,
// or offscreen when headless) and swap(), which presents the guest's front buffer. resolve() goes to the EDRAM
// emulation (edram.h, Task 18: host render targets, clears, tiled resolves into guest memory). Draws and shader
// loads do nothing yet (textures and the draw path arrive in Tasks 19-21), so binds_shaders() stays false until
// Task 20 binds the translated shaders.
//
// swap(phys, w, h) reads the front buffer straight from guest memory as a LINEAR w x h surface of 32-bit
// big-endian A8R8G8B8 pixels (D3D's X8R8G8B8 front buffer, alpha forced to 255), converts it to RGBA8 on the CPU,
// uploads it and presents it. The real front buffer is tiled (Xenos 2D tiling, format in the fetch constant VdSwap
// writes): untiling and the format come with the texture path (Task 19), which replaces this copy. A front buffer
// that is not mapped, or a size of 0 or above 8192, skips the frame with one warning on stderr.
//
// This header has no Vulkan types: the game's main.cpp includes it through the staged headers.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "common/result.h"
#include "runtime/gpu/backend.h"
#include "runtime/gpu/pm4.h"

namespace lr::gpu::vk {

struct VkBackendOptions {
  MemoryAccess mem;            // guest memory and the physical -> guest translation (Kernel::gpu_memory_access)
  void* sdl_window = nullptr;  // SDL_Window* created with SDL_WINDOW_VULKAN; nullptr: headless (offscreen)
  bool validation = false;     // VK_LAYER_KHRONOS_validation when installed (DeviceOptions::validation)
  // --screenshot-after N: after presenting swap N, capture it to screenshot_path and call on_screenshot (GPU thread)
  // with the result. 0: never.
  uint64_t screenshot_after = 0;
  std::filesystem::path screenshot_path;
  std::function<void(uint64_t swap, const Result<void>& written)> on_screenshot;
};

class VkBackend final : public Backend {
 public:
  // ErrorCode::ToolMissing (message naming the missing piece) when there is no usable Vulkan device.
  static Result<std::unique_ptr<VkBackend>> create(VkBackendOptions options);
  ~VkBackend() override;

  void load_shader(ShaderStage stage, uint32_t guest_addr, uint32_t dwords) override;
  void draw(const RegisterFile& regs, const DrawInfo& info, const ActiveShaders& shaders) override;
  void resolve(const RegisterFile& regs) override;
  void swap(uint32_t frontbuffer_phys, uint32_t width, uint32_t height) override;
  void wait_idle() override;
  // Task 20 binds the translated shaders; until then an untranslatable shader is only reported.
  bool binds_shaders() const override { return false; }

  const std::string& device_name() const { return name_; }
  // Any thread, any time: the per-frame fence waits (100 ms slices) give up and swap() stops submitting, so the GPU
  // thread leaves a frame in flight instead of hanging; the orderly exit calls it before GpuSystem::stop().
  void request_stop();
  // Waits for the device to be idle and destroys every Vulkan object (orderly exit, after GpuSystem::stop()), then
  // prints "[gpu] vulkan: shut down" on stderr; later calls do nothing. Not thread-safe against the other calls.
  void shutdown();

 private:
  struct Impl;
  explicit VkBackend(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
  std::string name_;
};

}  // namespace lr::gpu::vk
