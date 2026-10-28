// SPDX-License-Identifier: MIT
// EDRAM emulation (Task 18): host render targets, clears and resolves into guest memory in the Xenos tiled layout.
// The expected guest bytes come from the CPU tiling reference (support/xenos_tiling.h), an implementation
// independent of the resolve compute shader.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "common/log.h"
#include "lr_gpu_shaders.h"
#include "runtime/gpu/pm4.h"
#include "runtime/gpu/registers.h"
#include "runtime/gpu/vk/edram.h"
#include "support/vk_shader_harness.h"
#include "support/xenos_tiling.h"

using lr::gpu::MemoryAccess;
using lr::gpu::RegisterFile;
using lr::gpu::vk::Edram;
using lr::gpu::vk::RenderTarget;
using lr::gpu::vk::RenderTargetKey;
using lr::gpu::vk::ResolveRect;
namespace edram = lr::gpu::vk::edram;
namespace reg = lr::gpu;

namespace {

constexpr uint32_t kGuestSize = 1u << 20;
constexpr uint8_t kSentinel = 0xEE;

template <class T>
T make(VkStructureType type) {
  T s{};
  s.sType = type;
  return s;
}

#define EDRAM_VK(expr) REQUIRE((expr) == VK_SUCCESS)

// Collects the Vulkan validation-layer errors (Device logs them with log::error) for the scope.
class ValidationErrors {
 public:
  ValidationErrors() {
    previous_ = lr::log::set_sink([this](lr::log::Level level, std::string_view msg) {
      if (level == lr::log::Level::Error) errors.emplace_back(msg);
      std::fprintf(stderr, "%.*s\n", static_cast<int>(msg.size()), msg.data());
    });
  }
  ~ValidationErrors() { lr::log::set_sink(std::move(previous_)); }
  std::vector<std::string> errors;

