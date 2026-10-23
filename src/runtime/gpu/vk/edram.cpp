// SPDX-License-Identifier: MIT
#include "runtime/gpu/vk/edram.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common/log.h"
#include "lr_gpu_shaders.h"
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
  return Error{ErrorCode::Internal, std::string("EDRAM: ") + what + " failed (VkResult " + std::to_string(r) + ")", ""};
}

uint32_t bits(uint32_t v, uint32_t lo, uint32_t count) { return (v >> lo) & ((1u << count) - 1u); }

// Sign-extended bit field.
int32_t sbits(uint32_t v, uint32_t lo, uint32_t count) {
  const uint32_t u = bits(v, lo, count);
  const uint32_t sign = 1u << (count - 1);
  return int32_t(u ^ sign) - int32_t(sign);
}

constexpr uint32_t kMaxResolveSize = (1u << 14) - edram::kResolveAlignment;  // 14-bit size fields, 8-aligned
// The largest guest range one resolve may touch (a 64bpp 2048x2048 surface is 32 MiB); larger ones are cut there.
constexpr uint64_t kMaxResolveBytes = uint64_t{64} << 20;
constexpr uint64_t kPhysicalSpace = uint64_t{pm4::kPhysicalMask} + 1;

// Destination packings, the kPack* constants of resolve_common.glsl.
enum Pack : uint32_t {
  kPack8888 = 0,
  kPack2101010 = 1,
  kPack16x2Float = 2,
  kPack16x4Float = 3,
  kPack32Float = 4,
  kPack32x2Float = 5,
  kPackDepth = 6,
};

// Must match the push constant block of resolve_common.glsl.
struct ResolvePush {
  uint32_t rect[4];
  uint32_t src_size[2];
  uint32_t dest_pitch;
  uint32_t pack;
  uint32_t endian;
  uint32_t swap_rb;
  int32_t exp_bias;
  uint32_t sample_count;
  uint32_t samples;
  uint32_t dst_lo;
  uint32_t dst_bytes;
  uint32_t depth_format;
};
static_assert(sizeof(ResolvePush) == 64);

std::optional<Pack> dest_pack(uint32_t dest_format) {
  switch (dest_format) {
    case edram::kDest_8_8_8_8:
    case edram::kDest_8_8_8_8_A:
    case edram::kDest_8_8_8_8_AS_16_16_16_16:
      return kPack8888;
    case edram::kDest_2_10_10_10:
    case edram::kDest_2_10_10_10_AS_16_16_16_16:
      return kPack2101010;
    case edram::kDest_16_16_FLOAT:
      return kPack16x2Float;
    case edram::kDest_16_16_16_16_FLOAT:
      return kPack16x4Float;
    case edram::kDest_32_FLOAT:
      return kPack32Float;
    case edram::kDest_32_32_FLOAT:
      return kPack32x2Float;
    default:
      return std::nullopt;
  }
}

VkFormat color_vk_format(uint32_t format) {
  switch (format) {
    case edram::kColor_8_8_8_8:
    case edram::kColor_8_8_8_8_GAMMA:  // the gamma-encoded values are stored as they are
      return VK_FORMAT_R8G8B8A8_UNORM;
    case edram::kColor_2_10_10_10:
    case edram::kColor_2_10_10_10_AS_10_10_10_10:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;  // R in bits 0-9, like the EDRAM word
    case edram::kColor_16_16_FLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case edram::kColor_16_16_16_16_FLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case edram::kColor_32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case edram::kColor_32_32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    default:
      return VK_FORMAT_UNDEFINED;
  }
}

float half_to_float(uint32_t h) {
  const float sign = (h & 0x8000u) ? -1.0f : 1.0f;
  const uint32_t e = bits(h, 10, 5), m = bits(h, 0, 10);
  if (e == 0) return sign * std::ldexp(float(m), -24);
  if (e == 31) return m ? std::numeric_limits<float>::quiet_NaN() : sign * std::numeric_limits<float>::infinity();
  return sign * std::ldexp(float(1024u + m), int(e) - 25);
}

// Xenos 20e4 depth (4-bit exponent biased by 15, 20-bit mantissa, denormals at exponent 0) as a number.
double from_20e4(uint32_t f) {
  const uint32_t e = bits(f, 20, 4), m = bits(f, 0, 20);
  return e == 0 ? std::ldexp(double(m), -34) : std::ldexp(1.0 + double(m) / 1048576.0, int(e) - 15);
}

