// SPDX-License-Identifier: MIT
// The Vulkan renderer behind gpu::Backend (plan B): a Device, a Presenter (SDL window swapchain, or offscreen when
// headless), the frame's command stream (stream.h), the EDRAM emulation (edram.h: host render targets, clears, tiled
// resolves into guest memory), the texture and sampler caches and the draw path (draw.h, Task 20), which draws with
// the translated shaders: binds_shaders() is true, so an untranslatable shader is fatal. A guest error a draw or a
// resolve cannot get past (an index or vertex buffer outside guest memory, an unsupported primitive type or format)
// goes to the fatal handler GpuSystem installs (guest_fatal, exit 5), named, and the backend does no more work.
//
// swap(regs, phys, w, h) decodes the w x h front buffer with the texture loader (Task 19,
// runtime/gpu/texture_format.h front_buffer_rgba8): fetch constant 0, which VdSwap wrote just before, gives its
// format, tiling, endian swap, pitch and swizzle; alpha is forced to 255. The RGBA8 result is uploaded and presented.
// A front buffer that cannot be decoded (unsupported format, not in guest memory) or a size of 0 or above 8192 skips
// the frame with one warning per distinct reason on stderr. The texture and sampler caches (textures.h,
// samplers.h) are created here; every swap starts a texture-cache frame and every resolve invalidates the textures
// it overwrote.
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

// What the draw path did (tests, --trace-gpu style diagnostics).
struct DrawStats {
  uint64_t draws = 0;              // draws recorded
  uint64_t skipped = 0;            // draws that could produce nothing (no vertex shader, no surface, empty viewport)
  uint64_t pipelines = 0;          // pipelines created so far
  bool last_fragment_stage = false;  // the last recorded draw's pipeline has a fragment stage
};

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
  void swap(const RegisterFile& regs, uint32_t frontbuffer_phys, uint32_t width, uint32_t height) override;
  uint64_t textures_created() const override;
  void wait_idle() override;
  // The draw path binds the translated shaders: an untranslatable one is fatal (exit 5).
  bool binds_shaders() const override { return true; }
  void set_fatal_handler(std::function<void(const std::string& what)> fatal) override;

  const std::string& device_name() const { return name_; }
  DrawStats draw_stats() const;
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