 private:
  lr::log::Sink previous_;
};

// Guest memory where physical address == guest address, size bytes reachable; `bytes` may be longer (a canary).
struct Guest {
  explicit Guest(uint32_t reachable, uint32_t extra = 0) : bytes(reachable + extra, kSentinel), size(reachable) {}
  MemoryAccess access() {
    MemoryAccess m;
    m.base = bytes.data();
    const uint64_t n = size;
    m.to_guest = [n](uint32_t phys) -> std::optional<uint32_t> {
      if (phys >= n) return std::nullopt;
      return phys;
    };
    m.size = size;
    return m;
  }
  std::vector<uint8_t> bytes;
  uint32_t size;
};

uint32_t scissor_tl(uint32_t x, uint32_t y) { return x | (y << 16) | (1u << 31); }  // window_offset_disable
uint32_t scissor_br(uint32_t x, uint32_t y) { return x | (y << 16); }

void set_scissor(RegisterFile& regs, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
  regs.write(reg::PA_SC_WINDOW_SCISSOR_TL, scissor_tl(x0, y0));
  regs.write(reg::PA_SC_WINDOW_SCISSOR_BR, scissor_br(x1, y1));
}

void set_surface(RegisterFile& regs, uint32_t pitch, uint32_t msaa) {
  regs.write(reg::RB_SURFACE_INFO, pitch | (msaa << 16));
}

// RB_COPY_CONTROL: source, sample select, clear enables, copy command.
uint32_t copy_control(uint32_t src, uint32_t sample_select, bool color_clear, bool depth_clear, uint32_t command) {
  return src | (sample_select << 4) | (uint32_t(color_clear) << 8) | (uint32_t(depth_clear) << 9) | (command << 20);
}

void set_dest(RegisterFile& regs, uint32_t base, uint32_t pitch, uint32_t height, uint32_t format, uint32_t endian) {
  regs.write(reg::RB_COPY_DEST_BASE, base);
  regs.write(reg::RB_COPY_DEST_PITCH, pitch | (height << 16));
  regs.write(reg::RB_COPY_DEST_INFO, endian | (format << 7));
}

// Little-endian bytes of `word` (how the GPU stores it into guest memory) at guest[offset].
void put_word(std::vector<uint8_t>& g, uint32_t offset, uint32_t word) { std::memcpy(g.data() + offset, &word, 4); }

std::string hex(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08X", v);
  return buf;
}

// The first differing byte of two guest images, for a readable failure.
std::string first_difference(const std::vector<uint8_t>& got, const std::vector<uint8_t>& want) {
  for (size_t i = 0; i < want.size(); ++i)
    if (got[i] != want[i]) {
      char buf[96];
      std::snprintf(buf, sizeof(buf), "byte 0x%zX: got 0x%02X, want 0x%02X", i, got[i], want[i]);
      return buf;
    }
  return "none";
}

struct ColorCase {
  const char* name;
  uint32_t rt_format;
  uint32_t dest_format;
  uint32_t endian;
  uint32_t bpp_log2;
  std::array<uint32_t, 2> a;  // clear value words (RB_COLOR_CLEAR, RB_COLOR_CLEAR_LO)
  std::array<uint32_t, 2> b;
};

// Draws the whole target with one colour into the host samples of guest_mask (blit.vert + clear.frag).
void draw_samples(const lr::gpu::vk::Device& dev, const RenderTarget& rt, uint32_t guest_mask,
                  const std::array<float, 4>& color) {
  const VkDevice d = dev.device();
  auto module = [&](const char* name) {
    for (std::size_t i = 0; i < lr::gpu::vk::shaders::kBuiltinShaderCount; ++i) {
      const auto& s = lr::gpu::vk::shaders::kBuiltinShaders[i];
      if (std::string(s.name) != name) continue;
      VkShaderModuleCreateInfo smci = make<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
      smci.codeSize = s.word_count * 4;
      smci.pCode = s.words;
      VkShaderModule m = VK_NULL_HANDLE;
      EDRAM_VK(vkCreateShaderModule(d, &smci, nullptr, &m));
      return m;
    }
    FAIL("no built-in shader " << name);
    return VkShaderModule{};
  };
  VkShaderModule vs = module("blit.vert");
  VkShaderModule fs = module("clear.frag");
  VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
  VkPipelineLayoutCreateInfo plci = make<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &range;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  EDRAM_VK(vkCreatePipelineLayout(d, &plci, nullptr, &layout));

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs;
  stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fs;
  stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vi =
      make<VkPipelineVertexInputStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
  VkPipelineInputAssemblyStateCreateInfo ia =
      make<VkPipelineInputAssemblyStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkViewport viewport{0.0f, 0.0f, float(rt.width), float(rt.height), 0.0f, 1.0f};
  VkRect2D scissor{{0, 0}, {rt.width, rt.height}};
  VkPipelineViewportStateCreateInfo vp =
      make<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
  vp.viewportCount = vp.scissorCount = 1;
  vp.pViewports = &viewport;
  vp.pScissors = &scissor;
  VkPipelineRasterizationStateCreateInfo rs =
      make<VkPipelineRasterizationStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.lineWidth = 1.0f;
  const VkSampleMask mask = rt.host_sample_mask(guest_mask);
  VkPipelineMultisampleStateCreateInfo ms =
      make<VkPipelineMultisampleStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
  ms.rasterizationSamples = rt.samples;
  ms.pSampleMask = &mask;
  VkPipelineColorBlendAttachmentState blend{};
  blend.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo cb =
      make<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
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
  gpci.renderPass = rt.render_pass;
  VkPipeline pipeline = VK_NULL_HANDLE;
  EDRAM_VK(vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline));

  VkCommandPoolCreateInfo cpci = make<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
  cpci.queueFamilyIndex = dev.graphics_family();
  VkCommandPool pool = VK_NULL_HANDLE;
  EDRAM_VK(vkCreateCommandPool(d, &cpci, nullptr, &pool));
  VkCommandBufferAllocateInfo cbai = make<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
  cbai.commandPool = pool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  EDRAM_VK(vkAllocateCommandBuffers(d, &cbai, &cmd));
  VkCommandBufferBeginInfo begin = make<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
  EDRAM_VK(vkBeginCommandBuffer(cmd, &begin));
  VkRenderPassBeginInfo rpbi = make<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
  rpbi.renderPass = rt.render_pass;
  rpbi.framebuffer = rt.framebuffer;
  rpbi.renderArea = scissor;
  vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, color.data());
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRenderPass(cmd);
  EDRAM_VK(vkEndCommandBuffer(cmd));
  VkSubmitInfo si = make<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  EDRAM_VK(vkQueueSubmit(dev.graphics_queue(), 1, &si, VK_NULL_HANDLE));
  EDRAM_VK(vkQueueWaitIdle(dev.graphics_queue()));
  vkDestroyCommandPool(d, pool, nullptr);
  vkDestroyPipeline(d, pipeline, nullptr);
  vkDestroyPipelineLayout(d, layout, nullptr);
  vkDestroyShaderModule(d, vs, nullptr);
  vkDestroyShaderModule(d, fs, nullptr);
}

