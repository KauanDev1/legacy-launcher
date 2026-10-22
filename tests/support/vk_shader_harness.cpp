// SPDX-License-Identifier: MIT
#include "support/vk_shader_harness.h"

#include <dlfcn.h>
#include <link.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "common/log.h"
#include "runtime/gpu/shader/contract.h"
#include "runtime/gpu/shader/spirv_builder.h"
#include "vk_mem_alloc.h"

namespace lr::testing {
namespace {

namespace sh = lr::gpu::shader;

Error vk_error(const char* what, VkResult r) {
  return Error{ErrorCode::Internal, std::string(what) + " failed (VkResult " + std::to_string(r) + ")", ""};
}

#define HARNESS_VK(call)                                \
  do {                                                  \
    const VkResult harness_r_ = (call);                 \
    if (harness_r_ != VK_SUCCESS) return vk_error(#call, harness_r_); \
  } while (0)

// Runs `first` (vkDeviceWaitIdle), then the registered destroy calls in reverse order.
struct Cleanup {
  std::function<void()> first;
  std::vector<std::function<void()>> fns;
  ~Cleanup() {
    if (first) first();
    for (auto it = fns.rbegin(); it != fns.rend(); ++it) (*it)();
  }
  void add(std::function<void()> fn) { fns.push_back(std::move(fn)); }
};

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation alloc = nullptr;
  void* mapped = nullptr;
};

Result<Buffer> make_host_buffer(VmaAllocator allocator, Cleanup& cleanup, VkDeviceSize size, VkBufferUsageFlags usage,
                                bool readback) {
  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = std::max<VkDeviceSize>(size, 16);
  bci.usage = usage;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VmaAllocationCreateInfo aci{};
  aci.usage = VMA_MEMORY_USAGE_AUTO;
  aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | (readback ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                                                           : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
  Buffer b;
  VmaAllocationInfo info{};
  HARNESS_VK(vmaCreateBuffer(allocator, &bci, &aci, &b.buffer, &b.alloc, &info));
  b.mapped = info.pMappedData;
  cleanup.add([allocator, b] { vmaDestroyBuffer(allocator, b.buffer, b.alloc); });
  return b;
}

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation alloc = nullptr;
  VkImageView view = VK_NULL_HANDLE;
};

Result<Image> make_image(VkDevice dev, VmaAllocator allocator, Cleanup& cleanup, VkImageType type,
                         VkImageViewType view_type, VkFormat format, VkExtent3D extent, uint32_t mips, uint32_t layers,
                         VkImageUsageFlags usage, VkImageAspectFlags aspect) {
  VkImageCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ici.flags = view_type == VK_IMAGE_VIEW_TYPE_CUBE ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
  ici.imageType = type;
  ici.format = format;
  ici.extent = extent;
  ici.mipLevels = mips;
  ici.arrayLayers = layers;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = usage;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VmaAllocationCreateInfo aci{};
  aci.usage = VMA_MEMORY_USAGE_AUTO;
  Image img;
  HARNESS_VK(vmaCreateImage(allocator, &ici, &aci, &img.image, &img.alloc, nullptr));
  cleanup.add([allocator, img] { vmaDestroyImage(allocator, img.image, img.alloc); });

  VkImageViewCreateInfo vci{};
  vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vci.image = img.image;
  vci.viewType = view_type;
  vci.format = format;
  vci.subresourceRange = {aspect, 0, mips, 0, layers};
  HARNESS_VK(vkCreateImageView(dev, &vci, nullptr, &img.view));
  const VkImageView view = img.view;
  cleanup.add([dev, view] { vkDestroyImageView(dev, view, nullptr); });
  return img;
}

void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                   VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
                   VkPipelineStageFlags dst_stage) {
  VkImageMemoryBarrier b{};
  b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b.srcAccessMask = src_access;
  b.dstAccessMask = dst_access;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
  vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

VkSamplerAddressMode address_mode(HarnessAddress a) {
  switch (a) {
    case HarnessAddress::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case HarnessAddress::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case HarnessAddress::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case HarnessAddress::ClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  }
  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}

uint32_t mip_size(uint32_t size, uint32_t mip) { return std::max<uint32_t>(size >> mip, 1); }

// Marks every loaded Vulkan driver (and its libgallium), layer and LLVM library RTLD_NODELETE, so the loader's dlclose at
// vkDestroyInstance leaves them mapped and LeakSanitizer can still name them at exit (see the header).
void pin_vulkan_libraries() {
  std::vector<std::string> paths;
  dl_iterate_phdr(
      [](dl_phdr_info* info, size_t, void* data) {
        const std::string_view name = info->dlpi_name ? info->dlpi_name : "";
        if (name.find("libvulkan_") != std::string_view::npos || name.find("libVkLayer") != std::string_view::npos ||
            name.find("libLLVM") != std::string_view::npos || name.find("libgallium") != std::string_view::npos)
          static_cast<std::vector<std::string>*>(data)->emplace_back(name);
        return 0;
      },
      &paths);
  for (const std::string& p : paths) dlopen(p.c_str(), RTLD_LAZY | RTLD_NOLOAD | RTLD_NODELETE);
}

}  // namespace

std::vector<uint32_t> fullscreen_triangle_vs() {
  sh::SpirvBuilder b(spv::ExecutionModelVertex);
  const sh::Id f32 = b.type_float(32);
  const sh::Id i32 = b.type_int(32, true);
  const sh::Id vec4 = b.type_vector(f32, 4);
  const sh::Id index_var = b.variable(spv::StorageClassInput, b.type_pointer(spv::StorageClassInput, i32), "vertex_index");
  b.decorate(index_var, spv::DecorationBuiltIn, {spv::BuiltInVertexIndex});
  const sh::Id pos_var = b.variable(spv::StorageClassOutput, b.type_pointer(spv::StorageClassOutput, vec4), "position");
  b.decorate(pos_var, spv::DecorationBuiltIn, {spv::BuiltInPosition});

  b.begin_main();
  const sh::Id index = b.op(spv::OpLoad, i32, {index_var});
  const sh::Id xi = b.op(spv::OpBitwiseAnd, i32, {index, b.constant_i32(1)});
  const sh::Id yi = b.op(spv::OpShiftRightArithmetic, i32, {index, b.constant_i32(1)});
  const sh::Id four = b.constant_f32(4.0f);
  const sh::Id one = b.constant_f32(1.0f);
  const sh::Id x = b.op(spv::OpFSub, f32, {b.op(spv::OpFMul, f32, {b.op(spv::OpConvertSToF, f32, {xi}), four}), one});
  const sh::Id y = b.op(spv::OpFSub, f32, {b.op(spv::OpFMul, f32, {b.op(spv::OpConvertSToF, f32, {yi}), four}), one});
  const sh::Id pos = b.op(spv::OpCompositeConstruct, vec4, {x, y, b.constant_f32(0.0f), one});
  b.op_void(spv::OpStore, {pos_var, pos});
  b.end_main();
  const sh::Id iface[] = {index_var, pos_var};
  b.entry_point("main", iface);
  return b.finish();
}

Result<ShaderHarness> ShaderHarness::create() {
  gpu::vk::DeviceOptions opts;
  opts.headless = true;
  opts.validation = true;  // when the layer is installed (the dev container has it); errors fail draw()
  LR_TRY_ASSIGN(device, gpu::vk::Device::create(opts));
  pin_vulkan_libraries();
  return ShaderHarness(std::move(device));
}

Result<HarnessResult> ShaderHarness::draw(std::span<const uint32_t> vs, std::span<const uint32_t> ps,
                                          const HarnessInputs& in) {
  // Validation-layer errors (logged by Device's messenger) fail the draw, including those raised while draw_impl
  // destroys its objects; the previous sink comes back afterwards.
  std::vector<std::string> vk_errors;
  log::Sink previous = log::set_sink([&vk_errors](log::Level level, std::string_view msg) {
    if (level == log::Level::Error) vk_errors.emplace_back(msg);
    std::fprintf(stderr, "%.*s\n", static_cast<int>(msg.size()), msg.data());
  });
  auto result = draw_impl(vs, ps, in, vk_errors);
  log::set_sink(std::move(previous));
  if (!vk_errors.empty())
    return Error{ErrorCode::Internal, "harness: Vulkan validation error: " + vk_errors.front(), ""};
  return result;
}

Result<HarnessResult> ShaderHarness::draw_impl(std::span<const uint32_t> vs, std::span<const uint32_t> ps,
                                               const HarnessInputs& in, std::vector<std::string>&) {
  namespace sc = gpu::shader;
  if (in.width == 0) return Error{ErrorCode::Internal, "harness: width is 0", ""};
  if (in.vs_constants.size() > sc::kFloatConstantCount || in.ps_constants.size() > sc::kFloatConstantCount)
    return Error{ErrorCode::Internal, "harness: more than 256 float constants", ""};
  if (in.color_targets < 1 || in.color_targets > 4)
    return Error{ErrorCode::Internal, "harness: color_targets must be 1..4", ""};
  if (in.push_constants.size() > kHarnessPushConstantBytes)
    return Error{ErrorCode::Internal, "harness: push constants larger than 128 bytes", ""};
  if (in.samplers.size() > kHarnessHeapSize) return Error{ErrorCode::Internal, "harness: too many samplers", ""};

  const VkDevice dev = device_.device();
  const VmaAllocator allocator = device_.allocator();
  Cleanup cleanup;
  cleanup.first = [dev] { vkDeviceWaitIdle(dev); };

  // --- shader modules ---
  auto make_module = [&](std::span<const uint32_t> code, VkShaderModule& out) -> Result<void> {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size_bytes();
    ci.pCode = code.data();
    HARNESS_VK(vkCreateShaderModule(dev, &ci, nullptr, &out));
    const VkShaderModule m = out;
    cleanup.add([dev, m] { vkDestroyShaderModule(dev, m, nullptr); });
    return {};
  };
  if (vs.empty() || ps.empty()) return Error{ErrorCode::Internal, "harness: empty shader module", ""};
  VkShaderModule vs_module = VK_NULL_HANDLE, ps_module = VK_NULL_HANDLE;
  LR_TRY(make_module(vs, vs_module));
  LR_TRY(make_module(ps, ps_module));

  // --- descriptor heaps: sets 0..3 ---
  const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  std::array<VkDescriptorSetLayout, 4> set_layouts{};
  for (uint32_t s = 0; s < 4; ++s) {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = s == 3 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    binding.descriptorCount = kHarnessHeapSize;
    binding.stageFlags = stages;
    const VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    fci.bindingCount = 1;
    fci.pBindingFlags = &flags;
    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.pNext = &fci;
    lci.bindingCount = 1;
    lci.pBindings = &binding;
    HARNESS_VK(vkCreateDescriptorSetLayout(dev, &lci, nullptr, &set_layouts[s]));
    const VkDescriptorSetLayout l = set_layouts[s];
    cleanup.add([dev, l] { vkDestroyDescriptorSetLayout(dev, l, nullptr); });
  }

  VkPushConstantRange push_range{stages, 0, kHarnessPushConstantBytes};
  VkPipelineLayoutCreateInfo plci{};
  plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  plci.setLayoutCount = 4;
  plci.pSetLayouts = set_layouts.data();
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &push_range;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  HARNESS_VK(vkCreatePipelineLayout(dev, &plci, nullptr, &layout));
  cleanup.add([dev, layout] { vkDestroyPipelineLayout(dev, layout, nullptr); });

  const VkDescriptorPoolSize pool_sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 * kHarnessHeapSize},
                                             {VK_DESCRIPTOR_TYPE_SAMPLER, kHarnessHeapSize}};
  VkDescriptorPoolCreateInfo dpci{};
  dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dpci.maxSets = 4;
  dpci.poolSizeCount = 2;
  dpci.pPoolSizes = pool_sizes;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  HARNESS_VK(vkCreateDescriptorPool(dev, &dpci, nullptr, &pool));
  cleanup.add([dev, pool] { vkDestroyDescriptorPool(dev, pool, nullptr); });
  VkDescriptorSetAllocateInfo dsai{};
  dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  dsai.descriptorPool = pool;
  dsai.descriptorSetCount = 4;
  dsai.pSetLayouts = set_layouts.data();
  std::array<VkDescriptorSet, 4> sets{};
  HARNESS_VK(vkAllocateDescriptorSets(dev, &dsai, sets.data()));

  // --- command buffer ---
  VkCommandPoolCreateInfo cpci{};
  cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpci.queueFamilyIndex = device_.graphics_family();
  VkCommandPool cmd_pool = VK_NULL_HANDLE;
  HARNESS_VK(vkCreateCommandPool(dev, &cpci, nullptr, &cmd_pool));
  cleanup.add([dev, cmd_pool] { vkDestroyCommandPool(dev, cmd_pool, nullptr); });
  VkCommandBufferAllocateInfo cbai{};
  cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbai.commandPool = cmd_pool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  HARNESS_VK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
  VkCommandBufferBeginInfo cbbi{};
  cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  HARNESS_VK(vkBeginCommandBuffer(cmd, &cbbi));

  // --- textures: upload through a staging buffer, then one descriptor per texture in its dimension's heap ---
  std::array<uint32_t, 3> heap_fill{};
  std::array<std::array<uint32_t, 32>, 4> heap_tables{};  // 2D, 3D, cube, sampler: heap index per fetch constant
  for (const HarnessTexture& t : in.textures) {
    const bool cube = t.dim == HarnessTextureDim::kCube;
    const bool is_3d = t.dim == HarnessTextureDim::k3D;
    const uint32_t layers = cube ? 6 : 1;
    const uint32_t depth = is_3d ? t.d : 1;
    if (t.w == 0 || t.h == 0 || depth == 0 || t.mips == 0 || (cube && t.w != t.h))
      return Error{ErrorCode::Internal, "harness: bad texture size", ""};
    std::vector<VkBufferImageCopy> regions;
    VkDeviceSize texel_count = 0;
    for (uint32_t m = 0; m < t.mips; ++m) {
      VkBufferImageCopy r{};
      r.bufferOffset = texel_count * sizeof(std::array<float, 4>);
      r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, layers};
      r.imageExtent = {mip_size(t.w, m), mip_size(t.h, m), mip_size(depth, m)};
      regions.push_back(r);
      texel_count += VkDeviceSize{r.imageExtent.width} * r.imageExtent.height * r.imageExtent.depth * layers;
    }
    if (texel_count != t.texels.size())
      return Error{ErrorCode::Internal,
                   "harness: texture has " + std::to_string(t.texels.size()) + " texels, expected " +
                       std::to_string(texel_count),
                   ""};
    const auto heap = static_cast<size_t>(t.dim);
    if (heap_fill[heap] >= kHarnessHeapSize) return Error{ErrorCode::Internal, "harness: too many textures", ""};

    LR_TRY_ASSIGN(staging, make_host_buffer(allocator, cleanup, texel_count * 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                            false));
    std::memcpy(staging.mapped, t.texels.data(), texel_count * 16);
    vmaFlushAllocation(allocator, staging.alloc, 0, VK_WHOLE_SIZE);
    const VkImageViewType view_type = cube ? VK_IMAGE_VIEW_TYPE_CUBE : is_3d ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    LR_TRY_ASSIGN(img, make_image(dev, allocator, cleanup, is_3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, view_type,
                                  VK_FORMAT_R32G32B32A32_SFLOAT, {t.w, t.h, depth}, t.mips, layers,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT));
    image_barrier(cmd, img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyBufferToImage(cmd, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(regions.size()), regions.data());
    image_barrier(cmd, img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    VkDescriptorImageInfo info{VK_NULL_HANDLE, img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = sets[heap];
    w.dstBinding = 0;
    if (t.fetch_slot < 32) heap_tables[heap][t.fetch_slot] = heap_fill[heap];
    w.dstArrayElement = heap_fill[heap]++;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
  }

  for (uint32_t i = 0; i < in.samplers.size(); ++i) {
    const HarnessSampler& s = in.samplers[i];
    const VkFilter filter = s.filter == HarnessFilter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = filter;
    sci.minFilter = filter;
    sci.mipmapMode =
        s.filter == HarnessFilter::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = address_mode(s.address);
    sci.mipLodBias = s.lod_bias;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler sampler = VK_NULL_HANDLE;
    HARNESS_VK(vkCreateSampler(dev, &sci, nullptr, &sampler));
    cleanup.add([dev, sampler] { vkDestroySampler(dev, sampler, nullptr); });
    VkDescriptorImageInfo info{sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = sets[3];
    w.dstBinding = 0;
    if (s.fetch_slot < 32) heap_tables[3][s.fetch_slot] = i;
    w.dstArrayElement = i;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
  }

  // --- push constants and the buffers they address ---
  // A device-address buffer holding `bytes`; returns its address.
  auto address_buffer = [&](std::span<const uint8_t> bytes) -> Result<uint64_t> {
    LR_TRY_ASSIGN(buf, make_host_buffer(allocator, cleanup, bytes.size(),
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                        false));
    if (!bytes.empty()) std::memcpy(buf.mapped, bytes.data(), bytes.size());
    vmaFlushAllocation(allocator, buf.alloc, 0, VK_WHOLE_SIZE);
    VkBufferDeviceAddressInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    ai.buffer = buf.buffer;
    return uint64_t{vkGetBufferDeviceAddress(dev, &ai)};
  };
  std::array<uint8_t, kHarnessPushConstantBytes> push{};
  {
    // The contract: float constants and the shared block, at their push-constant offsets.
    auto constants = [](const std::vector<std::array<float, 4>>& c) {
      std::vector<uint8_t> bytes(sc::kFloatConstantBytes, 0);
      if (!c.empty()) std::memcpy(bytes.data(), c.data(), c.size() * 16);
      return bytes;
    };
    std::vector<uint8_t> shared(sc::kSharedBlockBytes, 0);
    auto put = [&shared](uint32_t offset, const void* data, size_t size) {
      std::memcpy(shared.data() + offset, data, size);
    };
    put(sc::kSharedTexture2DIndex, heap_tables[0].data(), 128);
    put(sc::kSharedTexture3DIndex, heap_tables[1].data(), 128);
    put(sc::kSharedTextureCubeIndex, heap_tables[2].data(), 128);
    put(sc::kSharedSamplerIndex, heap_tables[3].data(), 128);
    put(sc::kSharedBools, in.bools.data(), sizeof(in.bools));
    put(sc::kSharedLoops, in.loops.data(), sizeof(in.loops));
    put(sc::kSharedNdcScale, in.ndc_scale.data(), 16);
    put(sc::kSharedNdcOffset, in.ndc_offset.data(), 16);
    put(sc::kSharedAlphaRef, &in.alpha_ref, 4);
    put(sc::kSharedVteFlags, &in.vte_flags, 4);
    put(sc::kSharedIndexOffset, &in.index_offset, 4);
    put(sc::kSharedMinIndex, &in.min_index, 4);
    put(sc::kSharedMaxIndex, &in.max_index, 4);
    const std::pair<uint32_t, std::vector<uint8_t>> blocks[] = {{sc::kPushVsConstants, constants(in.vs_constants)},
                                                                {sc::kPushPsConstants, constants(in.ps_constants)},
                                                                {sc::kPushSharedBlock, std::move(shared)}};
    for (const auto& [offset, bytes] : blocks) {
      LR_TRY_ASSIGN(address, address_buffer(bytes));
      std::memcpy(push.data() + offset, &address, sizeof(address));
    }
  }
  std::copy(in.push_constants.begin(), in.push_constants.end(), push.begin());
  for (const HarnessBuffer& hb : in.buffers) {
    if (hb.push_offset % 8 != 0 || hb.push_offset + 8 > kHarnessPushConstantBytes)
      return Error{ErrorCode::Internal, "harness: buffer push_offset out of range or unaligned", ""};
    LR_TRY_ASSIGN(address, address_buffer(hb.bytes));
    std::memcpy(push.data() + hb.push_offset, &address, sizeof(address));
  }

  // --- render targets, render pass, framebuffer ---
  const VkExtent3D extent{in.width, 1, 1};
  std::vector<Image> colors;
  std::vector<VkAttachmentDescription> attachments;
  std::vector<VkAttachmentReference> color_refs;
  std::vector<VkImageView> views;
  for (uint32_t t = 0; t < in.color_targets; ++t) {
    LR_TRY_ASSIGN(img, make_image(dev, allocator, cleanup, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D,
                                  VK_FORMAT_R32G32B32A32_SFLOAT, extent, 1, 1,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT));
    colors.push_back(img);
    views.push_back(img.view);
    VkAttachmentDescription a{};
    a.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachments.push_back(a);
    color_refs.push_back({t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
  }
  Image depth_img;
  VkAttachmentReference depth_ref{in.color_targets, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  if (in.depth_target) {
    LR_TRY_ASSIGN(img, make_image(dev, allocator, cleanup, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D,
                                  VK_FORMAT_D32_SFLOAT, extent, 1, 1,
                                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                  VK_IMAGE_ASPECT_DEPTH_BIT));
    depth_img = img;
    views.push_back(img.view);
    VkAttachmentDescription a{};
    a.format = VK_FORMAT_D32_SFLOAT;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachments.push_back(a);
  }

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = in.color_targets;
  subpass.pColorAttachments = color_refs.data();
  subpass.pDepthStencilAttachment = in.depth_target ? &depth_ref : nullptr;
  // The copies after the pass read what the pass wrote.
  VkSubpassDependency dep{};
  dep.srcSubpass = 0;
  dep.dstSubpass = VK_SUBPASS_EXTERNAL;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
  dep.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  VkRenderPassCreateInfo rpci{};
  rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rpci.attachmentCount = static_cast<uint32_t>(attachments.size());
  rpci.pAttachments = attachments.data();
  rpci.subpassCount = 1;
  rpci.pSubpasses = &subpass;
  rpci.dependencyCount = 1;
  rpci.pDependencies = &dep;
  VkRenderPass render_pass = VK_NULL_HANDLE;
  HARNESS_VK(vkCreateRenderPass(dev, &rpci, nullptr, &render_pass));
  cleanup.add([dev, render_pass] { vkDestroyRenderPass(dev, render_pass, nullptr); });

  VkFramebufferCreateInfo fbci{};
  fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fbci.renderPass = render_pass;
  fbci.attachmentCount = static_cast<uint32_t>(views.size());
  fbci.pAttachments = views.data();
  fbci.width = in.width;
  fbci.height = 1;
  fbci.layers = 1;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  HARNESS_VK(vkCreateFramebuffer(dev, &fbci, nullptr, &framebuffer));
  cleanup.add([dev, framebuffer] { vkDestroyFramebuffer(dev, framebuffer, nullptr); });

  // --- pipeline ---
  VkPipelineShaderStageCreateInfo stage_ci[2]{};
  stage_ci[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stage_ci[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stage_ci[0].module = vs_module;
  stage_ci[0].pName = "main";
  stage_ci[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stage_ci[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stage_ci[1].module = ps_module;
  stage_ci[1].pName = "main";
  const VkSpecializationMapEntry spec_entry{sc::kSpecAlphaTest, 0, sizeof(uint32_t)};
  VkSpecializationInfo spec{};
  spec.mapEntryCount = 1;
  spec.pMapEntries = &spec_entry;
  spec.dataSize = sizeof(in.spec_alpha);
  spec.pData = &in.spec_alpha;
  stage_ci[0].pSpecializationInfo = &spec;
  stage_ci[1].pSpecializationInfo = &spec;
  VkPipelineVertexInputStateCreateInfo vertex_input{};
  vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo assembly{};
  assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  assembly.topology = in.topology == HarnessTopology::PointList ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                                                                : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkViewport viewport{0.0f, 0.0f, static_cast<float>(in.width), 1.0f, 0.0f, 1.0f};
  VkRect2D scissor{{0, 0}, {in.width, 1}};
  VkPipelineViewportStateCreateInfo viewport_state{};
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.viewportCount = 1;
  viewport_state.pViewports = &viewport;
  viewport_state.scissorCount = 1;
  viewport_state.pScissors = &scissor;
  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth_state{};
  depth_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depth_state.depthTestEnable = in.depth_target ? VK_TRUE : VK_FALSE;
  depth_state.depthWriteEnable = in.depth_target ? VK_TRUE : VK_FALSE;
  depth_state.depthCompareOp = VK_COMPARE_OP_ALWAYS;
  std::vector<VkPipelineColorBlendAttachmentState> blend(in.color_targets);
  for (auto& bs : blend)
    bs.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend_state{};
  blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend_state.attachmentCount = in.color_targets;
  blend_state.pAttachments = blend.data();
  VkGraphicsPipelineCreateInfo gpci{};
  gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  gpci.stageCount = 2;
  gpci.pStages = stage_ci;
  gpci.pVertexInputState = &vertex_input;
  gpci.pInputAssemblyState = &assembly;
  gpci.pViewportState = &viewport_state;
  gpci.pRasterizationState = &raster;
  gpci.pMultisampleState = &multisample;
  gpci.pDepthStencilState = &depth_state;
  gpci.pColorBlendState = &blend_state;
  gpci.layout = layout;
  gpci.renderPass = render_pass;
  VkPipeline pipeline = VK_NULL_HANDLE;
  HARNESS_VK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline));
  cleanup.add([dev, pipeline] { vkDestroyPipeline(dev, pipeline, nullptr); });

  // --- readback buffers ---
  std::vector<Buffer> color_readback;
  for (uint32_t t = 0; t < in.color_targets; ++t) {
    LR_TRY_ASSIGN(buf, make_host_buffer(allocator, cleanup, VkDeviceSize{in.width} * 16,
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
    color_readback.push_back(buf);
  }
  Buffer depth_readback;
  if (in.depth_target) {
    LR_TRY_ASSIGN(buf, make_host_buffer(allocator, cleanup, VkDeviceSize{in.width} * 4,
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
    depth_readback = buf;
  }

  // --- record: draw, then copy every target out ---
  std::vector<VkClearValue> clears(attachments.size());
  if (in.depth_target) clears.back().depthStencil = {in.depth_clear, 0};
  VkRenderPassBeginInfo rpbi{};
  rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rpbi.renderPass = render_pass;
  rpbi.framebuffer = framebuffer;
  rpbi.renderArea = scissor;
  rpbi.clearValueCount = static_cast<uint32_t>(clears.size());
  rpbi.pClearValues = clears.data();
  vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 4, sets.data(), 0, nullptr);
  vkCmdPushConstants(cmd, layout, stages, 0, kHarnessPushConstantBytes, push.data());
  vkCmdDraw(cmd, in.vertex_count, 1, 0, 0);
  vkCmdEndRenderPass(cmd);

  for (uint32_t t = 0; t < in.color_targets; ++t) {
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = extent;
    vkCmdCopyImageToBuffer(cmd, colors[t].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, color_readback[t].buffer, 1, &r);
  }
  if (in.depth_target) {
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    r.imageExtent = extent;
    vkCmdCopyImageToBuffer(cmd, depth_img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depth_readback.buffer, 1, &r);
  }
  VkMemoryBarrier host_barrier{};
  host_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  host_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_barrier, 0,
                       nullptr, 0, nullptr);
  HARNESS_VK(vkEndCommandBuffer(cmd));

  VkFenceCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  HARNESS_VK(vkCreateFence(dev, &fci, nullptr, &fence));
  cleanup.add([dev, fence] { vkDestroyFence(dev, fence, nullptr); });
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  HARNESS_VK(vkQueueSubmit(device_.graphics_queue(), 1, &si, fence));
  HARNESS_VK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));

  HarnessResult result;
  for (uint32_t t = 0; t < in.color_targets; ++t) {
    vmaInvalidateAllocation(allocator, color_readback[t].alloc, 0, VK_WHOLE_SIZE);
    result.color[t].resize(in.width);
    std::memcpy(result.color[t].data(), color_readback[t].mapped, size_t{in.width} * 16);
  }
  if (in.depth_target) {
    vmaInvalidateAllocation(allocator, depth_readback.alloc, 0, VK_WHOLE_SIZE);
    result.depth.resize(in.width);
    std::memcpy(result.depth.data(), depth_readback.mapped, size_t{in.width} * 4);
  }
  return result;
}

}  // namespace lr::testing

// LeakSanitizer's default suppressions for lr_tests (read once at startup; unused without the sanitizer): the Vulkan
// driver's and validation layer's own allocations, see ShaderHarness::create(). Our objects are destroyed in draw().
extern "C" const char* __lsan_default_suppressions() {
  // libgallium: newer Mesa builds move lavapipe's core (and its worker-thread state) out of libvulkan_lvp.so into
  // libgallium-<version>.so, so the same leak is reported under that module name.
  return "leak:libvulkan_lvp.so\n"
         "leak:libgallium\n"
         "leak:libLLVM\n"
         "leak:libVkLayer_khronos_validation.so\n";
}
