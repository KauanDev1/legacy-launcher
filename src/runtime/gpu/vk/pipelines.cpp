// SPDX-License-Identifier: MIT
#include "runtime/gpu/vk/pipelines.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "lr_gpu_shaders.h"
#include "runtime/gpu/shader/contract.h"
#include "runtime/gpu/shader/translator.h"

namespace lr::gpu::vk {
namespace {

template <class T>
T make(VkStructureType type) {
  T s{};
  s.sType = type;
  return s;
}

Error vk_error(const char* what, VkResult r) {
  return Error{ErrorCode::Internal, std::string("pipelines: ") + what + " failed (VkResult " + std::to_string(r) + ")",
               ""};
}

Result<VkShaderModule> create_module(VkDevice device, const uint32_t* words, std::size_t count) {
  VkShaderModuleCreateInfo smci = make<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
  smci.codeSize = count * sizeof(uint32_t);
  smci.pCode = words;
  VkShaderModule module = VK_NULL_HANDLE;
  if (VkResult r = vkCreateShaderModule(device, &smci, nullptr, &module); r != VK_SUCCESS)
    return vk_error("vkCreateShaderModule", r);
  return module;
}

}  // namespace

uint32_t RenderPassKey::color_reference_count() const {
  uint32_t n = 0;
  for (uint32_t i = 0; i < kColorSlots; ++i)
    if (color[i] != VK_FORMAT_UNDEFINED) n = i + 1;
  return n;
}

struct PipelineCache::Impl {
  const Device& device;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkShaderModule rect_gs = VK_NULL_HANDLE;
  std::map<shader::ShaderKey, VkShaderModule> modules;
  std::map<RenderPassKey, VkRenderPass> passes;
  std::map<PipelineKey, VkPipeline> pipelines;
  bool last_fragment = false;

  explicit Impl(const Device& d) : device(d) {}
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  ~Impl() {
    const VkDevice d = device.device();
    if (!d) return;
    vkDeviceWaitIdle(d);
    for (auto& [k, p] : pipelines) vkDestroyPipeline(d, p, nullptr);
    for (auto& [k, rp] : passes) vkDestroyRenderPass(d, rp, nullptr);
    for (auto& [k, m] : modules) vkDestroyShaderModule(d, m, nullptr);
    if (rect_gs) vkDestroyShaderModule(d, rect_gs, nullptr);
    if (layout) vkDestroyPipelineLayout(d, layout, nullptr);
  }

  Result<void> init(const std::array<VkDescriptorSetLayout, 4>& sets) {
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                              shader::kPushConstantBytes};
    VkPipelineLayoutCreateInfo plci = make<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    plci.setLayoutCount = uint32_t(sets.size());
    plci.pSetLayouts = sets.data();
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    if (VkResult r = vkCreatePipelineLayout(device.device(), &plci, nullptr, &layout); r != VK_SUCCESS)
      return vk_error("vkCreatePipelineLayout", r);
    if (device.geometry_shader()) {
      const shaders::BuiltinShader* gs = nullptr;
      for (std::size_t i = 0; i < shaders::kBuiltinShaderCount; ++i)
        if (std::strcmp(shaders::kBuiltinShaders[i].name, "rect_list.geom") == 0) gs = &shaders::kBuiltinShaders[i];
      if (!gs) return Error{ErrorCode::Internal, "pipelines: built-in shader rect_list.geom is missing", ""};
      LR_TRY_ASSIGN(m, create_module(device.device(), gs->words, gs->word_count));
      rect_gs = m;
    }
    return {};
  }