// 20e4 (Xenos float depth) -> double, for building test values.
double from_20e4(uint32_t f) {
  const uint32_t e = (f >> 20) & 0xF, m = f & 0xFFFFF;
  return e == 0 ? std::ldexp(double(m), -34) : std::ldexp(1.0 + double(m) / 1048576.0, int(e) - 15);
}

}  // namespace

TEST_CASE("xenos tiling reference places every block once", "[unit][gpu]") {
  using lr::testing::xenos_tiled_offset_2d;
  CHECK(xenos_tiled_offset_2d(0, 0, 64, 2) == 0);
  CHECK(xenos_tiled_offset_2d(1, 0, 64, 2) == 4);
  CHECK(xenos_tiled_offset_2d(0, 1, 64, 2) == 16);  // y[0] lands on bit 4
  CHECK(xenos_tiled_offset_2d(32, 0, 64, 2) == 4096);  // the second 32x32 macro tile
  CHECK(xenos_tiled_offset_2d(0, 32, 64, 2) == 2 * 4096);
  CHECK(xenos_tiled_offset_2d(0, 32, 64, 3) == 2 * 8192);
  // Hand-computed from the bit layout (pitch 64, 4-byte pixels): pipe = {x[4] ^ y[3], x[3]} at bits 6-7, micro
  // row y[3:1] through linear[7:5] at bits 8-10, bank y[4] at bit 11.
  CHECK(xenos_tiled_offset_2d(8, 0, 64, 2) == 64);
  CHECK(xenos_tiled_offset_2d(16, 0, 64, 2) == 128);
  CHECK(xenos_tiled_offset_2d(0, 8, 64, 2) == 1152);
  CHECK(xenos_tiled_offset_2d(8, 8, 64, 2) == 1216);
  CHECK(xenos_tiled_offset_2d(0, 16, 64, 2) == 2048);
  for (uint32_t b : {2u, 3u}) {
    std::set<uint32_t> seen;
    for (uint32_t y = 0; y < 64; ++y)
      for (uint32_t x = 0; x < 64; ++x) {
        const uint32_t off = xenos_tiled_offset_2d(x, y, 64, b);
        CHECK(off % (1u << b) == 0);
        CHECK(off < (64u * 64u) << b);
        // Every 32x32 macro tile is its own contiguous range.
        const uint32_t origin = lr::testing::xenos_tile_origin_2d(x, y, 64, b);
        CHECK(off >= origin);
        CHECK(off < origin + (1024u << b));
        seen.insert(off);
      }
    CHECK(seen.size() == 64u * 64u);
  }
}

TEST_CASE("Xenos MSAA samples map to the Vulkan sample order", "[unit][gpu]") {
  RenderTarget rt;
  SECTION("1x") {
    rt.key.msaa = edram::kMsaa1x;
    rt.samples = VK_SAMPLE_COUNT_1_BIT;
    CHECK(rt.host_sample(0) == 0);
    CHECK(rt.host_sample_mask(0x1) == 0x1);
    CHECK(rt.host_sample_mask(0xF) == 0x1);
  }
  SECTION("native 2x: Xenos (top, bottom), Vulkan (bottom, top)") {
    rt.key.msaa = edram::kMsaa2x;
    rt.samples = VK_SAMPLE_COUNT_2_BIT;
    CHECK(rt.host_sample(0) == 1);
    CHECK(rt.host_sample(1) == 0);
    CHECK(rt.host_sample_mask(0x1) == 0x2);
    CHECK(rt.host_sample_mask(0x2) == 0x1);
    CHECK(rt.host_sample_mask(0x3) == 0x3);
  }
  SECTION("4x: Xenos TL, BL, TR, BR; Vulkan TL, TR, BL, BR") {
    rt.key.msaa = edram::kMsaa4x;
    rt.samples = VK_SAMPLE_COUNT_4_BIT;
    CHECK(rt.host_sample(0) == 0);
    CHECK(rt.host_sample(1) == 2);
    CHECK(rt.host_sample(2) == 1);
    CHECK(rt.host_sample(3) == 3);
    CHECK(rt.host_sample_mask(0x2) == 0x4);
    CHECK(rt.host_sample_mask(0x4) == 0x2);
    CHECK(rt.host_sample_mask(0x9) == 0x9);
    CHECK(rt.host_sample_mask(0x6) == 0x6);
    CHECK(rt.host_sample_mask(0x3) == 0x5);
  }
  SECTION("2x on a 4x host image: top half {0, 1}, bottom half {2, 3}") {
    rt.key.msaa = edram::kMsaa2x;
    rt.samples = VK_SAMPLE_COUNT_4_BIT;
    CHECK(rt.host_sample(0) == 0);
    CHECK(rt.host_sample(1) == 2);
    CHECK(rt.host_sample_mask(0x1) == 0x3);
    CHECK(rt.host_sample_mask(0x2) == 0xC);
    CHECK(rt.host_sample_mask(0x3) == 0xF);
  }
}