VkClearValue clear_value(const RenderTargetKey& key, uint32_t lo, uint32_t hi) {
  VkClearValue v{};
  if (key.depth) {
    const uint32_t d24 = lo >> 8;
    // D24FS8 keeps [0, 2) in host [0, 1).
    v.depthStencil.depth = key.format == edram::kDepth_D24S8 ? float(double(d24) / 16777215.0) : float(from_20e4(d24) * 0.5);
    v.depthStencil.stencil = lo & 0xFFu;
    return v;
  }
  float* c = v.color.float32;
  switch (key.format) {
    case edram::kColor_8_8_8_8:
    case edram::kColor_8_8_8_8_GAMMA:
      for (uint32_t j = 0; j < 4; ++j) c[j] = float(bits(lo, 8 * j, 8)) / 255.0f;
      break;
    case edram::kColor_2_10_10_10:
    case edram::kColor_2_10_10_10_AS_10_10_10_10:
      for (uint32_t j = 0; j < 3; ++j) c[j] = float(bits(lo, 10 * j, 10)) / 1023.0f;
      c[3] = float(bits(lo, 30, 2)) / 3.0f;
      break;
    case edram::kColor_16_16_FLOAT:
      c[0] = half_to_float(lo & 0xFFFFu);
      c[1] = half_to_float(lo >> 16);
      break;
    case edram::kColor_16_16_16_16_FLOAT:
      c[0] = half_to_float(lo & 0xFFFFu);
      c[1] = half_to_float(lo >> 16);
      c[2] = half_to_float(hi & 0xFFFFu);
      c[3] = half_to_float(hi >> 16);
      break;
    case edram::kColor_32_FLOAT:
      c[0] = std::bit_cast<float>(lo);
      break;
    case edram::kColor_32_32_FLOAT:
      c[0] = std::bit_cast<float>(lo);
      c[1] = std::bit_cast<float>(hi);
      break;
    default:
      break;
  }
  return v;
}

// The guest samples RB_COPY_CONTROL.copy_sample_select names, made valid for the MSAA mode (Xenia draw_util's
// sanitising, reimplemented): 1x reads sample 0; 2x maps 2/3 to 0/1 and the pairs past 01 to 01; 4x caps at 0123.
// Depth is never averaged: a group reads its first sample.
std::vector<uint32_t> guest_samples(uint32_t select, uint32_t msaa, bool depth) {
  if (msaa == edram::kMsaa1x) return {0};
  if (msaa == edram::kMsaa2x) {
    if (select == edram::kSample2) select = edram::kSample0;
    if (select == edram::kSample3) select = edram::kSample1;
    if (select > edram::kSamples01) select = edram::kSamples01;
  } else if (select > edram::kSamples0123) {
    select = edram::kSamples0123;
  }
  if (depth) {
    if (select == edram::kSamples01 || select == edram::kSamples0123) select = edram::kSample0;
    if (select == edram::kSamples23) select = edram::kSample2;
  }
  switch (select) {
    case edram::kSamples01:
      return {0, 1};
    case edram::kSamples23:
      return {2, 3};
    case edram::kSamples0123:
      return {0, 1, 2, 3};
    default:
      return {select};
  }
}

// Bounding box of the three vertices of vertex fetch constant 0, in 1/256 pixels (top-left rule applied later).
std::optional<std::array<int32_t, 4>> vertex_rect(const RegisterFile& regs, const MemoryAccess* mem) {
  if (!mem || !mem->base || !mem->to_guest) return std::nullopt;
  const uint32_t f0 = regs.read(kShaderConstantFetchBase), f1 = regs.read(kShaderConstantFetchBase + 1);
  if (bits(f0, 0, 2) != 3 || bits(f1, 2, 24) != 6) return std::nullopt;  // type 3 = vertex; 6 dwords
  const std::optional<uint32_t> guest = mem->to_guest((f0 & ~3u) & pm4::kPhysicalMask);
  if (!guest || uint64_t{*guest} + 24 > mem->size) return std::nullopt;
  const float half_pixel = bits(regs.read(PA_SU_VTX_CNTL), 0, 1) == 0 ? 0.5f : 0.0f;  // pix_center 0: D3D
  std::array<int32_t, 6> fixed{};
  for (uint32_t i = 0; i < 6; ++i) {
    uint32_t raw = 0;
    std::memcpy(&raw, mem->base + *guest + 4 * i, 4);
    const float f = std::bit_cast<float>(pm4::gpu_swap(raw, bits(f1, 0, 2))) + half_pixel;
    if (!std::isfinite(f) || std::fabs(f) > 65536.0f) return std::nullopt;
    fixed[i] = int32_t(std::lrint(double(f) * 256.0));
  }
  return std::array<int32_t, 4>{std::min({fixed[0], fixed[2], fixed[4]}), std::min({fixed[1], fixed[3], fixed[5]}),
                                std::max({fixed[0], fixed[2], fixed[4]}), std::max({fixed[1], fixed[3], fixed[5]})};
}

}  // namespace

bool is_64bpp(const RenderTargetKey& key) {
  return !key.depth && (key.format == 5 /* k_16_16_16_16 */ || key.format == edram::kColor_16_16_16_16_FLOAT ||
                        key.format == edram::kColor_32_32_FLOAT);
}

uint32_t pitch_tiles(const RenderTargetKey& key) {
  const uint32_t samples_x = key.msaa >= edram::kMsaa4x ? 2 : 1;
  const uint32_t tiles = (key.pitch * samples_x + edram::kTileWidthSamples - 1) / edram::kTileWidthSamples;
  return is_64bpp(key) ? tiles * 2 : tiles;
}

uint32_t surface_height(const RenderTargetKey& key) {
  const uint32_t pt = pitch_tiles(key);
  if (pt == 0) return 0;
  const uint32_t rows = std::max(edram::kTileCount / pt, 1u);
  const uint32_t samples_y = key.msaa >= edram::kMsaa2x ? 2 : 1;
  return std::min(rows * edram::kTileHeightSamples / samples_y, edram::kMaxHeight);
}

