// SPDX-License-Identifier: MIT
#include "runtime/gpu/vk/textures.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "runtime/gpu/vk/samplers.h"
#include "vk_mem_alloc.h"

namespace lr::gpu::vk {
namespace {

template <class T>
T make(VkStructureType type) {
  T s{};
  s.sType = type;
  return s;
}

Error vk_error(const char* what, VkResult r) {
  return Error{ErrorCode::Internal, std::string(what) + " failed (VkResult " + std::to_string(r) + ")", ""};
}

enum Heap : uint32_t { kHeap2D = 0, kHeap3D = 1, kHeapCube = 2 };

Heap heap_of(const TextureDesc& d) {
  if (d.dimension == TextureDimension::kCube) return kHeapCube;
  if (d.dimension == TextureDimension::k3D || d.stacked) return kHeap3D;
  return kHeap2D;
}

Heap heap_of(const TextureFetch& f) {
  if (f.dimension() == TextureDimension::kCube) return kHeapCube;
  if (f.dimension() == TextureDimension::k3D ||
      (f.dimension() == TextureDimension::k2DOrStacked && f.stacked()))
    return kHeap3D;
  return kHeap2D;
}

// The cache key: the description without the swizzle (views) and the flags that only drive logging.
std::vector<uint32_t> key_of(const TextureDesc& d) {
  return {uint32_t(d.format), uint32_t(d.dimension), uint32_t(d.stacked), uint32_t(d.tiled), uint32_t(d.packed_mips),
          d.endian, d.width, d.height, d.depth, d.pitch_texels, d.base_phys, d.mip_phys, d.mips.min_level,
          d.mips.max_level, uint32_t(d.mips.has_base), uint32_t(d.mips.has_mips), uint32_t(d.gamma)};
}

bool overlaps(const std::optional<GuestSpan>& s, uint64_t lo, uint64_t hi) {
  return s && s->size != 0 && s->phys < hi && lo < uint64_t{s->phys} + s->size;
}

}  // namespace

std::optional<VkFormat> host_format(XenosFormat format) {
  switch (format_info(format).host) {
    case HostLayout::kR8: return VK_FORMAT_R8_UNORM;
    case HostLayout::kR8G8: return VK_FORMAT_R8G8_UNORM;
    case HostLayout::kR8G8B8A8: return VK_FORMAT_R8G8B8A8_UNORM;
    case HostLayout::kA2B10G10R10: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case HostLayout::kR16: return VK_FORMAT_R16_UNORM;
    case HostLayout::kR16G16: return VK_FORMAT_R16G16_UNORM;
    case HostLayout::kR16G16B16A16: return VK_FORMAT_R16G16B16A16_UNORM;
    case HostLayout::kR16F: return VK_FORMAT_R16_SFLOAT;
    case HostLayout::kR16G16F: return VK_FORMAT_R16G16_SFLOAT;
    case HostLayout::kR16G16B16A16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case HostLayout::kR32F: return VK_FORMAT_R32_SFLOAT;
    case HostLayout::kR32G32F: return VK_FORMAT_R32G32_SFLOAT;
    case HostLayout::kR32G32B32A32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case HostLayout::kBC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case HostLayout::kBC2: return VK_FORMAT_BC2_UNORM_BLOCK;
    case HostLayout::kBC3: return VK_FORMAT_BC3_UNORM_BLOCK;
    case HostLayout::kBC4: return VK_FORMAT_BC4_UNORM_BLOCK;
    case HostLayout::kBC5: return VK_FORMAT_BC5_UNORM_BLOCK;
    case HostLayout::kNone: break;
  }
  return std::nullopt;
}

std::optional<VkFormat> srgb_format(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8_UNORM: return VK_FORMAT_R8_SRGB;
    case VK_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_SRGB;
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case VK_FORMAT_BC2_UNORM_BLOCK: return VK_FORMAT_BC2_SRGB_BLOCK;
    case VK_FORMAT_BC3_UNORM_BLOCK: return VK_FORMAT_BC3_SRGB_BLOCK;
    default: return std::nullopt;
  }
}