TEST_CASE("EDRAM render target keys and sizes follow the RB registers", "[unit][gpu]") {
  RegisterFile regs;
  set_surface(regs, 1280, edram::kMsaa1x);
  regs.write(reg::RB_COLOR_INFO, 0x0123 | (edram::kColor_8_8_8_8 << 16));
  regs.write(reg::RB_COLOR1_INFO, 0x0800 | 0x10 | (edram::kColor_16_16_16_16_FLOAT << 16));  // bit 11 ignored
  regs.write(reg::RB_DEPTH_INFO, 0x0200 | (edram::kDepth_D24FS8 << 16));
  auto k0 = lr::gpu::vk::render_target_key(regs, 0);
  REQUIRE(k0.has_value());
  CHECK(k0->base_tiles == 0x123);
  CHECK(k0->pitch == 1280);
  CHECK(k0->format == edram::kColor_8_8_8_8);
  CHECK_FALSE(k0->depth);
  CHECK(lr::gpu::vk::pitch_tiles(*k0) == 16);
  CHECK(lr::gpu::vk::surface_height(*k0) == 2048);  // 128 rows of 16-sample tiles
  auto k1 = lr::gpu::vk::render_target_key(regs, 1);
  REQUIRE(k1.has_value());
  CHECK(k1->base_tiles == 0x10);
  CHECK(lr::gpu::vk::is_64bpp(*k1));
  CHECK(lr::gpu::vk::pitch_tiles(*k1) == 32);
  CHECK(lr::gpu::vk::surface_height(*k1) == 1024);
  auto kd = lr::gpu::vk::render_target_key(regs, edram::kDepthSlot);
  REQUIRE(kd.has_value());
  CHECK(kd->depth);
  CHECK(kd->base_tiles == 0x200);
  CHECK(kd->format == edram::kDepth_D24FS8);
  set_surface(regs, 1280, edram::kMsaa4x);
  auto k4 = lr::gpu::vk::render_target_key(regs, 0);
  REQUIRE(k4.has_value());
  CHECK(lr::gpu::vk::pitch_tiles(*k4) == 32);         // 2560 samples wide
  CHECK(lr::gpu::vk::surface_height(*k4) == 64 * 8);  // 64 tile rows, 2 samples high per pixel
  CHECK_FALSE(lr::gpu::vk::render_target_key(regs, 5).has_value());
  set_surface(regs, 0, edram::kMsaa1x);
  CHECK_FALSE(lr::gpu::vk::render_target_key(regs, 0).has_value());
  set_surface(regs, 64, 3);
  CHECK_FALSE(lr::gpu::vk::render_target_key(regs, 0).has_value());
}