std::optional<RenderTargetKey> render_target_key(const RegisterFile& regs, uint32_t slot) {
  if (slot > edram::kDepthSlot) return std::nullopt;
  const uint32_t surface = regs.read(RB_SURFACE_INFO);
  RenderTargetKey key;
  key.pitch = bits(surface, 0, 14);
  key.msaa = bits(surface, 16, 2);
  if (key.pitch == 0 || key.msaa > edram::kMsaa4x) return std::nullopt;
  static constexpr uint32_t kColorInfo[4] = {RB_COLOR_INFO, RB_COLOR1_INFO, RB_COLOR2_INFO, RB_COLOR3_INFO};
  key.depth = slot == edram::kDepthSlot;
  const uint32_t info = regs.read(key.depth ? uint32_t{RB_DEPTH_INFO} : kColorInfo[slot]);
  key.base_tiles = bits(info, 0, 11);
  key.format = key.depth ? bits(info, 16, 1) : bits(info, 16, 4);
  return key;
}

ResolveRect resolve_rect(const RegisterFile& regs, const MemoryAccess* mem) {
  const uint32_t tl = regs.read(PA_SC_WINDOW_SCISSOR_TL), br = regs.read(PA_SC_WINDOW_SCISSOR_BR);
  const uint32_t offset = regs.read(PA_SC_WINDOW_OFFSET);
  const int32_t off_x = sbits(offset, 0, 15), off_y = sbits(offset, 16, 15);
  int32_t sx0 = int32_t(bits(tl, 0, 14)), sy0 = int32_t(bits(tl, 16, 14));
  int32_t sx1 = int32_t(bits(br, 0, 14)), sy1 = int32_t(bits(br, 16, 14));
  if (bits(tl, 31, 1) == 0) {  // window_offset_disable clear: the scissor moves with the window
    sx0 += off_x;
    sx1 += off_x;
    sy0 += off_y;
    sy1 += off_y;
  }
  sx0 = std::max(sx0, 0);
  sy0 = std::max(sy0, 0);
  sx1 = std::max(sx1, sx0);
  sy1 = std::max(sy1, sy0);
  int32_t x0 = sx0, y0 = sy0, x1 = sx1, y1 = sy1;
  if (const auto v = vertex_rect(regs, mem)) {
    // Top-left rule at 1/256 pixel: a pixel is covered when its centre is inside, an edge through it counts on the
    // top/left only.
    x0 = ((*v)[0] + 127) >> 8;
    y0 = ((*v)[1] + 127) >> 8;
    x1 = ((*v)[2] + 127) >> 8;
    y1 = ((*v)[3] + 127) >> 8;
    if (bits(regs.read(PA_SU_SC_MODE_CNTL), 16, 1)) {  // vtx_window_offset_enable
      x0 += off_x;
      x1 += off_x;
      y0 += off_y;
      y1 += off_y;
    }
    x0 = std::clamp(x0, sx0, sx1);
    x1 = std::clamp(x1, sx0, sx1);
    y0 = std::clamp(y0, sy0, sy1);
    y1 = std::clamp(y1, sy0, sy1);
  }
  const int32_t a = int32_t(edram::kResolveAlignment);
  x0 &= ~(a - 1);
  y0 &= ~(a - 1);
  x1 = (x1 + a - 1) & ~(a - 1);
  y1 = (y1 + a - 1) & ~(a - 1);
  const int32_t pitch = int32_t(bits(regs.read(RB_SURFACE_INFO), 0, 14)) & ~(a - 1);
  x0 = std::min(x0, pitch);
  x1 = std::min(x1, pitch);
  y1 = std::min(y1, y0 + int32_t(kMaxResolveSize));
  if (x1 < x0) x1 = x0;
  if (y1 < y0) y1 = y0;
  return ResolveRect{uint32_t(x0), uint32_t(y0), uint32_t(x1), uint32_t(y1)};
}

// Xenos sample order (Xenia spirv_shader_translator_rb.cc, reference only): 2x is (top, bottom); 4x is top-left,
// bottom-left, top-right, bottom-right. Vulkan's standard 2x pattern is (bottom-right, top-left), so a native 2x
// host image swaps the two; its standard 4x pattern is (top, right, left, bottom) = TL, TR, BL, BR in quadrant
// terms, so guest 4x sample s is host sample {0, 2, 1, 3}[s]. Guest 2x on a 4x host image: guest sample s covers
// the host samples of its half of the pixel, 2s and 2s+1 (0, 1 lie in the top half, 2, 3 in the bottom).
uint32_t RenderTarget::host_sample(uint32_t guest_sample) const {
  guest_sample &= 3u;
  if (key.msaa == edram::kMsaa2x) {
    if (samples == VK_SAMPLE_COUNT_4_BIT) return (guest_sample & 1u) * 2;
    return 1u - (guest_sample & 1u);
  }
  if (key.msaa == edram::kMsaa4x) {
    static constexpr uint32_t kXenos4xToVulkan[4] = {0, 2, 1, 3};
    return kXenos4xToVulkan[guest_sample];
  }
  return 0;
}

uint32_t RenderTarget::host_sample_mask(uint32_t guest_mask) const {
  const uint32_t guest_samples = key.msaa == edram::kMsaa4x ? 4 : key.msaa == edram::kMsaa2x ? 2 : 1;
  uint32_t mask = 0;
  for (uint32_t s = 0; s < guest_samples; ++s) {
    if (!(guest_mask & (1u << s))) continue;
    const uint32_t h = host_sample(s);
    mask |= 1u << h;
    if (key.msaa == edram::kMsaa2x && samples == VK_SAMPLE_COUNT_4_BIT) mask |= 1u << (h + 1);
  }
  return mask;
}

