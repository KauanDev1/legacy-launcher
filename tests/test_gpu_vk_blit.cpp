// SPDX-License-Identifier: MIT
// The renderer's own GLSL shaders (src/runtime/gpu/vk/shaders), compiled by glslangValidator at build time and
// embedded in lr_vulkan (cmake/LrGpuShaders.cmake).
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "lr_gpu_shaders.h"
#include "support/spirv_tools.h"
#include "support/vk_shader_harness.h"
#include "vk_mem_alloc.h"

namespace shaders = lr::gpu::vk::shaders;

namespace {

std::span<const uint32_t> words_of(const shaders::BuiltinShader& s) { return {s.words, s.word_count}; }

// Runs the registered undo functions in reverse order.
class Cleanup {
 public:
  void add(std::function<void()> f) { fns_.push_back(std::move(f)); }
  ~Cleanup() {
    for (auto it = fns_.rbegin(); it != fns_.rend(); ++it) (*it)();
  }

 private:
  std::vector<std::function<void()>> fns_;
};

template <class T>
T make(VkStructureType type) {
  T s{};
  s.sType = type;
  return s;
}

#define BLIT_VK(expr) REQUIRE((expr) == VK_SUCCESS)

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation alloc = nullptr;
  VkImageView view = VK_NULL_HANDLE;
};

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation alloc = nullptr;
  void* mapped = nullptr;
};

}  // namespace

TEST_CASE("every built-in shader passes spirv-val", "[unit][gpu]") {
  if (!lr::testing::spirv_tools_available()) SKIP("spirv-val/spirv-dis not installed");
  REQUIRE(shaders::kBuiltinShaderCount >= 5);
  for (std::size_t i = 0; i < shaders::kBuiltinShaderCount; ++i) {
    const auto& s = shaders::kBuiltinShaders[i];
    INFO(s.name);
    REQUIRE(s.word_count > 5);
    CHECK(s.words[0] == 0x07230203u);  // SPIR-V magic: the embedded words have the right endianness
    CHECK(reinterpret_cast<std::uintptr_t>(s.words) % alignof(std::uint32_t) == 0);
    const auto err = lr::testing::spirv_val(words_of(s));
    CHECK_FALSE(err.has_value());
    if (err) UNSCOPED_INFO(*err);
  }
}