TEST_CASE("resolve rectangle comes from the vertices, window offset and scissor", "[unit][gpu]") {
  RegisterFile regs;
  set_surface(regs, 1280, edram::kMsaa1x);
  regs.write(reg::PA_SC_WINDOW_SCISSOR_TL, 0);  // offset applies
  regs.write(reg::PA_SC_WINDOW_SCISSOR_BR, scissor_br(1280, 720));
  SECTION("no vertex fetch constant: the scissor") {
    CHECK(lr::gpu::vk::resolve_rect(regs, nullptr) == ResolveRect{0, 0, 1280, 720});
    regs.write(reg::PA_SC_WINDOW_OFFSET, 16u | (8u << 16));
    CHECK(lr::gpu::vk::resolve_rect(regs, nullptr) == ResolveRect{16, 8, 1280, 728});
    regs.write(reg::PA_SC_WINDOW_SCISSOR_TL, 3u | (5u << 16) | (1u << 31));  // offset disabled; 8-aligned outwards
    regs.write(reg::PA_SC_WINDOW_SCISSOR_BR, scissor_br(61, 30));
    CHECK(lr::gpu::vk::resolve_rect(regs, nullptr) == ResolveRect{0, 0, 64, 32});
  }
  SECTION("Direct3D 9 rectangle vertices in vertex fetch constant 0") {
    Guest g(0x10000);
    // Three k_32_32_FLOAT vertices at phys 0x1000, big-endian, with D3D9's -0.5 pixel offset.
    const float v[6] = {63.5f, 31.5f, -0.5f, 31.5f, -0.5f, -0.5f};
    for (int i = 0; i < 6; ++i) {
      const uint32_t be = __builtin_bswap32(std::bit_cast<uint32_t>(v[i]));
      std::memcpy(g.bytes.data() + 0x1000 + 4 * i, &be, 4);
    }
    regs.write(reg::kShaderConstantFetchBase + 0, 0x1000u | 3u);  // type 3 = vertex
    regs.write(reg::kShaderConstantFetchBase + 1, (6u << 2) | lr::gpu::pm4::kEndian8in32);
    MemoryAccess mem = g.access();
    CHECK(lr::gpu::vk::resolve_rect(regs, &mem) == ResolveRect{0, 0, 64, 32});
    // Window offset applied to the vertices when vtx_window_offset_enable, and to the scissor.
    regs.write(reg::PA_SC_WINDOW_OFFSET, 64u);
    regs.write(reg::PA_SU_SC_MODE_CNTL, 1u << 16);
    CHECK(lr::gpu::vk::resolve_rect(regs, &mem) == ResolveRect{64, 0, 128, 32});
    // Clamped to the scissor.
    regs.write(reg::PA_SC_WINDOW_SCISSOR_BR, scissor_br(40, 16));  // + offset: x < 104
    CHECK(lr::gpu::vk::resolve_rect(regs, &mem) == ResolveRect{64, 0, 104, 16});
    // pix_center 1 (OpenGL): no half-pixel correction, the rectangle shrinks by the top-left rule.
    regs.write(reg::PA_SU_SC_MODE_CNTL, 0);
    regs.write(reg::PA_SC_WINDOW_OFFSET, 0);
    regs.write(reg::PA_SC_WINDOW_SCISSOR_BR, scissor_br(1280, 720));
    regs.write(reg::PA_SU_VTX_CNTL, 1);
    CHECK(lr::gpu::vk::resolve_rect(regs, &mem) == ResolveRect{0, 0, 64, 32});  // 8-aligned outwards again
    // A vertex buffer outside guest memory: falls back to the scissor.
    regs.write(reg::kShaderConstantFetchBase + 0, 0xFFF0u | 3u);
    CHECK(lr::gpu::vk::resolve_rect(regs, &mem) == ResolveRect{0, 0, 1280, 720});
  }
  SECTION("clamped to the surface pitch") {
    set_surface(regs, 100, edram::kMsaa1x);
    CHECK(lr::gpu::vk::resolve_rect(regs, nullptr) == ResolveRect{0, 0, 96, 720});
  }
}

TEST_CASE("clear then resolve writes the tiled guest surface", "[unit][vulkan]") {
  auto h = lr::testing::ShaderHarness::create();
  if (!h.ok()) SKIP("no Vulkan ICD installed");
  const ColorCase c = GENERATE(
      ColorCase{"k_8_8_8_8", edram::kColor_8_8_8_8, edram::kDest_8_8_8_8, lr::gpu::pm4::kEndian8in32, 2,
                {0x80FF4020u, 0}, {0x11223344u, 0}},
      ColorCase{"k_2_10_10_10", edram::kColor_2_10_10_10, edram::kDest_2_10_10_10, lr::gpu::pm4::kEndian8in32, 2,
                {0xC00FFC01u, 0}, {0x5555AAAAu, 0}},
      ColorCase{"k_16_16_16_16_FLOAT", edram::kColor_16_16_16_16_FLOAT, edram::kDest_16_16_16_16_FLOAT,
                lr::gpu::pm4::kEndian8in16, 3, {0xC0003C00u, 0x7BFF3555u}, {0x38003400u, 0xBC00B800u}},
      ColorCase{"k_32_FLOAT", edram::kColor_32_FLOAT, edram::kDest_32_FLOAT, lr::gpu::pm4::kEndian8in32, 2,
                {0x3FC00000u, 0}, {0xC1200000u, 0}});
  INFO(c.name);
  ValidationErrors vk_errors;
  {
    auto e = Edram::create(h.value().device());
    REQUIRE(e.ok());
    Edram& edram_ = *e.value();
    constexpr uint32_t kPitch = 64, kDestBase = 0x40000;
    RegisterFile regs;
    set_surface(regs, kPitch, edram::kMsaa1x);
    regs.write(reg::RB_COLOR_INFO, 0x40 | (c.rt_format << 16));
    regs.write(reg::RB_COLOR1_INFO, 0x40 | (c.rt_format << 16));
    Guest g(kGuestSize);
    MemoryAccess mem = g.access();

    // Clear the whole surface to A, then a rectangle to B (clears alone: RB_COPY_CONTROL copy_command 3).
    regs.write(reg::RB_COPY_CONTROL, copy_control(0, 0, true, false, 3));
    regs.write(reg::RB_COLOR_CLEAR, c.a[0]);
    regs.write(reg::RB_COLOR_CLEAR_LO, c.a[1]);
    set_scissor(regs, 0, 0, kPitch, 64);
    REQUIRE(edram_.clear(regs).ok());
    regs.write(reg::RB_COLOR_CLEAR, c.b[0]);
    regs.write(reg::RB_COLOR_CLEAR_LO, c.b[1]);
    set_scissor(regs, 16, 8, 48, 40);
    REQUIRE(edram_.clear(regs).ok());

    // Resolve (8, 8)-(56, 48) without clearing.
    regs.write(reg::RB_COPY_CONTROL, copy_control(0, 0, false, false, 0));
    set_scissor(regs, 8, 8, 56, 48);
    set_dest(regs, kDestBase, kPitch, 64, c.dest_format, c.endian);
    auto r = edram_.resolve(regs, mem);
    INFO((r.ok() ? std::string("ok") : r.error().message));
    REQUIRE(r.ok());
    CHECK(edram_.render_target_count() == 1);

    std::vector<uint8_t> want(kGuestSize, kSentinel);
    for (uint32_t y = 8; y < 48; ++y)
      for (uint32_t x = 8; x < 56; ++x) {
        const bool in_b = x >= 16 && x < 48 && y >= 8 && y < 40;
        const auto& v = in_b ? c.b : c.a;
        const uint32_t off = kDestBase + lr::testing::xenos_tiled_offset_2d(x, y, kPitch, c.bpp_log2);
        put_word(want, off, lr::gpu::pm4::gpu_swap(v[0], c.endian));
        if (c.bpp_log2 == 3) put_word(want, off + 4, lr::gpu::pm4::gpu_swap(v[1], c.endian));
      }
    INFO("first difference: " << first_difference(g.bytes, want));
    CHECK(g.bytes == want);

    // The same surface through the other slot is the same host image (same key).
    REQUIRE(edram_.bind(regs, 1).ok());
    CHECK(edram_.render_target_count() == 1);
  }
  CHECK(vk_errors.errors.empty());
}