struct Edram::Impl {
  const Device& device;
  const std::atomic<bool>* stop;
  std::unique_ptr<CommandStream> own_stream;  // tests: no shared stream, flushed after every operation
  CommandStream* stream = nullptr;
  VkCommandBuffer cmd = VK_NULL_HANDLE;  // the stream's, while an operation records
  VkSampler sampler = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  // resolve_color, resolve_color_ms, resolve_depth, resolve_depth_ms
  std::array<VkPipeline, 4> pipelines{};

  // Host-visible mirror of the guest range a resolve writes.
  VkBuffer staging = VK_NULL_HANDLE;
  VmaAllocation staging_alloc = nullptr;
  void* staging_map = nullptr;
  VkDeviceSize staging_size = 0;

  std::vector<std::unique_ptr<RenderTarget>> targets;
  uint64_t uses = 0;
  std::set<std::string> warned;
  std::function<void(uint32_t phys, uint32_t size)> on_write;
  std::function<void(const RenderTarget&)> on_evict;

  Impl(const Device& d, const std::atomic<bool>* s) : device(d), stop(s) {}
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  ~Impl() {
    const VkDevice d = device.device();
    if (stream) stream->drain();  // nothing recorded may still use a target
    if (d) vkDeviceWaitIdle(d);
    for (auto& t : targets) destroy_target(*t);
    if (staging) vmaDestroyBuffer(device.allocator(), staging, staging_alloc);
    for (VkPipeline p : pipelines)
      if (p) vkDestroyPipeline(d, p, nullptr);
    if (descriptor_pool) vkDestroyDescriptorPool(d, descriptor_pool, nullptr);
    if (pipeline_layout) vkDestroyPipelineLayout(d, pipeline_layout, nullptr);
    if (set_layout) vkDestroyDescriptorSetLayout(d, set_layout, nullptr);
    if (sampler) vkDestroySampler(d, sampler, nullptr);
    own_stream.reset();
  }

  void warn_once(const std::string& msg) {
    if (warned.insert(msg).second) log::warn("[gpu] EDRAM: " + msg);
  }

