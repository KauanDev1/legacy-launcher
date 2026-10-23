// SPDX-License-Identifier: MIT
#include "runtime/gpu/vk/samplers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <tuple>

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

// Only clamp to border reads the border on the host (mirror-once-to-border becomes mirror clamp to edge).
bool uses_border(ClampMode m) { return m == ClampMode::kClampToBorder; }

VkSamplerAddressMode address_mode(ClampMode m, bool mirror_clamp) {
  switch (m) {
    case ClampMode::kRepeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case ClampMode::kMirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case ClampMode::kClampToEdge:
    case ClampMode::kClampToHalfway: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case ClampMode::kMirrorClampToEdge:
    case ClampMode::kMirrorClampToHalfway:
    case ClampMode::kMirrorClampToBorder:
      return mirror_clamp ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case ClampMode::kClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  }
  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}

VkFilter filter(TextureFilter f) { return f == TextureFilter::kLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; }

// The fields of a VkSamplerCreateInfo that sampler_create_info() can set: the cache key.
using SamplerKey = std::tuple<int, int, int, int, int, int, int, float, int, float, float, float, int>;

SamplerKey key_of(const VkSamplerCreateInfo& c) {
  return {c.magFilter,    c.minFilter,     c.mipmapMode, c.addressModeU, c.addressModeV, c.addressModeW,
          c.anisotropyEnable, c.maxAnisotropy, c.compareEnable, c.minLod, c.maxLod, c.mipLodBias, c.borderColor};
}

}  // namespace

Result<DescriptorHeap> create_descriptor_heap(VkDevice device, VkDescriptorType type, uint32_t count) {
  DescriptorHeap h;
  VkDescriptorSetLayoutBinding binding{};
  binding.binding = 0;
  binding.descriptorType = type;
  binding.descriptorCount = count;
  binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  const VkDescriptorBindingFlags flags =
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
  VkDescriptorSetLayoutBindingFlagsCreateInfo fci =
      make<VkDescriptorSetLayoutBindingFlagsCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
  fci.bindingCount = 1;
  fci.pBindingFlags = &flags;
  VkDescriptorSetLayoutCreateInfo lci = make<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
  lci.pNext = &fci;
  lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
  lci.bindingCount = 1;
  lci.pBindings = &binding;
  if (VkResult r = vkCreateDescriptorSetLayout(device, &lci, nullptr, &h.layout); r != VK_SUCCESS)
    return vk_error("vkCreateDescriptorSetLayout (heap)", r);
  const VkDescriptorPoolSize size{type, count};
  VkDescriptorPoolCreateInfo pci = make<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
  pci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
  pci.maxSets = 1;
  pci.poolSizeCount = 1;
  pci.pPoolSizes = &size;
  if (VkResult r = vkCreateDescriptorPool(device, &pci, nullptr, &h.pool); r != VK_SUCCESS) {
    destroy_descriptor_heap(device, h);
    return vk_error("vkCreateDescriptorPool (heap)", r);
  }
  VkDescriptorSetAllocateInfo ai = make<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
  ai.descriptorPool = h.pool;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &h.layout;
  if (VkResult r = vkAllocateDescriptorSets(device, &ai, &h.set); r != VK_SUCCESS) {
    destroy_descriptor_heap(device, h);
    return vk_error("vkAllocateDescriptorSets (heap)", r);
  }
  return h;
}

void destroy_descriptor_heap(VkDevice device, DescriptorHeap& h) {
  if (h.pool) vkDestroyDescriptorPool(device, h.pool, nullptr);  // frees the set
  if (h.layout) vkDestroyDescriptorSetLayout(device, h.layout, nullptr);
  h = DescriptorHeap{};
}

SamplerLimits sampler_limits(const Device& device) {
  SamplerLimits l;
  l.anisotropy = device.sampler_anisotropy();
  l.max_anisotropy = device.limits().maxSamplerAnisotropy;
  l.max_lod_bias = device.limits().maxSamplerLodBias;
  l.mirror_clamp_to_edge = device.mirror_clamp_to_edge();
  return l;
}