TEST_CASE("2x MSAA resolve averages samples", "[unit][vulkan]") {
  auto h = lr::testing::ShaderHarness::create();
  if (!h.ok()) SKIP("no Vulkan ICD installed");
  ValidationErrors vk_errors;
  {
    const auto& dev = h.value().device();
    auto e = Edram::create(dev);
    REQUIRE(e.ok());
    Edram& edram_ = *e.value();
    constexpr uint32_t kPitch = 32, kDestBase = 0x20000;
    RegisterFile regs;
    set_surface(regs, kPitch, edram::kMsaa2x);
    regs.write(reg::RB_COLOR_INFO, 0 | (edram::kColor_8_8_8_8 << 16));
    auto rt = edram_.bind(regs, 0);
    REQUIRE(rt.ok());
    CHECK(rt.value()->width == kPitch);
    CHECK(rt.value()->height == edram::kMaxHeight);  // one EDRAM tile per row: 2048 tile rows, capped
    // Guest sample 0 and 1 get different colours.
    draw_samples(dev, *rt.value(), 0x1, {0x20 / 255.0f, 0x40 / 255.0f, 0x60 / 255.0f, 0x80 / 255.0f});
    draw_samples(dev, *rt.value(), 0x2, {0x60 / 255.0f, 0x80 / 255.0f, 0xA0 / 255.0f, 0xC0 / 255.0f});

    Guest g(kGuestSize);
    MemoryAccess mem = g.access();
    set_scissor(regs, 0, 0, kPitch, 16);
    set_dest(regs, kDestBase, kPitch, 16, edram::kDest_8_8_8_8, lr::gpu::pm4::kEndian8in32);
    auto expect = [&](uint32_t word) {
      std::vector<uint8_t> want(kGuestSize, kSentinel);
      for (uint32_t y = 0; y < 16; ++y)
        for (uint32_t x = 0; x < kPitch; ++x)
          put_word(want, kDestBase + lr::testing::xenos_tiled_offset_2d(x, y, kPitch, 2), __builtin_bswap32(word));
      return want;
    };
    SECTION("samples 0 and 1 averaged") {
      regs.write(reg::RB_COPY_CONTROL, copy_control(0, edram::kSamples01, false, false, 0));
      REQUIRE(edram_.resolve(regs, mem).ok());
      const auto want = expect(0xA0806040u);  // A B G R from the top: R 0x40, G 0x60, B 0x80, A 0xA0
      INFO("first difference: " << first_difference(g.bytes, want));
      CHECK(g.bytes == want);
    }
    SECTION("sample 1 alone") {
      regs.write(reg::RB_COPY_CONTROL, copy_control(0, edram::kSample1, false, false, 0));
      REQUIRE(edram_.resolve(regs, mem).ok());
      const auto want = expect(0xC0A08060u);
      INFO("first difference: " << first_difference(g.bytes, want));
      CHECK(g.bytes == want);
    }
    SECTION("sample 0 alone") {
      regs.write(reg::RB_COPY_CONTROL, copy_control(0, edram::kSample0, false, false, 0));
      REQUIRE(edram_.resolve(regs, mem).ok());
      const auto want = expect(0x80604020u);
      INFO("first difference: " << first_difference(g.bytes, want));
      CHECK(g.bytes == want);
    }
  }
  CHECK(vk_errors.errors.empty());
}

