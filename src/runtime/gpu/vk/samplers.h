// SPDX-License-Identifier: MIT
// The sampler cache (Task 19): one VkSampler per distinct sampler state of the texture fetch constants, stored in
// the set-3 sampler heap of the translated shaders' contract (runtime/gpu/shader/contract.h). Task 20's draw writes
// heap_index(fetch constant) into the shared block's sampler table (offset 384).
//
// Sampler state from the fetch constant (field layout in runtime/gpu/texture_format.h; semantics checked against
// Xenia's Vulkan texture cache, reference only):
//   - address modes per axis (clamp_x/y/z): repeat, mirrored repeat, clamp to edge, mirror once (mirror clamp to
//     edge; mirrored repeat without samplerMirrorClampToEdge), clamp to border. The "halfway" modes have no Vulkan
//     equivalent and become the edge modes; mirror-once-to-border loses its border (mirror clamp to edge). Axes the
//     dimension does not use are clamp to edge (y of 1D, z of 2D; a stacked texture is sampled as 3D, so it keeps z);
//   - mag, min and mip filters (point / linear; mip "base map" samples level min_level only);
//   - anisotropy: aniso_filter n (1..5) is 2^(n-1):1, clamped to the device limit; off without samplerAnisotropy;
//   - mip range: minLod = the first level the texture has (mip_range()), maxLod = the last one;
//   - the fetch constant's LOD bias (dword 4 bits 12-21, signed, /32) as mipLodBias, clamped to the device limit;
//   - border colour: ABGR black -> transparent black, ABGR white -> opaque white, the two YCbCr blacks -> opaque
//     black (Xenia uses transparent black for those). Normalized to transparent black when no axis uses the border,
//     so the colour never splits cache entries.
//   - always normalized coordinates (Task 15.9: the shader divides unnormalized coordinates by the size).
//
// Thread-safety: GPU thread only.
#pragma once

#include <cstdint>
#include <memory>

#include "common/result.h"
#include "runtime/gpu/texture_format.h"
#include "runtime/gpu/vk/device.h"

namespace lr::gpu::vk {

// One bindless heap of the contract: a set layout whose binding 0 holds `count` descriptors of `type` (partially
// bound, update-after-bind) for the vertex and fragment stages, its own pool, and the one set. Shared with the
// texture cache (sets 0-2).
struct DescriptorHeap {
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSet set = VK_NULL_HANDLE;
};
Result<DescriptorHeap> create_descriptor_heap(VkDevice device, VkDescriptorType type, uint32_t count);
void destroy_descriptor_heap(VkDevice device, DescriptorHeap& heap);

// What the device allows (Device::limits / optional features), as a plain value for the pure function below.
struct SamplerLimits {
  bool anisotropy = false;      // samplerAnisotropy enabled
  float max_anisotropy = 1.0f;  // VkPhysicalDeviceLimits::maxSamplerAnisotropy
  float max_lod_bias = 0.0f;    // VkPhysicalDeviceLimits::maxSamplerLodBias
  bool mirror_clamp_to_edge = false;
};
SamplerLimits sampler_limits(const Device& device);

// The sampler a fetch constant asks for (pure: no Vulkan calls; pNext null, flags 0).
VkSamplerCreateInfo sampler_create_info(const TextureFetch& fetch_constant, const SamplerLimits& limits);

inline constexpr uint32_t kSamplerHeapSize = 1024;  // set 3 descriptors; index 0 is a default sampler

class SamplerCache {
 public:
  static Result<std::unique_ptr<SamplerCache>> create(const Device& device);
  ~SamplerCache();
  SamplerCache(const SamplerCache&) = delete;
  SamplerCache& operator=(const SamplerCache&) = delete;

  // The heap index of the fetch constant's sampler, created and written to the heap on first use. Equal sampler
  // states share one entry. A full heap (kSamplerHeapSize distinct states) or a failed vkCreateSampler returns 0, the
  // default sampler (nearest, clamp to edge), with one warning on stderr.
  uint32_t heap_index(const TextureFetch& fetch_constant);

  VkDescriptorSetLayout set_layout() const;  // set 3: binding 0, kSamplerHeapSize samplers, partially bound
  VkDescriptorSet set() const;
  std::size_t size() const;  // distinct samplers created (the default one not counted)

 private:
  struct Impl;
  explicit SamplerCache(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace lr::gpu::vk