  Result<VkRenderPass> render_pass(const RenderPassKey& key) {
    if (auto it = passes.find(key); it != passes.end()) return it->second;
    std::vector<VkAttachmentDescription> atts;
    std::array<VkAttachmentReference, kColorSlots> refs{};
    const uint32_t ref_count = key.color_reference_count();
    for (uint32_t i = 0; i < kColorSlots; ++i) {
      refs[i] = {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL};
      if (key.color[i] == VK_FORMAT_UNDEFINED) continue;
      VkAttachmentDescription a{};
      a.format = key.color[i];
      a.samples = key.samples;
      a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
      refs[i].attachment = uint32_t(atts.size());
      atts.push_back(a);
    }
    VkAttachmentReference depth_ref{VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL};
    if (key.depth != VK_FORMAT_UNDEFINED) {
      VkAttachmentDescription a{};
      a.format = key.depth;
      a.samples = key.samples;
      a.loadOp = a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      a.storeOp = a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
      a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
      depth_ref.attachment = uint32_t(atts.size());
      atts.push_back(a);
    }
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = ref_count;
    sub.pColorAttachments = ref_count ? refs.data() : nullptr;
    sub.pDepthStencilAttachment = key.depth != VK_FORMAT_UNDEFINED ? &depth_ref : nullptr;
    // Before: any earlier write (clears, resolves' barriers, other passes, uploads); after: any later access.
    const VkPipelineStageFlags stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkAccessFlags access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    std::array<VkSubpassDependency, 2> deps{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    deps[0].dstStageMask = stages;
    deps[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    deps[0].dstAccessMask = access;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = stages;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    deps[1].srcAccessMask = access;
    deps[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    VkRenderPassCreateInfo rpci = make<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
    rpci.attachmentCount = uint32_t(atts.size());
    rpci.pAttachments = atts.data();
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = uint32_t(deps.size());
    rpci.pDependencies = deps.data();
    VkRenderPass rp = VK_NULL_HANDLE;
    if (VkResult r = vkCreateRenderPass(device.device(), &rpci, nullptr, &rp); r != VK_SUCCESS)
      return vk_error("vkCreateRenderPass", r);
    passes.emplace(key, rp);
    return rp;
  }

  Result<VkPipeline> get(const PipelineKey& key) {
    if (auto it = pipelines.find(key); it != pipelines.end()) {
      last_fragment = key.has_ps;
      return it->second;
    }
    auto vs = modules.find(key.vs);
    if (vs == modules.end()) return Error{ErrorCode::Internal, "pipelines: vertex shader module was not added", ""};
    VkShaderModule ps_module = VK_NULL_HANDLE;
    if (key.has_ps) {
      auto ps = modules.find(key.ps);
      if (ps == modules.end()) return Error{ErrorCode::Internal, "pipelines: pixel shader module was not added", ""};
      ps_module = ps->second;
    }
    if (key.rect_list && !rect_gs)
      return Error{ErrorCode::Internal, "pipelines: rectangle lists need the geometryShader feature", ""};
    LR_TRY_ASSIGN(rp, render_pass(key.pass));

    std::array<VkPipelineShaderStageCreateInfo, 3> stages{};
    uint32_t stage_count = 0;
    auto add_stage = [&](VkShaderStageFlagBits stage, VkShaderModule module, const VkSpecializationInfo* spec) {
      VkPipelineShaderStageCreateInfo& s = stages[stage_count++];
      s = make<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
      s.stage = stage;
      s.module = module;
      s.pName = "main";
      s.pSpecializationInfo = spec;
    };
    const VkSpecializationMapEntry entry{shader::kSpecAlphaTest, 0, sizeof(uint32_t)};
    VkSpecializationInfo spec{1, &entry, sizeof(uint32_t), &key.alpha_test};
    add_stage(VK_SHADER_STAGE_VERTEX_BIT, vs->second, nullptr);
    if (key.rect_list) add_stage(VK_SHADER_STAGE_GEOMETRY_BIT, rect_gs, nullptr);
    if (key.has_ps) add_stage(VK_SHADER_STAGE_FRAGMENT_BIT, ps_module, &spec);

    VkPipelineVertexInputStateCreateInfo vi =
        make<VkPipelineVertexInputStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
    VkPipelineInputAssemblyStateCreateInfo ia =
        make<VkPipelineInputAssemblyStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
    ia.topology = VkPrimitiveTopology(key.topology);
    ia.primitiveRestartEnable = key.primitive_restart ? VK_TRUE : VK_FALSE;
    VkPipelineViewportStateCreateInfo vp =
        make<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs =
        make<VkPipelineRasterizationStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
    rs.polygonMode = VkPolygonMode(key.polygon_mode);
    rs.cullMode = key.cull_mode;
    rs.frontFace = VkFrontFace(key.front_face);
    rs.depthBiasEnable = key.depth_bias ? VK_TRUE : VK_FALSE;
    rs.lineWidth = 1.0f;
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pv =
        make<VkPipelineRasterizationProvokingVertexStateCreateInfoEXT>(
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT);
    pv.provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
    if (key.provoking_last) rs.pNext = &pv;
    const VkSampleMask sample_mask = key.sample_mask;
    VkPipelineMultisampleStateCreateInfo ms =
        make<VkPipelineMultisampleStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
    ms.rasterizationSamples = key.pass.samples;
    ms.pSampleMask = &sample_mask;
    VkPipelineDepthStencilStateCreateInfo ds =
        make<VkPipelineDepthStencilStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
    ds.depthTestEnable = key.depth_test ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = key.depth_write ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = VkCompareOp(key.depth_compare);
    ds.stencilTestEnable = key.stencil_test ? VK_TRUE : VK_FALSE;
    auto stencil = [](const StencilKey& s) {
      VkStencilOpState o{};
      o.failOp = VkStencilOp(s.fail);
      o.passOp = VkStencilOp(s.pass);
      o.depthFailOp = VkStencilOp(s.depth_fail);
      o.compareOp = VkCompareOp(s.compare);
      return o;  // masks and reference are dynamic
    };
    ds.front = stencil(key.stencil_front);
    ds.back = stencil(key.stencil_back);
    ds.maxDepthBounds = 1.0f;
    std::array<VkPipelineColorBlendAttachmentState, kColorSlots> blend{};
    const uint32_t color_count = key.pass.color_reference_count();
    for (uint32_t i = 0; i < color_count; ++i) {
      // Without independentBlend every attachment must be identical: target 0's state for all (the draw path
      // already made them equal; see draw.cpp).
      const BlendKey& b = key.blend[i];
      VkPipelineColorBlendAttachmentState& a = blend[i];
      a.blendEnable = b.enable ? VK_TRUE : VK_FALSE;
      a.srcColorBlendFactor = VkBlendFactor(b.src_color);
      a.dstColorBlendFactor = VkBlendFactor(b.dst_color);
      a.colorBlendOp = VkBlendOp(b.color_op);
      a.srcAlphaBlendFactor = VkBlendFactor(b.src_alpha);
      a.dstAlphaBlendFactor = VkBlendFactor(b.dst_alpha);
      a.alphaBlendOp = VkBlendOp(b.alpha_op);
      a.colorWriteMask = b.write_mask;
    }
    VkPipelineColorBlendStateCreateInfo cb =
        make<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
    cb.attachmentCount = color_count;
    cb.pAttachments = color_count ? blend.data() : nullptr;
    const std::array<VkDynamicState, 7> dynamic{VK_DYNAMIC_STATE_VIEWPORT,           VK_DYNAMIC_STATE_SCISSOR,
                                                VK_DYNAMIC_STATE_DEPTH_BIAS,         VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                                VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                                                VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                                VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dyn =
        make<VkPipelineDynamicStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
    dyn.dynamicStateCount = uint32_t(dynamic.size());
    dyn.pDynamicStates = dynamic.data();

    VkGraphicsPipelineCreateInfo gpci = make<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
    gpci.stageCount = stage_count;
    gpci.pStages = stages.data();
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = key.pass.depth != VK_FORMAT_UNDEFINED ? &ds : nullptr;
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &dyn;
    gpci.layout = layout;
    gpci.renderPass = rp;
    gpci.subpass = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (VkResult r = vkCreateGraphicsPipelines(device.device(), VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline);
        r != VK_SUCCESS)
      return vk_error("vkCreateGraphicsPipelines", r);
    pipelines.emplace(key, pipeline);
    last_fragment = key.has_ps;
    return pipeline;
  }
};

Result<std::unique_ptr<PipelineCache>> PipelineCache::create(const Device& device,
                                                             const std::array<VkDescriptorSetLayout, 4>& set_layouts) {
  auto impl = std::make_unique<Impl>(device);
  LR_TRY(impl->init(set_layouts));
  return std::unique_ptr<PipelineCache>(new PipelineCache(std::move(impl)));
}

PipelineCache::PipelineCache(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PipelineCache::~PipelineCache() = default;

Result<void> PipelineCache::add_shader(const shader::ShaderKey& key, const shader::TranslatedShader& shader) {
  if (impl_->modules.count(key)) return {};
  LR_TRY_ASSIGN(m, create_module(impl_->device.device(), shader.spirv.data(), shader.spirv.size()));
  impl_->modules.emplace(key, m);
  return {};
}

Result<VkPipeline> PipelineCache::get(const PipelineKey& key) { return impl_->get(key); }
Result<VkRenderPass> PipelineCache::render_pass(const RenderPassKey& key) { return impl_->render_pass(key); }
VkPipelineLayout PipelineCache::layout() const { return impl_->layout; }
std::size_t PipelineCache::pipeline_count() const { return impl_->pipelines.size(); }
std::size_t PipelineCache::module_count() const { return impl_->modules.size(); }
bool PipelineCache::last_has_fragment_stage() const { return impl_->last_fragment; }

}  // namespace lr::gpu::vk
