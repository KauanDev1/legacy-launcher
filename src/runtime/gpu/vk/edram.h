// SPDX-License-Identifier: MIT
// Xenos EDRAM emulation on host render targets (Task 18, M3 spec 5.4). The 10 MiB EDRAM holds 2048 tiles of 80x16
// 32-bit samples (5120 bytes). The guest places its colour and depth surfaces in it (RB_COLOR*_INFO / RB_DEPTH_INFO
// base in tiles, RB_SURFACE_INFO pitch in pixels and MSAA mode); a 2x MSAA pixel is 1x2 samples, a 4x one 2x2, a
// 64bpp surface takes twice the tiles of a 32bpp one. Here each surface is a host Vulkan image instead, keyed by
// (EDRAM base, pitch, format, MSAA), and EDRAM contents are never shared between differently keyed images (no
// ownership transfer and no per-pixel interlock path in M3: the spec defers both until an effect needs them).
//
// Clears and resolves follow RB_COPY_CONTROL: a resolve (a draw in RB_MODECONTROL edram_mode 6, "copy") copies the
// rectangle of the colour target copy_src_select (0-3) or the depth target (4) to guest memory at
// RB_COPY_DEST_BASE, in the Xenos 2D tiled layout of RB_COPY_DEST_PITCH, RB_COPY_DEST_INFO's format and endian
// swap, then clears the colour (color_clear_enable, colour copies only) and depth (depth_clear_enable) targets in
// that rectangle to RB_COLOR_CLEAR(_LO) / RB_DEPTH_CLEAR. Semantics per Xenia xenos.h, registers.h, draw_util.cc
// and render_target_cache (reference only, no code taken from them).
//
// Thread-safety: GPU thread only. Each clear or resolve is recorded, submitted and waited for on the spot (Task 20
// folds this into the draw command stream).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "common/result.h"
#include "runtime/gpu/pm4.h"
#include "runtime/gpu/registers.h"
#include "runtime/gpu/vk/device.h"

struct VmaAllocation_T;  // same declaration as VK_DEFINE_HANDLE(VmaAllocation) in vk_mem_alloc.h
using VmaAllocation = VmaAllocation_T*;

