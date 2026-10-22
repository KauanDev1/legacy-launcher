// SPDX-License-Identifier: MIT
// Test helper: runs a vertex + pixel SPIR-V pair on the headless Vulkan device (lr::gpu::vk::Device, lavapipe in the
// dev container) and reads the pixels back. Every translator test whose result is a value goes through it (plan B,
// ADR 0005 M8).
//
// A draw renders in.vertex_count vertices (a triangle list with no vertex input; default one triangle, which
// fullscreen_triangle_vs() makes cover the target) into in.width x 1 targets: in.color_targets RGBA32F colour
// attachments (locations 0..3, cleared to 0) and, with in.depth_target, a D32_SFLOAT depth attachment (cleared to
// in.depth_clear, compare ALWAYS, writes on). Pixel i has its centre at (i + 0.5, 0.5).
//
// The pipeline layout is the translated shaders' resource interface, whatever the shaders use of it:
//   push constants  128 bytes, vertex and fragment stages; in.push_constants is copied to offset 0, then the device
//                   address of each in.buffers[i] is written as a little-endian uint64 at its push_offset
//   set 0 binding 0 sampled images, 2D (Texture2D heap)      in.textures with dim 2D, in order: heap index 0, 1, ...
//   set 1 binding 0 sampled images, 3D (Texture3D heap)      likewise for 3D
//   set 2 binding 0 sampled images, cube (TextureCube heap)  likewise for cube
//   set 3 binding 0 samplers (sampler heap)                  in.samplers, heap index = position
// Each heap holds kHarnessHeapSize descriptors and is partially bound. The fetch_slot fields name the texture fetch
// constant a texture or sampler stands for; Task 15.4 writes the shared block's index tables (heap index per fetch
// constant) from them. Until then a shader indexes the heaps directly.
//
// HarnessInputs fields are added by the task that first needs them:
//   15.3  width, vertex_count, push_constants, buffers, color_targets, depth_target/depth_clear, textures, samplers;
//   15.4  vs_constants/ps_constants (256 float4 each), bools (8 words), loops (32), the shared-block overrides
//         (ndc_scale, ndc_offset, vte_flags, index_offset, min_index, max_index, alpha_ref), spec_alpha (spec
//         constant 0);
//   15.8  fetch_slots ({slot, bytes, endian} per vertex-fetch table entry).
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/result.h"
#include "runtime/gpu/vk/device.h"

namespace lr::testing {

inline constexpr uint32_t kHarnessHeapSize = 16;
inline constexpr uint32_t kHarnessPushConstantBytes = 128;

enum class HarnessTextureDim { k2D, k3D, kCube };
enum class HarnessAddress { Repeat, MirroredRepeat, ClampToEdge, ClampToBorder };  // border: transparent black
enum class HarnessFilter { Nearest, Linear };  // min, mag and mip filter

// A byte blob the shader reads through its buffer device address.
struct HarnessBuffer {
  std::vector<uint8_t> bytes;
  uint32_t push_offset = 0;  // where the uint64 address goes in the push constants (8-byte aligned)
};

struct HarnessTexture {
  uint32_t fetch_slot = 0;
  HarnessTextureDim dim = HarnessTextureDim::k2D;
  uint32_t w = 1, h = 1, d = 1;  // d: 3D depth only
  uint32_t mips = 1;
  // RGBA32F texels: mip 0 first, then each smaller mip (max(size >> m, 1) per axis). Within a mip: cube faces in
  // Vulkan order (+X, -X, +Y, -Y, +Z, -Z) or 3D slices, each row-major.
  std::vector<std::array<float, 4>> texels;
};

struct HarnessSampler {
  uint32_t fetch_slot = 0;
  HarnessAddress address = HarnessAddress::ClampToEdge;  // u, v and w
  HarnessFilter filter = HarnessFilter::Nearest;
  float lod_bias = 0.0f;
};

struct HarnessInputs {
  uint32_t width = 4;
  uint32_t vertex_count = 3;
  std::vector<uint8_t> push_constants;  // at most kHarnessPushConstantBytes
  std::vector<HarnessBuffer> buffers;
  uint32_t color_targets = 1;  // 1..4
  bool depth_target = false;
  float depth_clear = 1.0f;
  std::vector<HarnessTexture> textures;
  std::vector<HarnessSampler> samplers;
};

struct HarnessResult {
  std::array<std::vector<std::array<float, 4>>, 4> color;  // color[t][i]: target t, pixel i; empty past color_targets
  std::vector<float> depth;                                // empty without a depth target
};

// The bytes of trivially copyable values, for push constants and buffers.
template <class T>
std::vector<uint8_t> harness_bytes(std::span<const T> values) {
  static_assert(std::is_trivially_copyable_v<T>);
  std::vector<uint8_t> out(values.size_bytes());
  if (!out.empty()) std::memcpy(out.data(), values.data(), out.size());
  return out;
}

// A vertex shader (made with SpirvBuilder) for the default draw: vertex i at ((i & 1) * 4 - 1, (i >> 1) * 4 - 1),
// one triangle that covers the whole target, with no outputs but the position.
std::vector<uint32_t> fullscreen_triangle_vs();

class ShaderHarness {
 public:
  // Fails with ErrorCode::ToolMissing when there is no Vulkan driver (tests SKIP then).
  // LeakSanitizer: lavapipe leaks a little thread-local state of its worker threads once it has drawn (112 bytes in
  // two allocations, whether or not the device is destroyed). create() therefore keeps every loaded Vulkan driver and
  // layer mapped to the end of the process (RTLD_NODELETE), so those frames keep their module names, and the
  // harness defines __lsan_default_suppressions() for the driver, its LLVM and the validation layer.
  static Result<ShaderHarness> create();

  Result<HarnessResult> draw(std::span<const uint32_t> vs, std::span<const uint32_t> ps, const HarnessInputs& in);

  const gpu::vk::Device& device() const { return device_; }

 private:
  explicit ShaderHarness(gpu::vk::Device device) : device_(std::move(device)) {}
  gpu::vk::Device device_;
};

}  // namespace lr::testing