  Result<void> init() {
    const VkDevice d = device.device();

    VkSamplerCreateInfo sci = make<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (VkResult r = vkCreateSampler(d, &sci, nullptr, &sampler); r != VK_SUCCESS)
      return vk_error("vkCreateSampler", r);

    // binding 0: colour or depth source; 1: stencil source (depth only); 2: the guest-range staging buffer.
    std::array<VkDescriptorSetLayoutBinding, 3> b{};
    for (uint32_t i = 0; i < 3; ++i) {
      b[i].binding = i;
      b[i].descriptorType = i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      b[i].descriptorCount = 1;
      b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dslci =
        make<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    dslci.bindingCount = 3;
    dslci.pBindings = b.data();
    if (VkResult r = vkCreateDescriptorSetLayout(d, &dslci, nullptr, &set_layout); r != VK_SUCCESS)
      return vk_error("vkCreateDescriptorSetLayout", r);
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ResolvePush)};
    VkPipelineLayoutCreateInfo plci = make<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &set_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    if (VkResult r = vkCreatePipelineLayout(d, &plci, nullptr, &pipeline_layout); r != VK_SUCCESS)
      return vk_error("vkCreatePipelineLayout", r);
    std::array<VkDescriptorPoolSize, 2> sizes{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
                                               {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}}};
    VkDescriptorPoolCreateInfo dpci = make<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    dpci.maxSets = 1;
    dpci.poolSizeCount = uint32_t(sizes.size());
    dpci.pPoolSizes = sizes.data();
    if (VkResult r = vkCreateDescriptorPool(d, &dpci, nullptr, &descriptor_pool); r != VK_SUCCESS)
      return vk_error("vkCreateDescriptorPool", r);

    static constexpr const char* kNames[4] = {"resolve_color.comp", "resolve_color_ms.comp", "resolve_depth.comp",
                                              "resolve_depth_ms.comp"};
    for (uint32_t i = 0; i < 4; ++i) {
      const shaders::BuiltinShader* s = nullptr;
      for (std::size_t j = 0; j < shaders::kBuiltinShaderCount; ++j)
        if (std::strcmp(shaders::kBuiltinShaders[j].name, kNames[i]) == 0) s = &shaders::kBuiltinShaders[j];
      if (!s) return Error{ErrorCode::Internal, std::string("EDRAM: built-in shader ") + kNames[i] + " is missing", ""};
      VkShaderModuleCreateInfo smci = make<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
      smci.codeSize = s->word_count * sizeof(uint32_t);
      smci.pCode = s->words;
      VkShaderModule module = VK_NULL_HANDLE;
      if (VkResult r = vkCreateShaderModule(d, &smci, nullptr, &module); r != VK_SUCCESS)
        return vk_error("vkCreateShaderModule", r);
      VkComputePipelineCreateInfo cpi = make<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
      cpi.stage = make<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
      cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      cpi.stage.module = module;
      cpi.stage.pName = "main";
      cpi.layout = pipeline_layout;
      const VkResult r = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipelines[i]);
      vkDestroyShaderModule(d, module, nullptr);
      if (r != VK_SUCCESS) return vk_error("vkCreateComputePipelines", r);
    }
    return {};
  }

  // --- Command recording: into the stream (the frame's command buffer, or the Edram's own) ---

  Result<void> begin() {
    LR_TRY_ASSIGN(c, stream->begin());
    cmd = c;
    full_barrier();
    return {};
  }

  // Everything before (other submissions, attachment writes, shader reads) happens-before everything after.
  void full_barrier() {
    VkMemoryBarrier mb = make<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
  }

  // The end of an operation that does not need its result on the CPU: submitted now only on the Edram's own stream.
  Result<void> done() {
    if (own_stream) return stream->flush();
    return {};
  }

  // --- Render targets ---

  bool supports(VkFormat format, VkImageUsageFlags usage, VkSampleCountFlagBits samples) const {
    VkImageFormatProperties props{};
    if (vkGetPhysicalDeviceImageFormatProperties(device.physical_device(), format, VK_IMAGE_TYPE_2D,
                                                 VK_IMAGE_TILING_OPTIMAL, usage, 0, &props) != VK_SUCCESS)
      return false;
    return (props.sampleCounts & samples) != 0;
  }

  void destroy_target(RenderTarget& t) {
    const VkDevice d = device.device();
    if (t.framebuffer) vkDestroyFramebuffer(d, t.framebuffer, nullptr);
    if (t.render_pass) vkDestroyRenderPass(d, t.render_pass, nullptr);
    if (t.stencil_view) vkDestroyImageView(d, t.stencil_view, nullptr);
    if (t.sample_view) vkDestroyImageView(d, t.sample_view, nullptr);
    if (t.attachment_view) vkDestroyImageView(d, t.attachment_view, nullptr);
    if (t.image) vmaDestroyImage(device.allocator(), t.image, t.alloc);
    t = RenderTarget{};
  }

  Result<std::unique_ptr<RenderTarget>> create_target(const RenderTargetKey& key) {
    const VkDevice d = device.device();
    const VkImageUsageFlags usage =
        (key.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // Guest 2x uses a host 2x image when the device has one, else 4x (lavapipe offers 1, 4 and 8).
    std::vector<VkSampleCountFlagBits> sample_choices;
    if (key.msaa == edram::kMsaa1x) sample_choices = {VK_SAMPLE_COUNT_1_BIT};
    if (key.msaa == edram::kMsaa2x) sample_choices = {VK_SAMPLE_COUNT_2_BIT, VK_SAMPLE_COUNT_4_BIT};
    if (key.msaa == edram::kMsaa4x) sample_choices = {VK_SAMPLE_COUNT_4_BIT};
    std::vector<VkFormat> formats;
    if (!key.depth) {
      const VkFormat f = color_vk_format(key.format);
      if (f == VK_FORMAT_UNDEFINED)
        return Error{ErrorCode::InvalidFormat,
                     "EDRAM colour format " + std::to_string(key.format) + " has no host render target format", ""};
      formats = {f};
    } else if (key.format == edram::kDepth_D24S8) {
      formats = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};
    } else {
      formats = {VK_FORMAT_D32_SFLOAT_S8_UINT};  // 20e4 needs a float host depth
    }
    auto t = std::make_unique<RenderTarget>();
    for (VkFormat f : formats) {
      for (VkSampleCountFlagBits s : sample_choices)
        if (supports(f, usage, s)) {
          t->format = f;
          t->samples = s;
          break;
        }
      if (t->format != VK_FORMAT_UNDEFINED) break;
    }
    if (t->format == VK_FORMAT_UNDEFINED)
      return Error{ErrorCode::InvalidFormat,
                   std::string("EDRAM: the Vulkan device has no ") + (key.depth ? "depth" : "colour") + " format " +
                       std::to_string(key.format) + " render target with " +
                       std::to_string(1u << key.msaa) + "x MSAA",
                   ""};
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device.physical_device(), &props);
    t->key = key;
    t->width = std::min(key.pitch, props.limits.maxImageDimension2D);
    t->height = std::min(surface_height(key), props.limits.maxImageDimension2D);

    VkImageCreateInfo ici = make<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = t->format;
    ici.extent = {t->width, t->height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = t->samples;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (VkResult r = vmaCreateImage(device.allocator(), &ici, &aci, &t->image, &t->alloc, nullptr); r != VK_SUCCESS)
      return vk_error("vmaCreateImage (render target)", r);
    struct Guard {  // destroys the half-built target on an early return
      Impl* self;
      RenderTarget* t;
      ~Guard() {
        if (t) self->destroy_target(*t);
      }
    } guard{this, t.get()};

    const VkImageAspectFlags all =
        key.depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
    auto view = [&](VkImageAspectFlags aspect, VkImageView* out) -> Result<void> {
      VkImageViewCreateInfo vci = make<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
      vci.image = t->image;
      vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vci.format = t->format;
      vci.subresourceRange = {aspect, 0, 1, 0, 1};
      if (VkResult r = vkCreateImageView(d, &vci, nullptr, out); r != VK_SUCCESS) return vk_error("vkCreateImageView", r);
      return {};
    };
    LR_TRY(view(all, &t->attachment_view));
    LR_TRY(view(key.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, &t->sample_view));
    if (key.depth) LR_TRY(view(VK_IMAGE_ASPECT_STENCIL_BIT, &t->stencil_view));

    VkAttachmentDescription att{};
    att.format = t->format;
    att.samples = t->samples;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = key.depth ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = key.depth ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
    att.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (key.depth) {
      sub.pDepthStencilAttachment = &ref;
    } else {
      sub.colorAttachmentCount = 1;
      sub.pColorAttachments = &ref;
    }
    const VkPipelineStageFlags attachment_stages =
        key.depth ? VkPipelineStageFlags(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                         VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)
                  : VkPipelineStageFlags(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    const VkAccessFlags attachment_access =
        key.depth ? VkAccessFlags(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
                  : VkAccessFlags(VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    // Before: any earlier write (clears, other passes, transfers); after: any later access (the resolve's reads).
    std::array<VkSubpassDependency, 2> deps{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    deps[0].dstStageMask = attachment_stages;
    deps[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    deps[0].dstAccessMask = attachment_access;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = attachment_stages;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    deps[1].srcAccessMask = attachment_access;
    deps[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    VkRenderPassCreateInfo rpci = make<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = uint32_t(deps.size());
    rpci.pDependencies = deps.data();
    if (VkResult r = vkCreateRenderPass(d, &rpci, nullptr, &t->render_pass); r != VK_SUCCESS)
      return vk_error("vkCreateRenderPass", r);
    VkFramebufferCreateInfo fbci = make<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
    fbci.renderPass = t->render_pass;
    fbci.attachmentCount = 1;
    fbci.pAttachments = &t->attachment_view;
    fbci.width = t->width;
    fbci.height = t->height;
    fbci.layers = 1;
    if (VkResult r = vkCreateFramebuffer(d, &fbci, nullptr, &t->framebuffer); r != VK_SUCCESS)
      return vk_error("vkCreateFramebuffer", r);

    // UNDEFINED -> GENERAL, zero-filled: a surface the guest never cleared reads as zeros, not garbage.
    LR_TRY(begin());
    VkImageMemoryBarrier ib = make<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    ib.srcAccessMask = 0;
    ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = t->image;
    ib.subresourceRange = {all, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &ib);
    if (key.depth) {
      VkClearDepthStencilValue zero{0.0f, 0};
      vkCmdClearDepthStencilImage(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &ib.subresourceRange);
    } else {
      VkClearColorValue zero{};
      vkCmdClearColorImage(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &ib.subresourceRange);
    }
    LR_TRY(done());
    guard.t = nullptr;
    return t;
  }

  Result<RenderTarget*> bind(const RegisterFile& regs, uint32_t slot) {
    const std::optional<RenderTargetKey> key = render_target_key(regs, slot);
    if (!key)
      return Error{ErrorCode::InvalidFormat,
                   "EDRAM: render target slot " + std::to_string(slot) +
                       " has no valid surface (RB_SURFACE_INFO pitch 0, reserved MSAA mode or bad slot)",
                   ""};
    for (auto& t : targets)
      if (t->key == *key) {
        t->last_use = ++uses;
        return t.get();
      }
    if (targets.size() >= edram::kMaxRenderTargets) {
      // Recorded commands may still use the evicted image: its destruction waits for the next completed flush.
      auto lru = std::min_element(targets.begin(), targets.end(),
                                  [](const auto& a, const auto& b) { return a->last_use < b->last_use; });
      if (on_evict) on_evict(**lru);
      std::shared_ptr<RenderTarget> victim(std::move(*lru));
      targets.erase(lru);
      stream->defer([this, victim] { destroy_target(*victim); });
    }
    LR_TRY_ASSIGN(t, create_target(*key));
    t->last_use = ++uses;
    targets.push_back(std::move(t));
    return targets.back().get();
  }

  // --- Clears ---

  void record_clear(const RenderTarget& t, const ResolveRect& rect, const VkClearValue& value) {
    const uint32_t x1 = std::min(rect.x1, t.width), y1 = std::min(rect.y1, t.height);
    if (rect.x0 >= x1 || rect.y0 >= y1) return;
    VkRenderPassBeginInfo rpbi = make<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
    rpbi.renderPass = t.render_pass;
    rpbi.framebuffer = t.framebuffer;
    rpbi.renderArea = {{0, 0}, {t.width, t.height}};
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    VkClearAttachment ca{};
    ca.aspectMask = t.key.depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT);
    ca.colorAttachment = 0;
    ca.clearValue = value;
    VkClearRect cr{};
    cr.rect = {{int32_t(rect.x0), int32_t(rect.y0)}, {x1 - rect.x0, y1 - rect.y0}};
    cr.baseArrayLayer = 0;
    cr.layerCount = 1;
    vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
    vkCmdEndRenderPass(cmd);
  }

  Result<void> clear(const RegisterFile& regs, const ResolveRect& rect) {
    const uint32_t control = regs.read(RB_COPY_CONTROL);
    const uint32_t src = bits(control, 0, 3);
    const bool color = bits(control, 8, 1) && src < edram::kDepthSlot;  // a depth copy cannot clear colour
    const bool depth = bits(control, 9, 1) != 0;
    if ((!color && !depth) || rect.empty()) return {};
    RenderTarget* color_rt = nullptr;
    RenderTarget* depth_rt = nullptr;
    if (color) {
      LR_TRY_ASSIGN(t, bind(regs, src));
      color_rt = t;
    }
    if (depth) {
      LR_TRY_ASSIGN(t, bind(regs, edram::kDepthSlot));
      depth_rt = t;
    }
    LR_TRY(begin());
    if (color_rt)
      record_clear(*color_rt, rect, clear_value(color_rt->key, regs.read(RB_COLOR_CLEAR), regs.read(RB_COLOR_CLEAR_LO)));
    if (depth_rt) record_clear(*depth_rt, rect, clear_value(depth_rt->key, regs.read(RB_DEPTH_CLEAR), 0));
    return done();
  }

  // --- Resolves ---

  Result<void> ensure_staging(VkDeviceSize bytes) {
    if (staging_size >= bytes) return {};
    if (staging) vmaDestroyBuffer(device.allocator(), staging, staging_alloc);
    staging = VK_NULL_HANDLE;
    staging_size = 0;
    VkBufferCreateInfo bci = make<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (VkResult r = vmaCreateBuffer(device.allocator(), &bci, &aci, &staging, &staging_alloc, &info); r != VK_SUCCESS)
      return vk_error("vmaCreateBuffer (resolve staging)", r);
    staging_map = info.pMappedData;
    staging_size = bytes;
    return {};
  }

  Result<void> copy(const RegisterFile& regs, const MemoryAccess& mem, const ResolveRect& rect) {
    const uint32_t control = regs.read(RB_COPY_CONTROL);
    const uint32_t src = bits(control, 0, 3);
    const bool depth = src >= edram::kDepthSlot;  // copy_src_select 4 (and the unused 5-7) is depth
    LR_TRY_ASSIGN(rt, bind(regs, depth ? edram::kDepthSlot : src));
    const ResolveRect r{rect.x0, rect.y0, std::min(rect.x1, rt->width), std::min(rect.y1, rt->height)};
    if (r.empty()) return {};

    const uint32_t info = regs.read(RB_COPY_DEST_INFO);
    const uint32_t endian = bits(info, 0, 3);
    if (bits(info, 3, 1)) {
      warn_once("resolve to a 3D/array destination (copy_dest_array) is not supported; copy skipped");
      return {};
    }
    if (endian > 4) {
      warn_once("resolve endian mode " + std::to_string(endian) + " is not supported; copy skipped");
      return {};
    }
    Pack pack = kPackDepth;
    if (!depth) {
      const std::optional<Pack> p = dest_pack(bits(info, 7, 6));
      if (!p) {
        warn_once("resolve destination format " + std::to_string(bits(info, 7, 6)) + " is not supported; copy skipped");
        return {};
      }
      pack = *p;
    }
    const uint32_t b = (pack == kPack16x4Float || pack == kPack32x2Float) ? 3 : 2;
    if (endian == 4 && b == 2)  // 8in64 has no 64-bit unit to reverse in a 32bpp pixel
      warn_once("resolve endian 8in64 on a 32bpp destination is treated as 8in32");
    const uint32_t dest_pitch = (bits(regs.read(RB_COPY_DEST_PITCH), 0, 14) + 31u) & ~31u;
    if (dest_pitch == 0) return {};

    // The guest bytes the copy may touch: whole 32x32 macro tiles, from the first to the last one in the rectangle
    // (each is a contiguous 2^(10 + b)-byte range, in row-major order).
    const uint64_t macro_pitch = dest_pitch / 32;
    const uint64_t lo = ((r.y0 / 32) * macro_pitch + r.x0 / 32) << (10 + b);
    const uint64_t hi = (((r.y1 - 1) / 32) * macro_pitch + (r.x1 - 1) / 32 + 1) << (10 + b);
    const uint32_t dest_base = regs.read(RB_COPY_DEST_BASE);
    if (dest_base & 3u) {
      warn_once("resolve destination base is not 4-byte aligned; copy skipped");
      return {};
    }
    const uint64_t phys_lo = uint64_t{dest_base & pm4::kPhysicalMask} + lo;
    const std::optional<uint32_t> guest =
        phys_lo < kPhysicalSpace && mem.to_guest ? mem.to_guest(uint32_t(phys_lo)) : std::nullopt;
    if (!guest || *guest >= mem.size || !mem.base) {
      warn_once("resolve destination is outside guest memory; copy dropped");
      return {};
    }
    uint64_t len = hi - lo;
    uint64_t room = std::min({mem.size - *guest, kPhysicalSpace - phys_lo, kMaxResolveBytes});
    if (mem.allocation_end) {
      // Never past the physical allocation holding the first byte: neighbouring allocations and unallocated
      // (uncommitted) pages stay untouched.
      const std::optional<uint32_t> end = mem.allocation_end(uint32_t(phys_lo));
      if (!end || *end <= phys_lo) {
        warn_once("resolve destination is not in a physical allocation; copy dropped");
        return {};
      }
      room = std::min(room, uint64_t{*end} - phys_lo);
    }
    if (len > room) {
      warn_once("resolve destination runs past its allocation or guest memory (or the per-resolve limit); the rest "
                "is dropped");
      len = room & ~uint64_t{3};
    }
    if (len == 0) return {};
    // The range must map linearly (the physical heap's guest view does); otherwise drop it rather than guess.
    const std::optional<uint32_t> guest_end = mem.to_guest(uint32_t(phys_lo + len - 1));
    if (!guest_end || uint64_t{*guest_end} != *guest + len - 1) {
      warn_once("resolve destination does not map to one guest range; copy dropped");
      return {};
    }

    LR_TRY(ensure_staging(len));
    uint8_t* guest_bytes = mem.base + *guest;
    std::memcpy(staging_map, guest_bytes, len);  // bytes the copy does not write keep their guest value
    if (VkResult vr = vmaFlushAllocation(device.allocator(), staging_alloc, 0, len); vr != VK_SUCCESS)
      return vk_error("vmaFlushAllocation", vr);

    vkResetDescriptorPool(device.device(), descriptor_pool, 0);
    VkDescriptorSetAllocateInfo dsai = make<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    dsai.descriptorPool = descriptor_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (VkResult vr = vkAllocateDescriptorSets(device.device(), &dsai, &set); vr != VK_SUCCESS)
      return vk_error("vkAllocateDescriptorSets", vr);
    VkDescriptorImageInfo source{sampler, rt->sample_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo stencil{sampler, rt->stencil_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo dest{staging, 0, len};
    std::array<VkWriteDescriptorSet, 3> writes{};
    uint32_t write_count = 0;
    auto add = [&](uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo* image,
                   const VkDescriptorBufferInfo* buffer) {
      VkWriteDescriptorSet& w = writes[write_count++];
      w = make<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
      w.dstSet = set;
      w.dstBinding = binding;
      w.descriptorCount = 1;
      w.descriptorType = type;
      w.pImageInfo = image;
      w.pBufferInfo = buffer;
    };
    add(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &source, nullptr);
    if (depth) add(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &stencil, nullptr);
    add(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &dest);
    vkUpdateDescriptorSets(device.device(), write_count, writes.data(), 0, nullptr);

    ResolvePush push{};
    push.rect[0] = r.x0;
    push.rect[1] = r.y0;
    push.rect[2] = r.x1 - r.x0;
    push.rect[3] = r.y1 - r.y0;
    push.src_size[0] = rt->width;
    push.src_size[1] = rt->height;
    push.dest_pitch = dest_pitch;
    push.pack = pack;
    push.endian = endian;
    push.swap_rb = depth ? 0 : bits(info, 24, 1);
    push.exp_bias = depth ? 0 : sbits(info, 16, 6);
    const std::vector<uint32_t> guest_list = guest_samples(bits(control, 4, 3), rt->key.msaa, depth);
    push.sample_count = uint32_t(guest_list.size());
    for (std::size_t i = 0; i < guest_list.size(); ++i) push.samples |= rt->host_sample(guest_list[i]) << (8 * i);
    push.dst_lo = uint32_t(lo);
    push.dst_bytes = uint32_t(len);
    push.depth_format = depth ? rt->key.format : 0;

    const uint32_t pipeline = (depth ? 2 : 0) + (rt->samples != VK_SAMPLE_COUNT_1_BIT ? 1 : 0);
    LR_TRY(begin());
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[pipeline]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (push.rect[2] + 7) / 8, (push.rect[3] + 7) / 8, 1);
    VkMemoryBarrier mb = make<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr,
                         0, nullptr);
    LR_TRY(stream->flush());  // the guest bytes are needed now
    if (VkResult vr = vmaInvalidateAllocation(device.allocator(), staging_alloc, 0, len); vr != VK_SUCCESS)
      return vk_error("vmaInvalidateAllocation", vr);
    std::memcpy(guest_bytes, staging_map, len);
    if (on_write) on_write(uint32_t(phys_lo), uint32_t(len));
    return {};
  }

  Result<void> resolve(const RegisterFile& regs, const MemoryAccess& mem) {
    const ResolveRect rect = resolve_rect(regs, &mem);
    const uint32_t command = bits(regs.read(RB_COPY_CONTROL), 20, 2);
    if (command == 0 || command == 1) {
      if (!rect.empty()) LR_TRY(copy(regs, mem, rect));
    } else if (command == 2) {
      warn_once("resolve copy command 2 (constant one) is not supported; nothing copied");
    }
    return clear(regs, rect);
  }
};

Result<std::unique_ptr<Edram>> Edram::create(const Device& device, const std::atomic<bool>* stop,
                                             CommandStream* stream) {
  auto impl = std::make_unique<Impl>(device, stop);
  if (stream) {
    impl->stream = stream;
  } else {
    LR_TRY_ASSIGN(own, CommandStream::create(device, stop));
    impl->own_stream = std::move(own);
    impl->stream = impl->own_stream.get();
  }
  LR_TRY(impl->init());
  return std::unique_ptr<Edram>(new Edram(std::move(impl)));
}

Edram::Edram(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Edram::~Edram() = default;

Result<RenderTarget*> Edram::bind(const RegisterFile& regs, uint32_t slot) { return impl_->bind(regs, slot); }

Result<void> Edram::clear(const RegisterFile& regs, const MemoryAccess* mem) {
  return impl_->clear(regs, resolve_rect(regs, mem));
}

Result<void> Edram::resolve(const RegisterFile& regs, const MemoryAccess& mem) { return impl_->resolve(regs, mem); }

std::size_t Edram::render_target_count() const { return impl_->targets.size(); }

void Edram::set_write_observer(std::function<void(uint32_t phys, uint32_t size)> observer) {
  impl_->on_write = std::move(observer);
}

void Edram::set_evict_observer(std::function<void(const RenderTarget& target)> observer) {
  impl_->on_evict = std::move(observer);
}

}  // namespace lr::gpu::vk