TEST_CASE("blit shader copies a texture to an offscreen image", "[unit][vulkan]") {
  auto h = lr::testing::ShaderHarness::create();
  if (!h.ok()) SKIP("no Vulkan ICD installed");
  const auto& dev = h.value().device();
  const VkDevice d = dev.device();
  constexpr uint32_t kSize = 8;
  constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;
  Cleanup cleanup;

  // Source texels: a pattern that tells every pixel apart.
  std::vector<uint32_t> src_pixels(kSize * kSize);
  for (uint32_t y = 0; y < kSize; ++y)
    for (uint32_t x = 0; x < kSize; ++x)
      src_pixels[y * kSize + x] = (x * 30u) | ((y * 30u) << 8) | ((x + y * 8u) << 16) | (0xFFu << 24);
  const VkDeviceSize image_bytes = sizeof(uint32_t) * kSize * kSize;

  auto make_buffer = [&](VkBufferUsageFlags usage, VmaAllocationCreateFlags flags) {
    VkBufferCreateInfo bci = make<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    bci.size = image_bytes;
    bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = flags | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    Buffer b;
    VmaAllocationInfo info{};
    BLIT_VK(vmaCreateBuffer(dev.allocator(), &bci, &aci, &b.buffer, &b.alloc, &info));
    b.mapped = info.pMappedData;
    cleanup.add([a = dev.allocator(), b] { vmaDestroyBuffer(a, b.buffer, b.alloc); });
    return b;
  };
  auto make_image = [&](VkImageUsageFlags usage) {
    VkImageCreateInfo ici = make<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = kFormat;
    ici.extent = {kSize, kSize, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    Image img;
    BLIT_VK(vmaCreateImage(dev.allocator(), &ici, &aci, &img.image, &img.alloc, nullptr));
    cleanup.add([a = dev.allocator(), img] { vmaDestroyImage(a, img.image, img.alloc); });
    VkImageViewCreateInfo vci = make<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    vci.image = img.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = kFormat;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    BLIT_VK(vkCreateImageView(d, &vci, nullptr, &img.view));
    cleanup.add([d, v = img.view] { vkDestroyImageView(d, v, nullptr); });
    return img;
  };

  Buffer staging = make_buffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
  REQUIRE(staging.mapped != nullptr);
  std::memcpy(staging.mapped, src_pixels.data(), image_bytes);
  Buffer readback = make_buffer(VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
  REQUIRE(readback.mapped != nullptr);
  std::memset(readback.mapped, 0xCD, image_bytes);
  Image source = make_image(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  Image target = make_image(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

  // Sampler, descriptor set (one combined image sampler at set 0 binding 0, like blit.frag).
  VkSamplerCreateInfo sci = make<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
  sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
  sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VkSampler sampler = VK_NULL_HANDLE;
  BLIT_VK(vkCreateSampler(d, &sci, nullptr, &sampler));
  cleanup.add([d, sampler] { vkDestroySampler(d, sampler, nullptr); });

  VkDescriptorSetLayoutBinding binding{};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo dslci = make<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
  dslci.bindingCount = 1;
  dslci.pBindings = &binding;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  BLIT_VK(vkCreateDescriptorSetLayout(d, &dslci, nullptr, &set_layout));
  cleanup.add([d, set_layout] { vkDestroyDescriptorSetLayout(d, set_layout, nullptr); });

  VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
  VkDescriptorPoolCreateInfo dpci = make<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
  dpci.maxSets = 1;
  dpci.poolSizeCount = 1;
  dpci.pPoolSizes = &pool_size;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  BLIT_VK(vkCreateDescriptorPool(d, &dpci, nullptr, &pool));
  cleanup.add([d, pool] { vkDestroyDescriptorPool(d, pool, nullptr); });
  VkDescriptorSetAllocateInfo dsai = make<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
  dsai.descriptorPool = pool;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &set_layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  BLIT_VK(vkAllocateDescriptorSets(d, &dsai, &set));
  VkDescriptorImageInfo dii{sampler, source.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet write = make<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
  write.dstSet = set;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.pImageInfo = &dii;
  vkUpdateDescriptorSets(d, 1, &write, 0, nullptr);

  // Render pass, framebuffer, pipeline.
  VkAttachmentDescription att{};
  att.format = kFormat;
  att.samples = VK_SAMPLE_COUNT_1_BIT;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;
  // The copy after the render pass reads what the subpass wrote: an explicit external dependency (colour attachment
  // writes, then the final layout transition, before transfer reads) keeps synchronization validation clean.
  VkSubpassDependency after{};
  after.srcSubpass = 0;
  after.dstSubpass = VK_SUBPASS_EXTERNAL;
  after.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  after.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
  after.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  after.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  VkRenderPassCreateInfo rpci = make<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
  rpci.attachmentCount = 1;
  rpci.pAttachments = &att;
  rpci.subpassCount = 1;
  rpci.pSubpasses = &sub;
  rpci.dependencyCount = 1;
  rpci.pDependencies = &after;
  VkRenderPass render_pass = VK_NULL_HANDLE;
  BLIT_VK(vkCreateRenderPass(d, &rpci, nullptr, &render_pass));
  cleanup.add([d, render_pass] { vkDestroyRenderPass(d, render_pass, nullptr); });

  VkFramebufferCreateInfo fbci = make<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
  fbci.renderPass = render_pass;
  fbci.attachmentCount = 1;
  fbci.pAttachments = &target.view;
  fbci.width = fbci.height = kSize;
  fbci.layers = 1;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  BLIT_VK(vkCreateFramebuffer(d, &fbci, nullptr, &framebuffer));
  cleanup.add([d, framebuffer] { vkDestroyFramebuffer(d, framebuffer, nullptr); });

  auto make_module = [&](const char* name) {
    for (std::size_t i = 0; i < shaders::kBuiltinShaderCount; ++i) {
      const auto& s = shaders::kBuiltinShaders[i];
      if (std::string(s.name) != name) continue;
      VkShaderModuleCreateInfo smci = make<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
      smci.codeSize = s.word_count * sizeof(uint32_t);
      smci.pCode = s.words;
      VkShaderModule m = VK_NULL_HANDLE;
      BLIT_VK(vkCreateShaderModule(d, &smci, nullptr, &m));
      cleanup.add([d, m] { vkDestroyShaderModule(d, m, nullptr); });
      return m;
    }
    FAIL("no built-in shader " << name);
    return VkShaderModule{};
  };
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = make_module("blit.vert");
  stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = make_module("blit.frag");
  stages[1].pName = "main";

  VkPipelineLayoutCreateInfo plci = make<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &set_layout;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  BLIT_VK(vkCreatePipelineLayout(d, &plci, nullptr, &layout));
  cleanup.add([d, layout] { vkDestroyPipelineLayout(d, layout, nullptr); });

  VkPipelineVertexInputStateCreateInfo vi = make<VkPipelineVertexInputStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
  VkPipelineInputAssemblyStateCreateInfo ia = make<VkPipelineInputAssemblyStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkViewport viewport{0.0f, 0.0f, float(kSize), float(kSize), 0.0f, 1.0f};
  VkRect2D scissor{{0, 0}, {kSize, kSize}};
  VkPipelineViewportStateCreateInfo vp = make<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
  vp.viewportCount = vp.scissorCount = 1;
  vp.pViewports = &viewport;
  vp.pScissors = &scissor;
  VkPipelineRasterizationStateCreateInfo rs = make<VkPipelineRasterizationStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms = make<VkPipelineMultisampleStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState blend{};
  blend.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo cb = make<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
  cb.attachmentCount = 1;
  cb.pAttachments = &blend;
  VkGraphicsPipelineCreateInfo gpci = make<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
  gpci.stageCount = 2;
  gpci.pStages = stages;
  gpci.pVertexInputState = &vi;
  gpci.pInputAssemblyState = &ia;
  gpci.pViewportState = &vp;
  gpci.pRasterizationState = &rs;
  gpci.pMultisampleState = &ms;
  gpci.pColorBlendState = &cb;
  gpci.layout = layout;
  gpci.renderPass = render_pass;
  VkPipeline pipeline = VK_NULL_HANDLE;
  BLIT_VK(vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline));
  cleanup.add([d, pipeline] { vkDestroyPipeline(d, pipeline, nullptr); });

  // Record: upload the source, draw the blit, copy the target back.
  VkCommandPoolCreateInfo cpci = make<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
  cpci.queueFamilyIndex = dev.graphics_family();
  VkCommandPool cmd_pool = VK_NULL_HANDLE;
  BLIT_VK(vkCreateCommandPool(d, &cpci, nullptr, &cmd_pool));
  cleanup.add([d, cmd_pool] { vkDestroyCommandPool(d, cmd_pool, nullptr); });
  VkCommandBufferAllocateInfo cbai = make<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
  cbai.commandPool = cmd_pool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  BLIT_VK(vkAllocateCommandBuffers(d, &cbai, &cmd));
  VkCommandBufferBeginInfo begin = make<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
  BLIT_VK(vkBeginCommandBuffer(cmd, &begin));

  auto barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                     VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = make<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
  };
  barrier(source.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {kSize, kSize, 1};
  vkCmdCopyBufferToImage(cmd, staging.buffer, source.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  barrier(source.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

  VkClearValue clear{};
  VkRenderPassBeginInfo rpbi = make<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
  rpbi.renderPass = render_pass;
  rpbi.framebuffer = framebuffer;
  rpbi.renderArea = scissor;
  rpbi.clearValueCount = 1;
  rpbi.pClearValues = &clear;
  vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRenderPass(cmd);

  VkMemoryBarrier host_barrier = make<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
  vkCmdCopyImageToBuffer(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
  host_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_barrier, 0,
                       nullptr, 0, nullptr);
  BLIT_VK(vkEndCommandBuffer(cmd));

  VkFenceCreateInfo fci = make<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
  VkFence fence = VK_NULL_HANDLE;
  BLIT_VK(vkCreateFence(d, &fci, nullptr, &fence));
  cleanup.add([d, fence] { vkDestroyFence(d, fence, nullptr); });
  VkSubmitInfo si = make<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  BLIT_VK(vkQueueSubmit(dev.graphics_queue(), 1, &si, fence));
  BLIT_VK(vkWaitForFences(d, 1, &fence, VK_TRUE, UINT64_MAX));
  BLIT_VK(vkDeviceWaitIdle(d));
  BLIT_VK(vmaInvalidateAllocation(dev.allocator(), readback.alloc, 0, VK_WHOLE_SIZE));

  std::vector<uint32_t> out(kSize * kSize);
  std::memcpy(out.data(), readback.mapped, image_bytes);
  CHECK(out == src_pixels);
}