TEST_CASE("depth resolve format", "[unit][vulkan]") {
  auto h = lr::testing::ShaderHarness::create();
  if (!h.ok()) SKIP("no Vulkan ICD installed");
  struct DepthCase {
    uint32_t format;
    uint32_t msaa;
    uint32_t clear;  // RB_DEPTH_CLEAR: depth << 8 | stencil
  };
  const DepthCase c = GENERATE(DepthCase{edram::kDepth_D24S8, edram::kMsaa1x, 0x12345678u},
                               DepthCase{edram::kDepth_D24S8, edram::kMsaa4x, 0xFFFFFF01u},
                               DepthCase{edram::kDepth_D24FS8, edram::kMsaa1x, 0x3ABCDE5Au},
                               DepthCase{edram::kDepth_D24FS8, edram::kMsaa2x, 0x0001235Au},  // a 20e4 denormal
                               DepthCase{edram::kDepth_D24FS8, edram::kMsaa1x, 0xF00000C3u});
  INFO("format " << c.format << " msaa " << c.msaa << " clear " << hex(c.clear) << " (20e4 value "
                 << from_20e4(c.clear >> 8) << ")");
  ValidationErrors vk_errors;
  {
    auto e = Edram::create(h.value().device());
    REQUIRE(e.ok());
    Edram& edram_ = *e.value();
    constexpr uint32_t kPitch = 48, kDestBase = 0x10000;
    RegisterFile regs;
    set_surface(regs, kPitch, c.msaa);
    regs.write(reg::RB_DEPTH_INFO, 0x100 | (c.format << 16));
    set_scissor(regs, 0, 0, kPitch, 32);
    regs.write(reg::RB_DEPTH_CLEAR, c.clear);
    regs.write(reg::RB_COPY_CONTROL, copy_control(edram::kDepthSlot, 0, false, true, 3));
    REQUIRE(edram_.clear(regs).ok());

    Guest g(kGuestSize);
    MemoryAccess mem = g.access();
    // Direct3D 9 names k_8_8_8_8 as the destination of a depth resolve; the depth word is copied as is.
    set_dest(regs, kDestBase, kPitch, 32, edram::kDest_8_8_8_8, lr::gpu::pm4::kEndian8in32);
    regs.write(reg::RB_COPY_CONTROL, copy_control(edram::kDepthSlot, edram::kSamples0123, false, false, 0));
    auto r = edram_.resolve(regs, mem);
    INFO((r.ok() ? std::string("ok") : r.error().message));
    REQUIRE(r.ok());
    std::vector<uint8_t> want(kGuestSize, kSentinel);
    for (uint32_t y = 0; y < 32; ++y)
      for (uint32_t x = 0; x < kPitch; ++x)
        put_word(want, kDestBase + lr::testing::xenos_tiled_offset_2d(x, y, 64, 2), __builtin_bswap32(c.clear));
    INFO("first difference: " << first_difference(g.bytes, want));
    CHECK(g.bytes == want);
  }
  CHECK(vk_errors.errors.empty());
}