namespace lr::gpu::vk {

namespace edram {

inline constexpr uint32_t kTileWidthSamples = 80;
inline constexpr uint32_t kTileHeightSamples = 16;
inline constexpr uint32_t kTileCount = 2048;  // EDRAM addressing wraps around at this many tiles
inline constexpr uint32_t kTileBytes = kTileWidthSamples * kTileHeightSamples * 4;  // 5120
inline constexpr uint32_t kResolveAlignment = 8;  // resolve rectangles are 8x8-pixel aligned (GPU_RESOLVE_ALIGNMENT)
inline constexpr uint32_t kDepthSlot = 4;  // bind() slot of the depth target = RB_COPY_CONTROL.copy_src_select 4
inline constexpr uint32_t kMaxRenderTargets = 8;  // host images kept; the least recently used one goes first
inline constexpr uint32_t kMaxHeight = 8192;      // host image rows, at most

// RB_SURFACE_INFO.msaa_samples
inline constexpr uint32_t kMsaa1x = 0, kMsaa2x = 1, kMsaa4x = 2;

// RB_COLOR_INFO.color_format values with a host image format (the others are an InvalidFormat error).
inline constexpr uint32_t kColor_8_8_8_8 = 0, kColor_8_8_8_8_GAMMA = 1, kColor_2_10_10_10 = 2,
                          kColor_16_16_FLOAT = 6, kColor_16_16_16_16_FLOAT = 7, kColor_2_10_10_10_AS_10_10_10_10 = 10,
                          kColor_32_FLOAT = 14, kColor_32_32_FLOAT = 15;
// RB_DEPTH_INFO.depth_format
inline constexpr uint32_t kDepth_D24S8 = 0, kDepth_D24FS8 = 1;

// RB_COPY_DEST_INFO.copy_dest_format (texture format numbers) a colour resolve can write.
inline constexpr uint32_t kDest_8_8_8_8 = 6, kDest_2_10_10_10 = 7, kDest_8_8_8_8_A = 14, kDest_16_16_FLOAT = 31,
                          kDest_16_16_16_16_FLOAT = 32, kDest_32_FLOAT = 36, kDest_32_32_FLOAT = 37,
                          kDest_8_8_8_8_AS_16_16_16_16 = 50, kDest_2_10_10_10_AS_16_16_16_16 = 54;

// RB_COPY_CONTROL.copy_sample_select
inline constexpr uint32_t kSample0 = 0, kSample1 = 1, kSample2 = 2, kSample3 = 3, kSamples01 = 4, kSamples23 = 5,
                          kSamples0123 = 6;

}  // namespace edram

// What a host render target stands for.
struct RenderTargetKey {
  uint32_t base_tiles = 0;  // RB_COLOR*_INFO / RB_DEPTH_INFO bits 0-10 (bit 11 is ignored: addressing wraps at 2048)
  uint32_t pitch = 0;       // RB_SURFACE_INFO.surface_pitch (bits 0-13), pixels
  uint32_t format = 0;      // color_format (bits 16-19), or depth_format (bit 16) when `depth`
  uint32_t msaa = 0;        // RB_SURFACE_INFO.msaa_samples (bits 16-17)
  bool depth = false;
  bool operator==(const RenderTargetKey&) const = default;
};

// 64-bit colour formats (k_16_16_16_16(_FLOAT), k_32_32_FLOAT): twice the EDRAM tiles per row.
bool is_64bpp(const RenderTargetKey& key);
// EDRAM tiles per row of tiles: ceil(pitch * horizontal samples / 80), doubled for 64bpp.
uint32_t pitch_tiles(const RenderTargetKey& key);
// Pixel rows of the host image: as many as the whole EDRAM holds at this pitch (addressing wraps), at most
// edram::kMaxHeight.
uint32_t surface_height(const RenderTargetKey& key);
// The key of slot 0-3 (RB_COLOR_INFO, RB_COLOR1..3_INFO) or edram::kDepthSlot (RB_DEPTH_INFO); nullopt when the
// slot is out of range, the pitch is 0 or the MSAA field is 3 (reserved).
std::optional<RenderTargetKey> render_target_key(const RegisterFile& regs, uint32_t slot);

// A resolve or clear rectangle in render-target pixels; x1/y1 exclusive; all four multiples of 8.
struct ResolveRect {
  uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool empty() const { return x0 >= x1 || y0 >= y1; }
  bool operator==(const ResolveRect&) const = default;
};

// The rectangle a resolve (or clear) covers. Direct3D 9 draws it as three k_32_32_FLOAT vertices in vertex fetch
// constant 0 (6 dwords, any endian swap); with `mem` and such a fetch constant, the rectangle is their bounding box
// (half-pixel offset undone when PA_SU_VTX_CNTL.pix_center is 0, top-left rule at 1/256-pixel precision, moved by
// PA_SC_WINDOW_OFFSET when PA_SU_SC_MODE_CNTL.vtx_window_offset_enable is set). Without them, it is the window
// scissor. Either way it is clamped to the window scissor (PA_SC_WINDOW_SCISSOR_TL/BR, offset by
// PA_SC_WINDOW_OFFSET unless TL.window_offset_disable), widened to 8-pixel alignment and clamped to the surface
// pitch (rounded down to 8).
ResolveRect resolve_rect(const RegisterFile& regs, const MemoryAccess* mem);

// One host render target. The image always stays in VK_IMAGE_LAYOUT_GENERAL (attachment, sampled and transfer
// destination alike); the render pass loads and stores it in that layout.
struct RenderTarget {
  RenderTargetKey key;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;  // host samples (a guest 2x target may use 4)
  uint32_t width = 0, height = 0;                         // pixels
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation alloc = nullptr;
  VkImageView attachment_view = VK_NULL_HANDLE;  // every aspect
  VkImageView sample_view = VK_NULL_HANDLE;      // colour, or the depth aspect
  VkImageView stencil_view = VK_NULL_HANDLE;     // depth targets only: the stencil aspect
  VkRenderPass render_pass = VK_NULL_HANDLE;     // one attachment, load/store, GENERAL layout
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  uint64_t last_use = 0;

  // Host sample of guest sample s. Guest 2x on a 4x host image: guest sample s covers host samples 2s and 2s+1
  // (the upper and lower halves of the pixel in the standard 4x pattern); reads use 2s.
  uint32_t host_sample(uint32_t guest_sample) const;
  // Host sample mask covering the guest samples in guest_mask.
  uint32_t host_sample_mask(uint32_t guest_mask) const;
};

class Edram {
 public:
  // `stop` (may be null): raised, the fence waits give up (wait_fence) and the call fails with Cancelled.
  static Result<std::unique_ptr<Edram>> create(const Device& device, const std::atomic<bool>* stop = nullptr);
  ~Edram();
  Edram(const Edram&) = delete;
  Edram& operator=(const Edram&) = delete;

  // The host render target of slot 0-3 (colour) or edram::kDepthSlot, created (zero-filled) on first use. The
  // pointer stays valid until a later bind() evicts it (more than edram::kMaxRenderTargets keys in use) or the Edram
  // is destroyed. InvalidFormat: a malformed key or a format with no host equivalent.
  Result<RenderTarget*> bind(const RegisterFile& regs, uint32_t slot);
  // The clear half of RB_COPY_CONTROL: colour target copy_src_select when color_clear_enable (and copy_src_select is
  // a colour slot) to RB_COLOR_CLEAR(_LO), the depth target when depth_clear_enable to RB_DEPTH_CLEAR, both in
  // resolve_rect(regs, mem).
  Result<void> clear(const RegisterFile& regs, const MemoryAccess* mem = nullptr);
  // Copies (copy_command 0 raw or 1 convert; 2 and 3 copy nothing) then clears, as above. The guest destination is
  // bounds-checked: bytes past the guest memory size, or a base that maps to no guest memory, are dropped (with one
  // warning), never written. An unsupported destination (3D/array, a format or endian it cannot write) skips the
  // copy with a warning and still clears.
  Result<void> resolve(const RegisterFile& regs, const MemoryAccess& mem);

  std::size_t render_target_count() const;

 private:
  struct Impl;
  explicit Edram(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace lr::gpu::vk
