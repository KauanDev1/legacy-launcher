// SPDX-License-Identifier: MIT
// Test helper: runs a vertex + pixel SPIR-V pair on the headless Vulkan device (lr::gpu::vk::Device, lavapipe in the
// dev container) and reads the pixels back. Every translator test whose result is a value goes through it (plan B,
// ADR 0005 M8).
//
// A draw renders in.vertex_count vertices (no vertex input; in.topology, default a triangle list of one triangle,
// which fullscreen_triangle_vs() makes cover the target) into in.width x 1 targets: in.color_targets RGBA32F colour
// attachments (locations 0..3, cleared to 0) and, with in.depth_target, a D32_SFLOAT depth attachment (cleared to
// in.depth_clear, compare ALWAYS, writes on). Pixel i has its centre at (i + 0.5, 0.5). The viewport is
// (0, 0, width, 1), depth 0..1; front faces are counter-clockwise, nothing is culled.
//
// The pipeline layout is the translated shaders' resource interface (runtime/gpu/shader/contract.h), whatever the
// shaders use of it:
//   push constants  128 bytes, vertex and fragment stages. First the contract's device addresses are written
//                   (offset 0: in.vs_constants, 8: in.ps_constants, 16: the shared block, 24: the vertex-fetch
//                   table), then in.push_constants is
//                   copied to offset 0, then the device address of each in.buffers[i] is written as a little-endian
//                   uint64 at its push_offset (later writes win)
//   set 0 binding 0 sampled images, 2D (Texture2D heap)      in.textures with dim 2D, in order: heap index 0, 1, ...
//   set 1 binding 0 sampled images, 3D (Texture3D heap)      likewise for 3D
//   set 2 binding 0 sampled images, cube (TextureCube heap)  likewise for cube
//   set 3 binding 0 samplers (sampler heap)                  in.samplers, heap index = position
//   spec constant 0 in.spec_alpha, both stages
// Each heap holds kHarnessHeapSize descriptors and is partially bound. The fetch_slot fields name the texture fetch
// constant a texture or sampler stands for: the shared block's index tables map that fetch constant to the heap
// index (other fetch constants map to 0).
//
// The shared block (kSharedBlockBytes) holds in.bools, in.loops, ndc_scale, ndc_offset, alpha_ref, vte_flags,
// index_offset, min_index and max_index at their contract offsets. The defaults draw a translated VS as is: identity
// ndc transform, W given as W (kVteW0NotReciprocal), no index offset or clamp.
//
// HarnessInputs fields are added by the task that first needs them:
//   15.3  width, vertex_count, push_constants, buffers, color_targets, depth_target/depth_clear, textures, samplers;
//   15.4  topology, vs_constants/ps_constants (256 float4 each), bools (8 words), loops (32), the shared-block
//         fields (ndc_scale, ndc_offset, vte_flags, index_offset, min_index, max_index, alpha_ref), spec_alpha (spec
//         constant 0);
//   15.8  fetch_slots ({slot, bytes, endian} per vertex-fetch table entry, plus lead and size_bytes).
//
// The vertex-fetch table (contract.h, push-constant offset 24) is always written: kVertexFetchSlots entries, all
// zero (address 0, size 0) except the slots in in.fetch_slots. Each of those gets its own device-address buffer
// holding `bytes`; the entry's address is that buffer's address + `lead` (so bytes[0..lead) lie before the slot start
// and a fetch must never see them), its size is size_bytes (default: bytes.size() - lead, which may claim less than
// the buffer holds), its endian the Xenos swap mode (0 none, 1 8in16, 2 8in32, 3 16in32).
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/result.h"
#include "runtime/gpu/shader/contract.h"
#include "runtime/gpu/vk/device.h"

namespace lr::testing {

inline constexpr uint32_t kHarnessHeapSize = 16;
inline constexpr uint32_t kHarnessPushConstantBytes = 128;

enum class HarnessTextureDim { k2D, k3D, kCube };
enum class HarnessAddress { Repeat, MirroredRepeat, ClampToEdge, ClampToBorder };  // border: transparent black
enum class HarnessFilter { Nearest, Linear };  // min, mag and mip filter
enum class HarnessTopology { TriangleList, PointList };

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

// One vertex-fetch table entry (see the header comment).
struct HarnessFetchSlot {
  uint32_t slot = 0;  // 0..kVertexFetchSlots-1
  std::vector<uint8_t> bytes;
  uint32_t endian = 0;
  uint32_t lead = 0;                     // bytes before the slot start (<= bytes.size(), a multiple of 4)
  std::optional<uint32_t> size_bytes;    // default bytes.size() - lead
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
  HarnessTopology topology = HarnessTopology::TriangleList;

  // The translated shaders' contract (contract.h).
  std::vector<std::array<float, 4>> vs_constants;  // c0, c1, ... (at most 256; the rest are 0)
  std::vector<std::array<float, 4>> ps_constants;
  std::array<uint32_t, 8> bools{};
  std::array<uint32_t, 32> loops{};
  std::array<float, 4> ndc_scale{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 4> ndc_offset{0.0f, 0.0f, 0.0f, 0.0f};
  uint32_t vte_flags = gpu::shader::kVteW0NotReciprocal;
  int32_t index_offset = 0;
  uint32_t min_index = 0;
  uint32_t max_index = 0xFFFFFFFFu;
  float alpha_ref = 0.0f;
  uint32_t spec_alpha = 0;  // spec constant 0
  std::vector<HarnessFetchSlot> fetch_slots;  // the vertex-fetch table (Task 15.8)
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

// Pins the loaded Vulkan driver, layer and LLVM libraries (RTLD_NODELETE) like create() does, for a test that makes
// its own device (a VkBackend): call it while the device exists, so LeakSanitizer can name the driver at exit.
void pin_vulkan_drivers();

class ShaderHarness {
 public:
  // Fails with ErrorCode::ToolMissing when there is no Vulkan driver (tests SKIP then).
  // LeakSanitizer: lavapipe leaks a little thread-local state of its worker threads once it has drawn (112 bytes in
  // two allocations, whether or not the device is destroyed). create() therefore keeps every loaded Vulkan driver and
  // layer mapped to the end of the process (RTLD_NODELETE), so those frames keep their module names, and the
  // harness defines __lsan_default_suppressions() for the driver (libvulkan_lvp, and libgallium where Mesa splits
  // the driver core out), its LLVM and the validation layer.
  static Result<ShaderHarness> create();

  Result<HarnessResult> draw(std::span<const uint32_t> vs, std::span<const uint32_t> ps, const HarnessInputs& in);

  const gpu::vk::Device& device() const { return device_; }

 private:
  explicit ShaderHarness(gpu::vk::Device device) : device_(std::move(device)) {}
  // The draw; every Vulkan object it creates is destroyed when it returns (after vkDeviceWaitIdle), so validation
  // errors from the destruction land in vk_errors too.
  Result<HarnessResult> draw_impl(std::span<const uint32_t> vs, std::span<const uint32_t> ps, const HarnessInputs& in,
                                  std::vector<std::string>& vk_errors);
  gpu::vk::Device device_;
};

}  // namespace lr::testing