TEST_CASE("resolve never writes outside guest memory", "[unit][vulkan]") {
  auto h = lr::testing::ShaderHarness::create();
  if (!h.ok()) SKIP("no Vulkan ICD installed");
  ValidationErrors vk_errors;
  {
    auto e = Edram::create(h.value().device());
    REQUIRE(e.ok());
    Edram& edram_ = *e.value();
    constexpr uint32_t kPitch = 64, kReachable = 0x8000, kCanary = 0x10000;
    RegisterFile regs;
    set_surface(regs, kPitch, edram::kMsaa1x);
    regs.write(reg::RB_COLOR_INFO, edram::kColor_8_8_8_8 << 16);
    set_scissor(regs, 0, 0, kPitch, 64);
    regs.write(reg::RB_COLOR_CLEAR, 0x01020304u);
    Guest g(kReachable, kCanary);  // bytes past kReachable are not guest memory
    MemoryAccess mem = g.access();
    SECTION("destination running past the end") {
      // 64x64 32bpp = 4 macro tiles of 4 KiB from 0x6000: only the first two fit.
      set_dest(regs, 0x6000, kPitch, 64, edram::kDest_8_8_8_8, lr::gpu::pm4::kEndianNone);
      regs.write(reg::RB_COPY_CONTROL, copy_control(0, 0, true, false, 0));
      REQUIRE(edram_.clear(regs).ok());
      REQUIRE(edram_.resolve(regs, mem).ok());
      std::vector<uint8_t> want(kReachable + kCanary, kSentinel);
      for (uint32_t y = 0; y < 64; ++y)
        for (uint32_t x = 0; x < kPitch; ++x) {
          const uint32_t off = 0x6000 + lr::testing::xenos_tiled_offset_2d(x, y, kPitch, 2);
          if (off + 4 <= kReachable) put_word(want, off, 0x01020304u);
        }
      INFO("first difference: " << first_difference(g.bytes, want));
      CHECK(g.bytes == want);
    }
    SECTION("destination running past its physical allocation") {
      // Allocations [0x1000, 0x5000) and [0x5000, 0x7000); the rest is unallocated. A 64x64 32bpp resolve at 0x5000
      // (16 KiB of macro tiles) must stop at 0x7000.
      mem.allocation_end = [](uint32_t phys) -> std::optional<uint32_t> {
        if (phys >= 0x1000 && phys < 0x5000) return 0x5000u;
        if (phys >= 0x5000 && phys < 0x7000) return 0x7000u;
        return std::nullopt;
      };
      set_dest(regs, 0x5000, kPitch, 64, edram::kDest_8_8_8_8, lr::gpu::pm4::kEndianNone);
      regs.write(reg::RB_COPY_CONTROL, copy_control(0, 0, true, false, 0));
      REQUIRE(edram_.clear(regs).ok());
      REQUIRE(edram_.resolve(regs, mem).ok());
      std::vector<uint8_t> want(kReachable + kCanary, kSentinel);
      uint32_t written = 0;
      for (uint32_t y = 0; y < 64; ++y)
        for (uint32_t x = 0; x < kPitch; ++x) {
          const uint32_t off = 0x5000 + lr::testing::xenos_tiled_offset_2d(x, y, kPitch, 2);
          if (off + 4 <= 0x7000) {
            put_word(want, off, 0x01020304u);
            ++written;
          }
        }
      CHECK(written == 2u * 1024u);  // the first two macro tiles
      INFO("first difference: " << first_difference(g.bytes, want));
      CHECK(g.bytes == want);
    }
    SECTION("destination not in a physical allocation") {
      mem.allocation_end = [](uint32_t) -> std::optional<uint32_t> { return std::nullopt; };
      set_dest(regs, 0x2000, kPitch, 64, edram::kDest_8_8_8_8, lr::gpu::pm4::kEndianNone);
      regs.write(reg::RB_COPY_CONTROL, copy_control(0, 0, true, false, 0));
      REQUIRE(edram_.clear(regs).ok());
      REQUIRE(edram_.resolve(regs, mem).ok());
      CHECK(g.bytes == std::vector<uint8_t>(kReachable + kCanary, kSentinel));
    }
    SECTION("destination base outside guest memory") {
      set_dest(regs, 0x1FFFF000, kPitch, 64, edram::kDest_8_8_8_8, lr::gpu::pm4::kEndianNone);
      regs.write(reg::RB_COPY_CONTROL, copy_control(0, 0, false, false, 0));
      REQUIRE(edram_.resolve(regs, mem).ok());
      CHECK(g.bytes == std::vector<uint8_t>(kReachable + kCanary, kSentinel));
    }
    SECTION("huge pitch") {
      set_dest(regs, 0x7FFC, 0x3FFF, 0x3FFF, edram::kDest_8_8_8_8, lr::gpu::pm4::kEndianNone);
      regs.write(reg::RB_COPY_CONTROL, copy_control(0, 0, false, false, 0));
      REQUIRE(edram_.resolve(regs, mem).ok());
      CHECK(std::vector<uint8_t>(g.bytes.begin() + kReachable, g.bytes.end()) ==
            std::vector<uint8_t>(kCanary, kSentinel));
    }
  }
  CHECK(vk_errors.errors.empty());
}
