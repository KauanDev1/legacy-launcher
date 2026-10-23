// SPDX-License-Identifier: MIT
// The texture cache (Task 19, M3 spec 5.4): turns texture fetch constants into sampled Vulkan images and keeps them
// in the bindless texture heaps of the translated shaders' contract (runtime/gpu/shader/contract.h): set 0
// Texture2D (1D textures are height-1 2D images), set 1 Texture3D (3D and stacked textures), set 2 TextureCube.
// Index 0 of every heap is a null texture that reads (0, 0, 0, 0): an empty or unbound fetch constant points there.
//
// Loading (runtime/gpu/texture_format.h, on the CPU): the guest bytes are untiled, endian-swapped and converted
// (CTX1 -> R8G8, the 16-bit packed formats -> RGBA8, 10_11_11 / 11_11_10 -> RGBA16) and uploaded through a staging
// buffer; DXT1/2_3/4_5, DXN and DXT5A stay compressed (BC1/2/3, BC5, BC4). Gamma components get an sRGB format, the
// fetch constant's swizzle becomes the view's component mapping (part of the view key: one image, one view per
// swizzle). Signed formats are an error (M3), as is any format without a host equivalent, naming it.
//
// Invalidation: a hash of the guest bytes each texture covers (its footprint), compared the first time the texture
// is used in a frame (begin_frame() starts one; VkBackend calls it on every swap). invalidate_range() marks the
// textures overlapping a range for re-upload at their next use (the EDRAM resolve calls it for what it wrote).
// No page protection: it would have to coexist with recompiled code writing guest memory and with the fault
// handler. A CPU write between two draws of the same frame is seen only in the next frame.
//
// Thread-safety: GPU thread only. Uploads and read-backs are submitted and waited for on the spot (Task 20 folds them
// into the frame's command stream).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "common/result.h"
#include "runtime/gpu/pm4.h"
#include "runtime/gpu/texture_format.h"
#include "runtime/gpu/vk/device.h"

namespace lr::gpu::vk {

// The VkFormat a Xenos format is uploaded as (its linear variant); nullopt for formats the loader does not support.
std::optional<VkFormat> host_format(XenosFormat format);
// The sRGB variant of a host format (gamma components), or nullopt when Vulkan has none.
std::optional<VkFormat> srgb_format(VkFormat format);
// The image view component mapping for a format and the fetch constant's 12-bit swizzle: output r, g, b, a take
// the selected texture component (X..W, a missing one reads as the format's last component), 0 or 1.
VkComponentMapping view_component_mapping(XenosFormat format, uint32_t fetch_swizzle);

// The heap indices of one fetch constant: the heap of its dimension holds its view, the other two point at their
// null texture (index 0). Task 20 writes them into the shared block's tables (offsets 0, 128 and 256).
struct TextureHeapIndices {
  uint32_t texture_2d = 0;
  uint32_t texture_3d = 0;
  uint32_t texture_cube = 0;
  bool operator==(const TextureHeapIndices&) const = default;
};

inline constexpr uint32_t kTextureHeapSize = 4096;          // descriptors per heap (sets 0, 1, 2)
inline constexpr uint64_t kTextureCacheBudget = 1024ull << 20;  // host bytes kept before evicting unused textures

class TextureCache {
 public:
  // `mem`: guest memory (Kernel::gpu_memory_access); `stop` (may be null): raised, fence waits give up (Cancelled).
  static Result<std::unique_ptr<TextureCache>> create(const Device& device, MemoryAccess mem,
                                                      const std::atomic<bool>* stop = nullptr);
  ~TextureCache();
  TextureCache(const TextureCache&) = delete;
  TextureCache& operator=(const TextureCache&) = delete;

  // The view of the texture the fetch constant describes, uploaded (or re-uploaded after a change) as needed. An
  // empty or unbound fetch constant gives the null view of its dimension. Errors (InvalidFormat, naming the format
  // or texture): unsupported or signed formats, textures outside guest memory or too large for the host or device.
  Result<VkImageView> get(const TextureFetch& fetch_constant);
  // get() for fetch constant `fetch_slot` (0..31) of a draw, remembering its heap indices; on an error the slot
  // points at the null textures and the error is returned.
  Result<void> bind(uint32_t fetch_slot, const TextureFetch& fetch_constant);
  TextureHeapIndices heap_index(uint32_t fetch_slot) const;
  // Textures whose guest footprint overlaps [phys, phys + size) are re-uploaded at their next use.
  void invalidate_range(uint32_t phys, uint32_t size);
  // A new frame: every texture's guest bytes are hashed again at its first use.
  void begin_frame();

  VkDescriptorSetLayout set_layout(uint32_t set) const;  // contract set 0, 1 or 2
  VkDescriptorSet set(uint32_t set) const;

  uint64_t textures_created() const;  // images created so far (null textures not counted), for --trace-gpu
  uint64_t uploads() const;           // uploads so far, re-uploads included
  std::size_t image_count() const;
  std::size_t view_count() const;
  // Reads level `level` (all its layers) of the fetch constant's image back from the device, tightly packed as the
  // loader produced it (tests).
  Result<std::vector<uint8_t>> download(const TextureFetch& fetch_constant, uint32_t level);

 private:
  struct Impl;
  explicit TextureCache(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace lr::gpu::vk
