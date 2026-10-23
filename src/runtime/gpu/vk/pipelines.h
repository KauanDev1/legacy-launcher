// SPDX-License-Identifier: MIT
// The graphics pipelines of the draw path (Task 20, plan B). A pipeline is built from the two translated shaders
// (runtime/gpu/shader/translator.h; one VkShaderModule per ShaderKey) and the render state the Xenos registers
// select, all of it in PipelineKey. There is no vertex input state: the translated vertex shader fetches its
// vertices itself through the vertex fetch table (contract.h). The layout is the contract's: four bindless sets
// (Texture2D, Texture3D, TextureCube, samplers) and 32 bytes of push constants for the vertex and fragment stages.
// Viewport, scissor, depth bias, blend constants and the stencil reference and masks are dynamic state.
//
// Render passes have one subpass whose colour attachment i is EDRAM colour slot i (VK_ATTACHMENT_UNUSED for a slot
// the draw does not bind, so pixel shader output location i always lands on slot i) and an optional depth/stencil
// attachment; every attachment loads, stores and stays in VK_IMAGE_LAYOUT_GENERAL, like the EDRAM's own passes.
//
// Rectangle lists draw as triangle lists through the built-in geometry shader rect_list.geom, which emits each
// rectangle as a two-triangle strip with the fourth corner (position and the 16 interpolators) computed as
// v_a + v_b - v_corner from the vertex opposite the longest edge (ADR 0005, Task 20).
//
// Thread-safety: GPU thread only.
#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "common/result.h"
#include "runtime/gpu/shader/shader_key.h"
#include "runtime/gpu/vk/device.h"

namespace lr::gpu::shader {
struct TranslatedShader;
}

namespace lr::gpu::vk {

inline constexpr uint32_t kColorSlots = 4;

// The attachments of a draw render pass: the host format of colour slots 0..3 (VK_FORMAT_UNDEFINED: not bound), of
// the depth/stencil target (VK_FORMAT_UNDEFINED: none) and the host sample count of all of them.
struct RenderPassKey {
  std::array<VkFormat, kColorSlots> color{VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED,
                                          VK_FORMAT_UNDEFINED};
  VkFormat depth = VK_FORMAT_UNDEFINED;
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  auto operator<=>(const RenderPassKey&) const = default;
  // Colour attachment references in the subpass: the highest bound slot + 1 (0 without colour).
  uint32_t color_reference_count() const;
};

// One colour target's blend state, as Vulkan enums (stored as uint32_t so the key compares as plain values).
struct BlendKey {
  uint32_t enable = 0;
  uint32_t src_color = 0, dst_color = 0, color_op = 0;  // VkBlendFactor, VkBlendFactor, VkBlendOp
  uint32_t src_alpha = 0, dst_alpha = 0, alpha_op = 0;
  uint32_t write_mask = 0;  // VkColorComponentFlags
  auto operator<=>(const BlendKey&) const = default;
};

struct StencilKey {
  uint32_t fail = 0, pass = 0, depth_fail = 0, compare = 0;  // VkStencilOp x3, VkCompareOp
  auto operator<=>(const StencilKey&) const = default;
};

struct PipelineKey {
  shader::ShaderKey vs{ShaderStage::Vertex, 0, 0, 0};
  shader::ShaderKey ps{ShaderStage::Pixel, 0, 0, 0};
  bool has_ps = false;      // false: no fragment stage (depth-only draw, or no pixel shader)
  uint32_t alpha_test = 0;  // specialization constant 0 (contract.h kAlphaTestEnable | func << kAlphaTestFuncShift)
  RenderPassKey pass;
  uint32_t sample_mask = 1;  // host samples written (RenderTarget::host_sample_mask)
  uint32_t topology = 0;     // VkPrimitiveTopology
  bool primitive_restart = false;
  bool rect_list = false;    // the geometry stage expands rectangles
  uint32_t cull_mode = 0;    // VkCullModeFlags
  uint32_t front_face = 0;   // VkFrontFace
  uint32_t polygon_mode = 0;  // VkPolygonMode
  bool depth_bias = false;
  bool provoking_last = false;  // VK_EXT_provoking_vertex LAST_VERTEX (only when the device has it)
  bool depth_test = false, depth_write = false;
  uint32_t depth_compare = 0;  // VkCompareOp
  bool stencil_test = false;
  StencilKey stencil_front, stencil_back;
  std::array<BlendKey, kColorSlots> blend{};
  auto operator<=>(const PipelineKey&) const = default;
};

class PipelineCache {
 public:
  // `set_layouts`: the contract's sets 0..3 (TextureCache::set_layout(0..2), SamplerCache::set_layout()).
  static Result<std::unique_ptr<PipelineCache>> create(const Device& device,
                                                       const std::array<VkDescriptorSetLayout, 4>& set_layouts);
  ~PipelineCache();
  PipelineCache(const PipelineCache&) = delete;
  PipelineCache& operator=(const PipelineCache&) = delete;

  // Creates the VkShaderModule of a translated shader once per ShaderKey (later calls with the same key do nothing).
  Result<void> add_shader(const shader::ShaderKey& key, const shader::TranslatedShader& shader);
  // The pipeline for `key`, created on first use. Its shaders must have been added. Internal on a Vulkan failure.
  Result<VkPipeline> get(const PipelineKey& key);
  // The render pass of a set of attachments, created on first use (pipelines and framebuffers share it).
  Result<VkRenderPass> render_pass(const RenderPassKey& key);

  VkPipelineLayout layout() const;
  std::size_t pipeline_count() const;
  std::size_t module_count() const;
  // Whether the pipeline last returned by get() has a fragment stage (tests: a depth-only draw has none).
  bool last_has_fragment_stage() const;

 private:
  struct Impl;
  explicit PipelineCache(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace lr::gpu::vk