VkSamplerCreateInfo sampler_create_info(const TextureFetch& f, const SamplerLimits& limits) {
  VkSamplerCreateInfo c = make<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
  const TextureDimension dim = f.dimension();
  const bool stacked = dim == TextureDimension::k2DOrStacked && f.stacked();
  ClampMode modes[3] = {f.clamp(0), f.clamp(1), f.clamp(2)};
  if (dim == TextureDimension::k1D) modes[1] = modes[2] = ClampMode::kClampToEdge;
  if (dim == TextureDimension::k2DOrStacked && !stacked) modes[2] = ClampMode::kClampToEdge;
  if (dim == TextureDimension::kCube) modes[0] = modes[1] = modes[2] = ClampMode::kClampToEdge;  // seamless faces
  c.addressModeU = address_mode(modes[0], limits.mirror_clamp_to_edge);
  c.addressModeV = address_mode(modes[1], limits.mirror_clamp_to_edge);
  c.addressModeW = address_mode(modes[2], limits.mirror_clamp_to_edge);

  c.magFilter = filter(f.mag_filter());
  c.minFilter = filter(f.min_filter());
  const TextureFilter mip = f.mip_filter();
  c.mipmapMode = mip == TextureFilter::kLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;

  const uint32_t aniso = f.aniso_filter();
  if (limits.anisotropy && aniso >= uint32_t(AnisoFilter::kMax_2_1) && aniso <= uint32_t(AnisoFilter::kMax_16_1)) {
    const float want = float(1u << (aniso - uint32_t(AnisoFilter::kMax_1_1)));
    const float max = std::max(1.0f, limits.max_anisotropy);
    c.anisotropyEnable = VK_TRUE;
    c.maxAnisotropy = std::min(want, max);
  } else {
    c.maxAnisotropy = 1.0f;
  }

  const MipRange range = mip_range(f);
  c.minLod = float(range.min_level);
  c.maxLod = mip == TextureFilter::kBaseMap ? c.minLod : float(range.max_level);
  const float bias = float(f.lod_bias()) / 32.0f;
  c.mipLodBias = std::clamp(bias, -limits.max_lod_bias, limits.max_lod_bias);

  const bool border = uses_border(modes[0]) || uses_border(modes[1]) || uses_border(modes[2]);
  c.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  if (border) {
    switch (f.border_color()) {
      case BorderColor::k_ABGR_Black: c.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK; break;
      case BorderColor::k_ABGR_White: c.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; break;
      default: c.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK; break;
    }
  }
  c.unnormalizedCoordinates = VK_FALSE;
  return c;
}

struct SamplerCache::Impl {
  VkDevice device = VK_NULL_HANDLE;
  SamplerLimits limits;
  DescriptorHeap heap;
  VkSampler default_sampler = VK_NULL_HANDLE;
  std::map<SamplerKey, uint32_t> indices;
  std::vector<VkSampler> samplers;  // heap index - 1
  bool warned_full = false;

  ~Impl() {
    for (VkSampler s : samplers) vkDestroySampler(device, s, nullptr);
    if (default_sampler) vkDestroySampler(device, default_sampler, nullptr);
    destroy_descriptor_heap(device, heap);
  }

  void write(uint32_t index, VkSampler s) {
    VkDescriptorImageInfo info{s, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet w = make<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    w.dstSet = heap.set;
    w.dstBinding = 0;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
  }

  uint32_t heap_index(const TextureFetch& f) {
    const VkSamplerCreateInfo ci = sampler_create_info(f, limits);
    const SamplerKey key = key_of(ci);
    if (auto it = indices.find(key); it != indices.end()) return it->second;
    if (samplers.size() + 1 >= kSamplerHeapSize) {
      if (!warned_full)
        std::fprintf(stderr, "warning: [gpu] sampler heap full (%u states); using the default sampler\n",
                     kSamplerHeapSize);
      warned_full = true;
      return 0;
    }
    VkSampler s = VK_NULL_HANDLE;
    if (VkResult r = vkCreateSampler(device, &ci, nullptr, &s); r != VK_SUCCESS) {
      std::fprintf(stderr, "warning: [gpu] vkCreateSampler failed (VkResult %d); using the default sampler\n", int(r));
      return 0;
    }
    samplers.push_back(s);
    const uint32_t index = uint32_t(samplers.size());
    write(index, s);
    indices.emplace(key, index);
    return index;
  }
};

Result<std::unique_ptr<SamplerCache>> SamplerCache::create(const Device& device) {
  auto impl = std::make_unique<Impl>();
  impl->device = device.device();
  impl->limits = sampler_limits(device);
  LR_TRY_ASSIGN(heap, create_descriptor_heap(impl->device, VK_DESCRIPTOR_TYPE_SAMPLER, kSamplerHeapSize));
  impl->heap = heap;
  VkSamplerCreateInfo ci = make<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
  ci.magFilter = ci.minFilter = VK_FILTER_NEAREST;
  ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  ci.maxAnisotropy = 1.0f;
  ci.maxLod = VK_LOD_CLAMP_NONE;
  if (VkResult r = vkCreateSampler(impl->device, &ci, nullptr, &impl->default_sampler); r != VK_SUCCESS)
    return vk_error("vkCreateSampler (default)", r);
  impl->write(0, impl->default_sampler);
  return std::unique_ptr<SamplerCache>(new SamplerCache(std::move(impl)));
}

SamplerCache::SamplerCache(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SamplerCache::~SamplerCache() = default;

uint32_t SamplerCache::heap_index(const TextureFetch& fetch_constant) { return impl_->heap_index(fetch_constant); }
VkDescriptorSetLayout SamplerCache::set_layout() const { return impl_->heap.layout; }
VkDescriptorSet SamplerCache::set() const { return impl_->heap.set; }
std::size_t SamplerCache::size() const { return impl_->samplers.size(); }

}  // namespace lr::gpu::vk