VkComponentMapping view_component_mapping(XenosFormat format, uint32_t fetch_swizzle) {
  const uint32_t components = std::max<uint32_t>(format_info(format).components, 1);
  VkComponentSwizzle out[4];
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t sel = (fetch_swizzle >> (3 * i)) & 7u;
    if (sel < 4)
      out[i] = VkComponentSwizzle(VK_COMPONENT_SWIZZLE_R + std::min(sel, components - 1));
    else if (sel == 5)
      out[i] = VK_COMPONENT_SWIZZLE_ONE;
    else
      out[i] = VK_COMPONENT_SWIZZLE_ZERO;  // 4, and the unused 6 and 7
  }
  return {out[0], out[1], out[2], out[3]};
}

struct TextureCache::Impl {
  struct View {
    uint32_t swizzle = 0;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t heap_index = 0;
  };
  struct Entry {
    TextureDesc desc;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
    uint32_t levels = 1, array_layers = 1;
    std::vector<View> views;
    TextureFootprint footprint;
    uint64_t hash = 0;
    uint64_t checked_frame = UINT64_MAX;
    uint64_t last_use = 0;
    bool dirty = false;
  };
  struct NullTexture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkImageView view = VK_NULL_HANDLE;
  };

  const Device& device;
  MemoryAccess mem;
  const std::atomic<bool>* stop;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  std::array<DescriptorHeap, 3> heaps{};
  std::array<std::vector<uint32_t>, 3> free_indices;  // per heap, popped from the back
  std::array<NullTexture, 3> nulls{};
  std::map<std::vector<uint32_t>, Entry> entries;
  std::array<TextureHeapIndices, kShaderConstantFetchCount> slots{};
  uint64_t frame = 0;
  uint64_t created = 0, upload_count = 0;
  uint64_t resident_bytes = 0;
  std::set<std::string> logged;
  std::map<VkFormat, bool> linear_ok;  // filterable() cache
  std::function<void()> after_hash_hook;  // tests: runs between the hash and the load (set_after_hash_hook)
  // Flushes the frame's command stream (VkBackend): runs before this cache submits work, destroys an image or view
  // or rewrites a descriptor, so commands recorded earlier never see the change and nothing pending is touched.
  std::function<Result<void>()> before_gpu_work;

  Impl(const Device& d, MemoryAccess m, const std::atomic<bool>* s) : device(d), mem(std::move(m)), stop(s) {}
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  ~Impl() {
    const VkDevice d = device.device();
    if (d) vkDeviceWaitIdle(d);
    for (auto& [key, e] : entries) destroy(e);
    for (NullTexture& n : nulls) {
      if (n.view) vkDestroyImageView(d, n.view, nullptr);
      if (n.image) vmaDestroyImage(device.allocator(), n.image, n.alloc);
    }
    for (DescriptorHeap& h : heaps) destroy_descriptor_heap(d, h);
    if (fence) vkDestroyFence(d, fence, nullptr);
    if (pool) vkDestroyCommandPool(d, pool, nullptr);
  }

  bool filterable(VkFormat f) {
    if (auto it = linear_ok.find(f); it != linear_ok.end()) return it->second;
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(device.physical_device(), f, &props);
    const bool ok = (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    linear_ok.emplace(f, ok);
    return ok;
  }

  bool sampleable(VkFormat f) const {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(device.physical_device(), f, &props);
    return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0 &&
           (props.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) != 0;
  }

  void log_once(const std::string& msg) {
    if (logged.insert(msg).second) std::fprintf(stderr, "[gpu] textures: %s\n", msg.c_str());
  }

  void destroy(Entry& e) {
    const VkDevice d = device.device();
    for (View& v : e.views) {
      if (v.view) vkDestroyImageView(d, v.view, nullptr);
      if (v.heap_index != 0) {
        write_descriptor(heap_of(e.desc), v.heap_index, nulls[heap_of(e.desc)].view);
        free_indices[heap_of(e.desc)].push_back(v.heap_index);
      }
    }
    e.views.clear();
    if (e.image) vmaDestroyImage(device.allocator(), e.image, e.alloc);
    e.image = VK_NULL_HANDLE;
  }

  void write_descriptor(uint32_t heap, uint32_t index, VkImageView view) {
    VkDescriptorImageInfo info{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = make<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    w.dstSet = heaps[heap].set;
    w.dstBinding = 0;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(device.device(), 1, &w, 0, nullptr);
  }

  Result<void> init() {
    const VkDevice d = device.device();
    VkCommandPoolCreateInfo cpci = make<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = device.graphics_family();
    if (VkResult r = vkCreateCommandPool(d, &cpci, nullptr, &pool); r != VK_SUCCESS)
      return vk_error("vkCreateCommandPool (textures)", r);
    VkCommandBufferAllocateInfo cbai = make<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (VkResult r = vkAllocateCommandBuffers(d, &cbai, &cmd); r != VK_SUCCESS)
      return vk_error("vkAllocateCommandBuffers (textures)", r);
    VkFenceCreateInfo fci = make<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    if (VkResult r = vkCreateFence(d, &fci, nullptr, &fence); r != VK_SUCCESS) return vk_error("vkCreateFence", r);
    for (uint32_t h = 0; h < 3; ++h) {
      LR_TRY_ASSIGN(heap, create_descriptor_heap(d, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kTextureHeapSize));
      heaps[h] = heap;
      for (uint32_t i = kTextureHeapSize - 1; i >= 1; --i) free_indices[h].push_back(i);
    }
    LR_TRY(create_nulls());
    return {};
  }

  // --- command submission -------------------------------------------------------------------------------------------
  Result<void> flush_others() {
    if (before_gpu_work) return before_gpu_work();
    return {};
  }

  Result<void> begin() {
    LR_TRY(flush_others());
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo b = make<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (VkResult r = vkBeginCommandBuffer(cmd, &b); r != VK_SUCCESS) return vk_error("vkBeginCommandBuffer", r);
    return {};
  }

  Result<void> submit() {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = make<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (VkResult r = vkQueueSubmit(device.graphics_queue(), 1, &si, fence); r != VK_SUCCESS)
      return vk_error("vkQueueSubmit (textures)", r);
    if (!wait_fence(device.device(), fence, stop))
      return Error{ErrorCode::Cancelled, "texture upload abandoned: the GPU is stopping", ""};
    vkResetFences(device.device(), 1, &fence);
    return {};
  }

  void barrier(VkImage image, uint32_t levels, uint32_t layers, VkImageLayout from, VkImageLayout to,
               VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
               VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = make<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
  }

  struct Staging {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    void* map = nullptr;
  };
  Result<Staging> make_staging(VkDeviceSize bytes, bool readback) {
    Staging s;
    VkBufferCreateInfo bci = make<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    bci.size = std::max<VkDeviceSize>(bytes, 4);
    bci.usage = readback ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = (readback ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                          : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT) |
                VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (VkResult r = vmaCreateBuffer(device.allocator(), &bci, &aci, &s.buffer, &s.alloc, &info); r != VK_SUCCESS)
      return vk_error("vmaCreateBuffer (texture staging)", r);
    s.map = info.pMappedData;
    return s;
  }
  void free_staging(Staging& s) {
    if (s.buffer) vmaDestroyBuffer(device.allocator(), s.buffer, s.alloc);
    s = Staging{};
  }

  // --- images -------------------------------------------------------------------------------------------------------
  Result<void> create_image(VkImageType type, VkFormat format, VkExtent3D extent, uint32_t levels, uint32_t layers,
                            bool cube, VkImage& image, VmaAllocation& alloc) {
    VkImageCreateInfo ici = make<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    ici.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ici.imageType = type;
    ici.format = format;
    ici.extent = extent;
    ici.mipLevels = levels;
    ici.arrayLayers = layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageFormatProperties props{};
    if (vkGetPhysicalDeviceImageFormatProperties(device.physical_device(), format, type, ici.tiling, ici.usage,
                                                 ici.flags, &props) != VK_SUCCESS ||
        props.maxExtent.width < extent.width || props.maxExtent.height < extent.height ||
        props.maxExtent.depth < extent.depth || props.maxMipLevels < levels || props.maxArrayLayers < layers)
      return Error{ErrorCode::InvalidFormat,
                   "the Vulkan device cannot sample a " + std::to_string(extent.width) + "x" +
                       std::to_string(extent.height) + "x" + std::to_string(extent.depth) + " image of VkFormat " +
                       std::to_string(int(format)) + " with " + std::to_string(levels) + " levels",
                   ""};
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (VkResult r = vmaCreateImage(device.allocator(), &ici, &aci, &image, &alloc, nullptr); r != VK_SUCCESS)
      return vk_error("vmaCreateImage (texture)", r);
    return {};
  }

  Result<VkImageView> create_view(VkImage image, VkImageViewType type, VkFormat format, VkComponentMapping mapping,
                                  uint32_t levels, uint32_t layers) {
    VkImageViewCreateInfo vci = make<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    vci.image = image;
    vci.viewType = type;
    vci.format = format;
    vci.components = mapping;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
    VkImageView view = VK_NULL_HANDLE;
    if (VkResult r = vkCreateImageView(device.device(), &vci, nullptr, &view); r != VK_SUCCESS)
      return vk_error("vkCreateImageView (texture)", r);
    return view;
  }

  // Copies `bytes` (levels laid out as `levels` says) into `image` and leaves it SHADER_READ_ONLY_OPTIMAL.
  Result<void> upload(VkImage image, const std::vector<uint8_t>& bytes, const std::vector<HostLevel>& levels,
                      bool is_3d, uint32_t array_layers) {
    LR_TRY_ASSIGN(staging, make_staging(bytes.size(), false));
    if (!bytes.empty()) std::memcpy(staging.map, bytes.data(), bytes.size());
    vmaFlushAllocation(device.allocator(), staging.alloc, 0, VK_WHOLE_SIZE);
    std::vector<VkBufferImageCopy> regions;
    for (uint32_t l = 0; l < levels.size(); ++l) {
      const HostLevel& hl = levels[l];
      VkBufferImageCopy r{};
      r.bufferOffset = hl.offset;
      r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, array_layers};
      r.imageExtent = {hl.width, hl.height, is_3d ? hl.layers : 1};
      regions.push_back(r);
    }
    auto r = begin();
    if (r) {
      const uint32_t level_count = uint32_t(levels.size());
      barrier(image, level_count, array_layers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
              VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
      vkCmdCopyBufferToImage(cmd, staging.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             uint32_t(regions.size()), regions.data());
      barrier(image, level_count, array_layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
      r = submit();
    }
    free_staging(staging);
    if (r) ++upload_count;
    return r;
  }

  Result<void> create_nulls() {
    // 1x1 RGBA8 zero images: 2D, 3D, cube (6 layers); identity views read (0, 0, 0, 0).
    const std::vector<uint8_t> zeros(6 * 4, 0);
    for (uint32_t h = 0; h < 3; ++h) {
      NullTexture& n = nulls[h];
      const bool cube = h == kHeapCube;
      const uint32_t layers = cube ? 6 : 1;
      LR_TRY(create_image(h == kHeap3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {1, 1, 1}, 1,
                          layers, cube, n.image, n.alloc));
      HostLevel level;
      level.width = level.height = 1;
      level.layers = layers;
      level.size = 4 * layers;
      LR_TRY(upload(n.image, std::vector<uint8_t>(zeros.begin(), zeros.begin() + 4 * layers), {level}, h == kHeap3D,
                    layers));
      const VkImageViewType type = h == kHeap3D ? VK_IMAGE_VIEW_TYPE_3D : cube ? VK_IMAGE_VIEW_TYPE_CUBE
                                                                              : VK_IMAGE_VIEW_TYPE_2D;
      const VkComponentMapping identity{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
      LR_TRY_ASSIGN(view, create_view(n.image, type, VK_FORMAT_R8G8B8A8_UNORM, identity, 1, layers));
      n.view = view;
      // Every heap entry starts at the null texture, so a stale index never reads a destroyed view.
      for (uint32_t i = 0; i < kTextureHeapSize; ++i) write_descriptor(h, i, n.view);
    }
    upload_count = 0;
    return {};
  }

  // --- the cache ----------------------------------------------------------------------------------------------------
  Result<void> load_into(Entry& e) {
    LR_TRY_ASSIGN(fp, texture_footprint(e.desc, mem));
    // Hash first, then load: the guest CPU may write the texture concurrently. A write between the two leaves a hash
    // of the older bytes with the newer data uploaded, so the next check sees a difference and uploads once more.
    // The other order could hash bytes newer than the upload and keep a stale texture until the next change.
    const uint64_t hash = hash_footprint(fp, mem);
    if (after_hash_hook) after_hash_hook();
    LR_TRY_ASSIGN(host, load_texture(e.desc, mem, fp));
    e.footprint = fp;
    e.hash = hash;
    LR_TRY(upload(e.image, host.bytes, host.levels, heap_of(e.desc) == kHeap3D, e.array_layers));
    e.dirty = false;
    e.checked_frame = frame;
    return {};
  }

  Result<void> evict_for(uint64_t bytes) {
    bool flushed = false;
    while (resident_bytes + bytes > kTextureCacheBudget) {
      auto victim = entries.end();
      for (auto it = entries.begin(); it != entries.end(); ++it)
        if (it->second.last_use < frame && (victim == entries.end() || it->second.last_use < victim->second.last_use))
          victim = it;
      if (victim == entries.end()) return {};  // everything is in use this frame: go over the budget
      if (!flushed) {
        LR_TRY(flush_others());
        flushed = true;
      }
      resident_bytes -= victim->second.footprint.host_bytes;
      destroy(victim->second);
      entries.erase(victim);
    }
    return {};
  }

  Result<Entry*> create_entry(const TextureDesc& desc) {
    const FormatInfo& info = format_info(desc.format);
    std::optional<VkFormat> format = host_format(desc.format);
    if (!format) return Error{ErrorCode::InvalidFormat, std::string("texture format ") + info.name + " is not supported", ""};
    if (desc.gamma) {
      if (std::optional<VkFormat> s = srgb_format(*format); s && sampleable(*s)) {
        format = *s;
      } else if (s) {
        log_once(std::string("gamma ignored for ") + info.name + ": the device cannot sample VkFormat " +
                 std::to_string(int(*s)) + "; using the UNORM format");
      } else {
        log_once(std::string("gamma ignored for ") + info.name + " (no sRGB host format)");
      }
    }
    LR_TRY_ASSIGN(fp, texture_footprint(desc, mem));  // validates the guest range and the host size first
    LR_TRY(evict_for(fp.host_bytes));

    Entry e;
    e.desc = desc;
    e.format = *format;
    e.levels = desc.mips.max_level + 1;
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkExtent3D extent{desc.width, desc.dimension == TextureDimension::k1D ? 1 : desc.height, 1};
    bool cube = false;
    switch (heap_of(desc)) {
      case kHeap3D:
        type = VK_IMAGE_TYPE_3D;
        extent.depth = desc.depth;
        e.view_type = VK_IMAGE_VIEW_TYPE_3D;
        break;
      case kHeapCube:
        cube = true;
        e.array_layers = 6;
        e.view_type = VK_IMAGE_VIEW_TYPE_CUBE;
        break;
      default:
        e.view_type = VK_IMAGE_VIEW_TYPE_2D;
        break;
    }
    LR_TRY(create_image(type, e.format, extent, e.levels, e.array_layers, cube, e.image, e.alloc));
    if (auto r = load_into(e); !r) {
      vmaDestroyImage(device.allocator(), e.image, e.alloc);
      return r.error();
    }
    ++created;
    resident_bytes += e.footprint.host_bytes;
    auto [it, inserted] = entries.emplace(key_of(desc), std::move(e));
    return &it->second;
  }

  Result<Entry*> lookup(const TextureDesc& desc) {
    auto it = entries.find(key_of(desc));
    if (it == entries.end()) return create_entry(desc);
    Entry& e = it->second;
    if (e.dirty || e.checked_frame != frame) {
      LR_TRY_ASSIGN(fp, texture_footprint(e.desc, mem));
      const uint64_t h = hash_footprint(fp, mem);
      if (e.dirty || h != e.hash) {
        LR_TRY(load_into(e));
      } else {
        e.footprint = fp;
        e.checked_frame = frame;
      }
    }
    return &e;
  }

  Result<View*> view_of(Entry& e, uint32_t swizzle) {
    for (View& v : e.views)
      if (v.swizzle == swizzle) return &v;
    LR_TRY_ASSIGN(view, create_view(e.image, e.view_type, e.format, view_component_mapping(e.desc.format, swizzle),
                                    e.levels, e.array_layers));
    View v;
    v.swizzle = swizzle;
    v.view = view;
    std::vector<uint32_t>& free = free_indices[heap_of(e.desc)];
    if (free.empty()) {
      vkDestroyImageView(device.device(), view, nullptr);
      return Error{ErrorCode::Internal,
                   "texture heap full (" + std::to_string(kTextureHeapSize - 1) + " views of one dimension)", ""};
    }
    if (auto r = flush_others(); !r) {
      vkDestroyImageView(device.device(), view, nullptr);
      return r.error();
    }
    v.heap_index = free.back();
    free.pop_back();
    write_descriptor(heap_of(e.desc), v.heap_index, v.view);
    e.views.push_back(v);
    return &e.views.back();
  }

  void note(const TextureFetch& f, const TextureDesc& d) {
    if (d.biased) log_once("\"unsigned biased\" texture components (2v - 1) are not applied (M3 limit)");
    if (f.exp_adjust() != 0) log_once("texture exp_adjust is not applied (M3 limit)");
    if (d.stacked)
      log_once(std::string("stacked texture ") + describe(d) + " sampled as 3D" +
               (d.mips.max_level > 0 ? " (mipped: each level halves the layer count; M3 limit)" : ""));
  }

  Result<std::pair<Entry*, View*>> resolve(const TextureFetch& f) {
    LR_TRY_ASSIGN(desc, describe_texture(f));
    if (desc.null) return std::pair<Entry*, View*>{nullptr, nullptr};
    note(f, desc);
    const uint32_t swizzle = desc.swizzle;
    desc.swizzle = 0;
    desc.biased = false;
    LR_TRY_ASSIGN(entry, lookup(desc));
    entry->last_use = frame;
    LR_TRY_ASSIGN(view, view_of(*entry, swizzle));
    return std::pair<Entry*, View*>{entry, view};
  }
};

Result<std::unique_ptr<TextureCache>> TextureCache::create(const Device& device, MemoryAccess mem,
                                                           const std::atomic<bool>* stop) {
  auto impl = std::make_unique<Impl>(device, std::move(mem), stop);
  LR_TRY(impl->init());
  return std::unique_ptr<TextureCache>(new TextureCache(std::move(impl)));
}

TextureCache::TextureCache(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TextureCache::~TextureCache() = default;

Result<VkImageView> TextureCache::get(const TextureFetch& fetch_constant) {
  LR_TRY_ASSIGN(found, impl_->resolve(fetch_constant));
  if (!found.second) return impl_->nulls[heap_of(fetch_constant)].view;
  return found.second->view;
}

Result<void> TextureCache::bind(uint32_t fetch_slot, const TextureFetch& fetch_constant) {
  TextureHeapIndices& slot = impl_->slots[fetch_slot % kShaderConstantFetchCount];
  slot = TextureHeapIndices{};
  LR_TRY_ASSIGN(found, impl_->resolve(fetch_constant));
  if (!found.second) return {};
  slot.filter_linear = impl_->filterable(found.first->format);
  switch (heap_of(found.first->desc)) {
    case kHeap2D: slot.texture_2d = found.second->heap_index; break;
    case kHeap3D: slot.texture_3d = found.second->heap_index; break;
    case kHeapCube: slot.texture_cube = found.second->heap_index; break;
  }
  return {};
}

TextureHeapIndices TextureCache::heap_index(uint32_t fetch_slot) const {
  return impl_->slots[fetch_slot % kShaderConstantFetchCount];
}

void TextureCache::invalidate_range(uint32_t phys, uint32_t size) {
  const uint64_t lo = phys & pm4::kPhysicalMask, hi = lo + size;
  for (auto& [key, e] : impl_->entries)
    if (overlaps(e.footprint.base, lo, hi) || overlaps(e.footprint.mips, lo, hi)) e.dirty = true;
}

void TextureCache::begin_frame() { ++impl_->frame; }

void TextureCache::set_after_hash_hook(std::function<void()> hook) { impl_->after_hash_hook = std::move(hook); }

void TextureCache::set_before_gpu_work(std::function<Result<void>()> hook) {
  impl_->before_gpu_work = std::move(hook);
}

VkDescriptorSetLayout TextureCache::set_layout(uint32_t set) const { return impl_->heaps[std::min(set, 2u)].layout; }
VkDescriptorSet TextureCache::set(uint32_t set) const { return impl_->heaps[std::min(set, 2u)].set; }
uint64_t TextureCache::textures_created() const { return impl_->created; }
uint64_t TextureCache::uploads() const { return impl_->upload_count; }
std::size_t TextureCache::image_count() const { return impl_->entries.size(); }

std::size_t TextureCache::view_count() const {
  std::size_t n = 0;
  for (const auto& [key, e] : impl_->entries) n += e.views.size();
  return n;
}

Result<std::vector<uint8_t>> TextureCache::download(const TextureFetch& fetch_constant, uint32_t level) {
  Impl& m = *impl_;
  LR_TRY_ASSIGN(found, m.resolve(fetch_constant));
  if (!found.first) return Error{ErrorCode::InvalidFormat, "download: null texture", ""};
  Impl::Entry& e = *found.first;
  if (level >= e.levels) return Error{ErrorCode::InvalidFormat, "download: no such level", ""};
  const FormatInfo& info = format_info(e.desc.format);
  const uint32_t w = std::max(e.desc.width >> level, 1u);
  const uint32_t h = e.desc.dimension == TextureDimension::k1D ? 1 : std::max(e.desc.height >> level, 1u);
  const bool is_3d = heap_of(e.desc) == kHeap3D;
  const uint32_t d = is_3d ? std::max(e.desc.depth >> level, 1u) : 1;
  const uint32_t layers = is_3d ? d : e.array_layers;
  const VkDeviceSize bytes = VkDeviceSize{(w + info.host_block_width - 1) / info.host_block_width} *
                             ((h + info.host_block_height - 1) / info.host_block_height) * layers *
                             info.host_bytes_per_block;
  LR_TRY_ASSIGN(staging, m.make_staging(bytes, true));
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, e.array_layers};
  region.imageExtent = {w, h, d};
  auto r = m.begin();
  if (r) {
    m.barrier(e.image, e.levels, e.array_layers, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
              VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyImageToBuffer(m.cmd, e.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &region);
    m.barrier(e.image, e.levels, e.array_layers, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    VkMemoryBarrier mb = make<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(m.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
                         nullptr);
    r = m.submit();
  }
  std::vector<uint8_t> out;
  if (r) {
    vmaInvalidateAllocation(m.device.allocator(), staging.alloc, 0, VK_WHOLE_SIZE);
    out.assign(static_cast<const uint8_t*>(staging.map), static_cast<const uint8_t*>(staging.map) + bytes);
  }
  m.free_staging(staging);
  if (!r) return r.error();
  return out;
}

}  // namespace lr::gpu::vk
