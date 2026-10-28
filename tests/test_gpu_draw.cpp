// SPDX-License-Identifier: MIT
// The draw path (Task 20, plan B): synthetic Xenos microcode (support/xenos_shader_builder.h) translated by the
// runtime, drawn by the Vulkan backend into EDRAM render targets, resolved into guest memory and read back with the
// CPU tiling reference (support/xenos_tiling.h). Every [vulkan] test runs with the validation layer on and fails on
// any validation error. The pure helpers of runtime/gpu/vk/draw.h are checked without a device.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/log.h"
#include "runtime/be.h"
#include "runtime/gpu/gpu_system.h"
#include "runtime/gpu/mmio.h"
#include "runtime/gpu/pm4.h"
#include "runtime/gpu/registers.h"
#include "runtime/gpu/shader/shader_key.h"
#include "runtime/gpu/shader/translator.h"
#include "runtime/gpu/shader_cache.h"
#include "runtime/gpu/vk/draw.h"
#include "runtime/gpu/vk/vk_backend.h"
#include "runtime/kernel/kernel.h"
#include "runtime/memory/physical_heap.h"
#include "support/kernel_fixture.h"
#include "support/pm4_builder.h"
#include "support/vk_shader_harness.h"
#include "support/xenos_shader_builder.h"
#include "support/xenos_tiling.h"

namespace gpu = lr::gpu;
namespace vk = lr::gpu::vk;
namespace pm4 = lr::gpu::pm4;
namespace x = lr::testing::xenos;
using gpu::ActiveShaders;
using gpu::DrawInfo;
using gpu::MemoryAccess;
using gpu::RegisterFile;
using gpu::ShaderStage;
using namespace std::chrono_literals;

namespace {

constexpr uint32_t kSize = 32;            // the render target is kSize x kSize pixels (RB_SURFACE_INFO pitch 32)
constexpr uint32_t kSlot = 3;             // the vertex fetch constant the tests use (registers 0x4806/0x4807)
constexpr uint32_t kFormatFloat4 = x::kFormat32_32_32_32Float;
constexpr uint32_t kFormatHalf2 = 31;     // k_16_16_FLOAT
constexpr uint32_t kColor8888 = 0;        // RB_COLOR_INFO.color_format k_8_8_8_8
constexpr uint32_t kDest8888 = 6;         // RB_COPY_DEST_INFO.copy_dest_format k_8_8_8_8
constexpr uint32_t kBlue = 0xFFFF0000u;   // A B G R from the top (R in the low byte, like the resolved word)
constexpr uint32_t kRed = 0xFF0000FFu;
constexpr uint32_t kGreen = 0xFF00FF00u;

// Direct-backend guest: physical == guest address; [0, kAllocEnd) is one physical allocation, the rest unallocated.
constexpr uint32_t kGuestSize = 4u << 20;
constexpr uint32_t kAllocEnd = 0x300000;
constexpr uint32_t kVertexBase = 0x10000, kIndexBase = 0x20000, kTextureBase = 0x100000, kDestBase = 0x200000;

uint32_t f2u(float f) { return std::bit_cast<uint32_t>(f); }

std::string hex(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08X", v);
  return b;
}

// Collects the Vulkan validation-layer errors (Device logs them with log::error) for the scope.
class ValidationErrors {
 public:
  ValidationErrors() {
    previous_ = lr::log::set_sink([this](lr::log::Level level, std::string_view msg) {
      if (level == lr::log::Level::Error) {
        std::lock_guard<std::mutex> lock(m_);
        errors_.emplace_back(msg);
      }
      std::fprintf(stderr, "%.*s\n", static_cast<int>(msg.size()), msg.data());
    });
  }
  ~ValidationErrors() { lr::log::set_sink(std::move(previous_)); }
  std::vector<std::string> errors() {
    std::lock_guard<std::mutex> lock(m_);
    return errors_;
  }

 private:
  std::mutex m_;
  std::vector<std::string> errors_;
  lr::log::Sink previous_;
};

// --- microcode ----------------------------------------------------------------------------------------------------

x::AluSrc r(uint32_t reg) { return {reg, true, 0}; }
x::AluSrc c(uint32_t reg) { return {reg, false, 0}; }

x::Alu mov(uint32_t dest, x::AluSrc s, bool to_export) {
  x::Alu a;
  a.vector_op = x::kVectorMax;
  a.vector_dest = dest;
  a.export_data = to_export;
  a.src1 = s;
  a.src2 = s;
  return a;
}

// VS: r1 = float4 position, r2 = float4 colour from the vertex (stride 8 dwords, fetch constant kSlot, index r0.x);
// exports the position and interpolator 0 = colour.
std::vector<uint32_t> vs_position_color() {
  std::vector<uint32_t> code;
  x::emit(code, x::cf_pair(x::cf_exec(true, 1, 4, 0b0101), x::cf_nop()));
  x::emit(code, x::vfetch(1, 0, kSlot, 0, 8, kFormatFloat4));
  x::emit(code, x::vfetch_mini(2, 4, kFormatFloat4));
  x::emit(code, x::alu(mov(x::kExportVsPosition, r(1), true)));
  x::emit(code, x::alu(mov(x::kExportVsInterpolator0, r(2), true)));
  return code;
}

// VS: position = k_16_16_FLOAT xy (z 0, w 1 by the missing-component rule), one dword per vertex.
std::vector<uint32_t> vs_half2() {
  std::vector<uint32_t> code;
  x::emit(code, x::cf_pair(x::cf_exec(true, 1, 2, 0b01), x::cf_nop()));
  x::emit(code, x::vfetch(1, 0, kSlot, 0, 1, kFormatHalf2));
  x::emit(code, x::alu(mov(x::kExportVsPosition, r(1), true)));
  return code;
}

std::vector<uint32_t> alu_only(const std::vector<x::Alu>& slots) {
  std::vector<uint32_t> code;
  x::emit(code, x::cf_pair(x::cf_exec(true, 1, uint32_t(slots.size()), 0), x::cf_nop()));
  for (const x::Alu& a : slots) x::emit(code, x::alu(a));
  return code;
}

std::vector<uint32_t> ps_interpolated() { return alu_only({mov(x::kExportPsColor0, r(0), true)}); }
std::vector<uint32_t> ps_constant() { return alu_only({mov(x::kExportPsColor0, c(0), true)}); }
std::vector<uint32_t> ps_memexport() { return alu_only({mov(x::kExportAddress, r(0), true)}); }

// PS: r0 = c0 (texture coordinates), r1 = tfetch tf0 at r0 (2D), oC0 = r1.
std::vector<uint32_t> ps_texture() {
  std::vector<uint32_t> code;
  x::emit(code, x::cf_pair(x::cf_exec(true, 1, 3, 0b0100), x::cf_nop()));
  x::emit(code, x::alu(mov(0, c(0), false)));
  x::emit(code, x::tfetch(1, 0, 0, x::kTextureDim2D));
  x::emit(code, x::alu(mov(x::kExportPsColor0, r(1), true)));
  return code;
}

// --- register state -----------------------------------------------------------------------------------------------

using RegList = std::vector<std::pair<uint32_t, uint32_t>>;

// A kSize x kSize k_8_8_8_8 colour target at EDRAM tile 0, a D24S8 depth target at tile 64, the D3D viewport
// mapping clip [-1, 1] onto it (y down), pixel centres at .5 (pix_center 1), [0, w] clip z, no culling, no
// blending, every index in range.
RegList default_state() {
  const float half = kSize / 2.0f;
  return {
      {gpu::RB_SURFACE_INFO, kSize},
      {gpu::RB_COLOR_INFO, kColor8888 << 16},
      {gpu::RB_DEPTH_INFO, 64u | (0u << 16)},
      {gpu::RB_COLOR_MASK, 0xF},
      {gpu::RB_BLENDCONTROL0, 0x00010001u},  // colour and alpha: src one, dst zero, add
      {gpu::RB_DEPTHCONTROL, 0},
      {gpu::RB_COLORCONTROL, 0},
      {gpu::PA_SC_WINDOW_SCISSOR_TL, 1u << 31},
      {gpu::PA_SC_WINDOW_SCISSOR_BR, kSize | (kSize << 16)},
      {gpu::PA_SC_SCREEN_SCISSOR_TL, 0},
      {gpu::PA_SC_SCREEN_SCISSOR_BR, kSize | (kSize << 16)},
      {gpu::PA_CL_VTE_CNTL, 0x3Fu | (1u << 10)},  // scale/offset enables, W given as W
      {gpu::PA_CL_VPORT_XSCALE, f2u(half)},
      {gpu::PA_CL_VPORT_XOFFSET, f2u(half)},
      {gpu::PA_CL_VPORT_YSCALE, f2u(-half)},
      {gpu::PA_CL_VPORT_YOFFSET, f2u(half)},
      {gpu::PA_CL_VPORT_ZSCALE, f2u(1.0f)},
      {gpu::PA_CL_VPORT_ZOFFSET, f2u(0.0f)},
      {gpu::PA_CL_CLIP_CNTL, 1u << 19},  // dx_clip_space_def
      {gpu::PA_SU_VTX_CNTL, 1},          // pix_center 1: no half-pixel offset
      {gpu::PA_SU_SC_MODE_CNTL, 0},
      {gpu::VGT_MAX_VTX_INDX, 0xFFFFFF},
      {gpu::VGT_MIN_VTX_INDX, 0},
      {gpu::VGT_INDX_OFFSET, 0},
      {gpu::RB_MODECONTROL, 4},
  };
}

// Vertex fetch constant kSlot: `dwords` dwords at physical `base`, endian 8in32.
RegList vertex_fetch(uint32_t base, uint32_t dwords) {
  return {{gpu::kShaderConstantFetchBase + 2 * kSlot, base | 3u},
          {gpu::kShaderConstantFetchBase + 2 * kSlot + 1, pm4::kEndian8in32 | (dwords << 2)}};
}

// The vertex data the guest CPU writes: big-endian dwords.
std::vector<uint8_t> be_words(const std::vector<uint32_t>& words) {
  std::vector<uint8_t> out;
  for (uint32_t w : words)
    for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(w >> s));
  return out;
}

using Vertex = std::array<float, 8>;  // position xyzw, colour rgba
std::vector<uint32_t> vertex_words(const std::vector<Vertex>& vs) {
  std::vector<uint32_t> out;
  for (const Vertex& v : vs)
    for (float f : v) out.push_back(f2u(f));
  return out;
}

Vertex vert(float px, float py, std::array<float, 4> color = {1, 0, 0, 1}, float pz = 0.0f) {
  return {px, py, pz, 1.0f, color[0], color[1], color[2], color[3]};
}

// A triangle covering the whole target.
std::vector<Vertex> fullscreen(std::array<float, 4> color = {1, 0, 0, 1}, float z = 0.0f) {
  return {vert(-1, 1, color, z), vert(3, 1, color, z), vert(-1, -3, color, z)};
}

// Resolve: RB_COPY_CONTROL copying colour slot 0 (or depth) raw into `dest` as k_8_8_8_8, endian none.
RegList resolve_state(uint32_t dest, bool depth = false) {
  return {{gpu::RB_COPY_CONTROL, depth ? 4u : 0u},
          {gpu::RB_COPY_DEST_BASE, dest},
          {gpu::RB_COPY_DEST_PITCH, kSize | (kSize << 16)},
          {gpu::RB_COPY_DEST_INFO, kDest8888 << 7}};
}

// --- direct-backend fixture ---------------------------------------------------------------------------------------

struct Guest {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(kGuestSize, 0);
  MemoryAccess access() {
    MemoryAccess m;
    m.base = bytes.data();
    m.to_guest = [](uint32_t phys) -> std::optional<uint32_t> {
      if (phys >= kGuestSize) return std::nullopt;
      return phys;
    };
    m.size = kGuestSize;
    m.allocation_end = [](uint32_t phys) -> std::optional<uint32_t> {
      if (phys >= kAllocEnd) return std::nullopt;
      return kAllocEnd;
    };
    return m;
  }
  void put(uint32_t at, const std::vector<uint8_t>& data) { std::memcpy(bytes.data() + at, data.data(), data.size()); }
  uint32_t word(uint32_t at) const {
    uint32_t w;
    std::memcpy(&w, bytes.data() + at, 4);
    return w;
  }
};

struct Fixture {
  Guest guest;
  RegisterFile regs;
  std::unique_ptr<vk::VkBackend> backend;
  gpu::ShaderCache cache;
  std::vector<std::string> fatals;
  bool ok = false;

  Fixture() {
    vk::VkBackendOptions o;
    o.mem = guest.access();
    o.validation = true;
    auto b = vk::VkBackend::create(std::move(o));
    if (!b) return;
    backend = std::move(b.value());
    lr::testing::pin_vulkan_drivers();
    backend->set_fatal_handler([this](const std::string& what) { fatals.push_back(what); });
    set(default_state());
    ok = true;
  }

  void set(const RegList& list) {
    for (const auto& [reg, value] : list) regs.write(reg, value);
  }
  void set_float(uint32_t reg, float v) { regs.write(reg, f2u(v)); }
  // PS constant c<i> = (x, y, z, w): register-file float constant 256 + i.
  void ps_constant(uint32_t i, std::array<float, 4> v) {
    for (uint32_t k = 0; k < 4; ++k) set_float(gpu::kShaderConstantFloatBase + 4 * (256 + i) + k, v[k]);
  }

  void vertices(const std::vector<Vertex>& vs) {
    const std::vector<uint32_t> words = vertex_words(vs);
    guest.put(kVertexBase, be_words(words));
    set(vertex_fetch(kVertexBase, uint32_t(words.size())));
  }

  const gpu::shader::TranslatedShader* translate(ShaderStage stage, const std::vector<uint32_t>& code,
                                                 const DrawInfo& info, gpu::shader::ShaderKey& key) {
    const std::vector<uint8_t> bytes = x::microcode_bytes(code);
    const uint64_t hash = cache.load(stage, bytes.data(), uint32_t(code.size()));
    const uint32_t inputs = cache.referenced_interpolators(stage, hash, uint32_t(code.size()));
    key = {stage, hash, uint32_t(code.size()), gpu::shader::key_modifiers(stage, regs, info, inputs)};
    auto t = cache.translate(key);
    if (!t) FAIL("translate: " << t.error().message);
    return t.value();
  }

  ActiveShaders shaders(const DrawInfo& info, const std::vector<uint32_t>& vs, const std::vector<uint32_t>& ps) {
    ActiveShaders a;
    a.vs = translate(ShaderStage::Vertex, vs, info, a.vs_key);
    a.ps = translate(ShaderStage::Pixel, ps, info, a.ps_key);
    return a;
  }

  void draw(uint32_t prim, uint32_t count, const std::vector<uint32_t>& vs, const std::vector<uint32_t>& ps) {
    DrawInfo info{prim, count, 0, 0, false, prim | (2u << 6) | (count << 16)};
    backend->draw(regs, info, shaders(info, vs, ps));
  }

  // An indexed draw: the guest index bytes are placed at kIndexBase.
  void draw_indexed(uint32_t prim, const std::vector<uint8_t>& index_bytes, uint32_t index_size, uint32_t endian,
                    const std::vector<uint32_t>& vs, const std::vector<uint32_t>& ps) {
    guest.put(kIndexBase, index_bytes);
    const uint32_t count = uint32_t(index_bytes.size() / index_size);
    DrawInfo info{prim, count, kIndexBase, index_size, true, prim | (count << 16), endian, count};
    backend->draw(regs, info, shaders(info, vs, ps));
  }

  // Clears colour slot 0 to `rgba` (a resolve that only clears: RB_COPY_CONTROL copy_command 3).
  void clear(uint32_t rgba) {
    regs.write(gpu::RB_COPY_CONTROL, (1u << 8) | (3u << 20));
    regs.write(gpu::RB_COLOR_CLEAR, rgba);
    backend->resolve(regs);
  }

  // Resolves colour slot 0 (or the depth target) and returns its kSize x kSize words, row-major.
  std::vector<uint32_t> read(bool depth = false) {
    set(resolve_state(kDestBase, depth));
    std::memset(guest.bytes.data() + kDestBase, 0xEE, kSize * kSize * 4);
    backend->resolve(regs);
    std::vector<uint32_t> out(kSize * kSize);
    for (uint32_t y = 0; y < kSize; ++y)
      for (uint32_t x0 = 0; x0 < kSize; ++x0)
        out[y * kSize + x0] = guest.word(kDestBase + lr::testing::xenos_tiled_offset_2d(x0, y, kSize, 2));
    return out;
  }
};

uint32_t at(const std::vector<uint32_t>& px, uint32_t x0, uint32_t y) { return px[y * kSize + x0]; }

uint32_t channel(uint32_t word, uint32_t i) { return (word >> (8 * i)) & 0xFF; }

// --- GpuSystem fixture (the PM4 path) -----------------------------------------------------------------------------

bool wait_until(const std::function<bool()>& pred, std::chrono::milliseconds limit = 60s) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (!pred()) {
    if (std::chrono::steady_clock::now() > end) return pred();
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

struct Phys {
  uint32_t guest, phys;
};
Phys phys_alloc(uint32_t bytes) {
  auto res = lr::kernel::kernel().physical().allocate(bytes, 0x20000004 /* MEM_LARGE_PAGES | PAGE_READWRITE */, 0,
                                                      0xFFFFFFFFu, 0);
  REQUIRE(res);
  return {res.value(), lr::runtime::PhysicalHeap::physical_address(res.value())};
}

// A GpuSystem over the kernel fixture's memory driving a VkBackend, recording fatals (they return: the GPU thread
// stops).
struct GpuRig {
  lr::testing::KernelFixture& f;
  std::mutex m;
  std::vector<std::string> fatals;
  std::unique_ptr<gpu::GpuSystem> gpu;
  Phys ring = phys_alloc(0x10000);
  uint32_t wptr = 0;
  bool ok = false;

  explicit GpuRig(lr::testing::KernelFixture& fx) : f(fx) {
    gpu::GpuHooks hooks;
    hooks.alloc_guest = [this](uint32_t bytes) { return f.alloc(bytes); };
    hooks.fatal = [this](const std::string& what) {
      std::lock_guard<std::mutex> lock(m);
      fatals.push_back(what);
    };
    gpu = std::make_unique<gpu::GpuSystem>(lr::kernel::kernel().gpu_memory_access(), std::move(hooks));
    vk::VkBackendOptions o;
    o.mem = lr::kernel::kernel().gpu_memory_access();
    o.validation = true;
    auto b = vk::VkBackend::create(std::move(o));
    if (!b) return;
    lr::testing::pin_vulkan_drivers();
    gpu->set_backend(std::move(b.value()));
    gpu->initialize_ring_buffer(ring.phys, 9);  // 1024 dwords
    ok = true;
  }
  ~GpuRig() {
    gpu->stop();
    gpu.reset();
    *f.mem.translate<lr::runtime::be<uint32_t>>(gpu::mmio_register_address(gpu::CP_RB_WPTR)) = 0;
  }
  std::string fatal() {
    std::lock_guard<std::mutex> lock(m);
    return fatals.empty() ? std::string() : fatals.front();
  }
  void run(const lr::testing::Pm4Builder& b) {
    for (uint32_t d : b.dwords()) *f.mem.translate<lr::runtime::be<uint32_t>>(ring.guest + 4 * wptr++) = d;
    *f.mem.translate<lr::runtime::be<uint32_t>>(gpu::mmio_register_address(gpu::CP_RB_WPTR)) = wptr;
    gpu->start();
    REQUIRE(wait_until([&] { return gpu->read_pointer() == wptr || !fatal().empty(); }));
  }
};

void put_registers(lr::testing::Pm4Builder& b, const RegList& list) {
  for (const auto& [reg, value] : list) b.type0(reg, {value});
}

void im_load_immediate(lr::testing::Pm4Builder& b, ShaderStage stage, const std::vector<uint32_t>& microcode) {
  std::vector<uint32_t> payload = {stage == ShaderStage::Vertex ? 0u : 1u, uint32_t(microcode.size())};
  payload.insert(payload.end(), microcode.begin(), microcode.end());
  b.type3(pm4::IM_LOAD_IMMEDIATE, payload);
}

}  // namespace

// --- pure helpers -------------------------------------------------------------------------------------------------

TEST_CASE("primitive types map to Vulkan topologies or name what is unsupported", "[unit][gpu]") {
  auto tri = vk::primitive(vk::prim::kTriangleList);
  REQUIRE(tri);
  CHECK(tri.value().topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
  CHECK(tri.value().list);
  auto fan = vk::primitive(vk::prim::kTriangleFan);
  REQUIRE(fan);
  CHECK(fan.value().topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN);
  CHECK_FALSE(fan.value().list);
  auto quads = vk::primitive(vk::prim::kQuadList);
  REQUIRE(quads);
  CHECK(quads.value().quads);
  auto rects = vk::primitive(vk::prim::kRectangleList);
  REQUIRE(rects);
  CHECK(rects.value().rects);
  auto loop = vk::primitive(vk::prim::kLineLoop);
  REQUIRE_FALSE(loop);
  CHECK(loop.error().message == "primitive type 0x0C (line loop) is not supported");
  CHECK_FALSE(vk::primitive(0x3F));
  CHECK(vk::expand_quads({0, 1, 2, 3, 4, 5, 6, 7, 8}) == std::vector<uint32_t>{0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7});
}

TEST_CASE("viewport and ndc follow PA_CL_VPORT, the window offset and the pixel centre", "[unit][gpu]") {
  RegisterFile regs;
  for (const auto& [reg, value] : default_state()) regs.write(reg, value);
  SECTION("pix_center 1: the viewport is the target, the transform the identity") {
    const vk::ViewportInfo v = vk::viewport_info(regs, kSize, kSize, false, false);
    CHECK(v.x == 0.0f);
    CHECK(v.y == 0.0f);
    CHECK(v.width == float(kSize));
    CHECK(v.height == float(kSize));
    CHECK(v.ndc_scale[0] == 1.0f);
    CHECK(v.ndc_scale[1] == -1.0f);  // the guest's negative YSCALE: y down, as in Vulkan
    CHECK(v.ndc_offset[0] == 0.0f);
    CHECK(v.ndc_offset[1] == 0.0f);
    CHECK(v.min_depth == 0.0f);
    CHECK(v.max_depth == 1.0f);
  }
  SECTION("pix_center 0 (Direct3D) moves everything by half a pixel") {
    regs.write(gpu::PA_SU_VTX_CNTL, 0);
    const vk::ViewportInfo v = vk::viewport_info(regs, kSize, kSize, false, false);
    // Snapped to [0, 32] (16.5 +- 16 truncated), the half pixel is carried by ndc_offset: 0.5 * 2 / 32.
    CHECK(v.width == float(kSize));
    CHECK(v.ndc_offset[0] == 1.0f / 32.0f);
    CHECK(v.ndc_offset[1] == 1.0f / 32.0f);
  }
  SECTION("window offset with vtx_window_offset_enable") {
    regs.write(gpu::PA_SC_WINDOW_OFFSET, 8u | (4u << 16));
    regs.write(gpu::PA_SU_SC_MODE_CNTL, 1u << 16);
    regs.write(gpu::PA_CL_VPORT_XSCALE, f2u(8.0f));
    regs.write(gpu::PA_CL_VPORT_XOFFSET, f2u(8.0f));
    const vk::ViewportInfo v = vk::viewport_info(regs, kSize, kSize, false, false);
    CHECK(v.x == 8.0f);  // [0, 16] moved by 8
    CHECK(v.width == 16.0f);
    CHECK(v.y == 4.0f);  // [4, 36] clamped to the target
    CHECK(v.height == 28.0f);
  }
  SECTION("[-w, w] clip z is halved; a D24FS8 target halves the depth range") {
    regs.write(gpu::PA_CL_CLIP_CNTL, 0);
    regs.write(gpu::PA_CL_VPORT_ZSCALE, f2u(0.5f));
    regs.write(gpu::PA_CL_VPORT_ZOFFSET, f2u(0.5f));
    const vk::ViewportInfo v = vk::viewport_info(regs, kSize, kSize, false, true);
    CHECK(v.ndc_scale[2] == 0.5f);
    CHECK(v.ndc_offset[2] == 0.5f);
    CHECK(v.min_depth == 0.0f);
    CHECK(v.max_depth == 0.5f);
  }
  SECTION("garbage scale gives a finite viewport") {
    regs.write(gpu::PA_CL_VPORT_XSCALE, 0x7FC00000u);  // NaN
    const vk::ViewportInfo v = vk::viewport_info(regs, kSize, kSize, false, false);
    CHECK(std::isfinite(v.ndc_scale[0]));
    CHECK(std::isfinite(v.width));
  }
}

TEST_CASE("index buffers are bounds-checked and endian-swapped", "[unit][gpu]") {
  Guest g;
  MemoryAccess mem = g.access();
  RegisterFile regs;
  auto tri = vk::primitive(vk::prim::kTriangleList).value();
  auto strip = vk::primitive(vk::prim::kTriangleStrip).value();
  SECTION("16-bit 8in16 and 32-bit 8in32 indices, 24-bit masked") {
    g.put(0x1000, {0x00, 0x03, 0x00, 0x01, 0x00, 0x02});
    DrawInfo d16{4, 3, 0x1000, 2, true, 0, pm4::kEndian8in16, 3};
    auto i16 = vk::read_indices(d16, regs, mem, tri);
    REQUIRE(i16);
    CHECK(i16.value().indices == std::vector<uint32_t>{3, 1, 2});
    g.put(0x2000, {0xFF, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x06});
    DrawInfo d32{4, 2, 0x2000, 4, true, 0, pm4::kEndian8in32, 2};
    auto i32 = vk::read_indices(d32, regs, mem, tri);
    REQUIRE(i32);
    CHECK(i32.value().indices == std::vector<uint32_t>{5, 6});
  }
  SECTION("an index count above the DMA size is cut to it") {
    DrawInfo d{4, 6, 0x1000, 2, true, 0, pm4::kEndian8in16, 3};
    auto i = vk::read_indices(d, regs, mem, tri);
    REQUIRE(i);
    CHECK(i.value().indices.size() == 3);
  }
  SECTION("outside guest memory or the physical allocation is an error naming the address") {
    DrawInfo outside{4, 3, kGuestSize + 0x1000, 2, true, 0, 0, 3};
    auto a = vk::read_indices(outside, regs, mem, tri);
    REQUIRE_FALSE(a);
    CHECK(a.error().message.find("index buffer at physical " + hex(kGuestSize + 0x1000)) != std::string::npos);
    DrawInfo unallocated{4, 3, kAllocEnd + 0x100, 2, true, 0, 0, 3};
    auto b = vk::read_indices(unallocated, regs, mem, tri);
    REQUIRE_FALSE(b);
    CHECK(b.error().message.find("is not in a physical allocation") != std::string::npos);
    DrawInfo straddling{4, 64, kAllocEnd - 8, 2, true, 0, 0, 64};
    auto c = vk::read_indices(straddling, regs, mem, tri);
    REQUIRE_FALSE(c);
    CHECK(c.error().message.find("runs past the end") != std::string::npos);
  }
  SECTION("a reset index in a list is an error; in a strip it restarts") {
    regs.write(gpu::PA_SU_SC_MODE_CNTL, 1u << 21);
    regs.write(gpu::VGT_MULTI_PRIM_IB_RESET_INDX, 0xFFFF);
    g.put(0x1000, {0x00, 0x00, 0xFF, 0xFF, 0x00, 0x01});
    DrawInfo d{6, 3, 0x1000, 2, true, 0, pm4::kEndian8in16, 3};
    auto s = vk::read_indices(d, regs, mem, strip);
    REQUIRE(s);
    CHECK(s.value().restart);
    CHECK(s.value().indices == std::vector<uint32_t>{0, 0xFFFFFFFFu, 1});
    auto l = vk::read_indices(d, regs, mem, tri);
    REQUIRE_FALSE(l);
    CHECK(l.error().message.find("primitive reset index 0xFFFF in a triangle list") != std::string::npos);
  }
}

TEST_CASE("vertex ranges are clamped to their allocation in bytes", "[unit][gpu]") {
  Guest g;
  MemoryAccess mem = g.access();
  RegisterFile regs;
  for (const auto& [reg, value] : vertex_fetch(0x1000, 12)) regs.write(reg, value);
  auto r = vk::vertex_range(regs, kSlot, mem);
  REQUIRE(r);
  CHECK(r.value().guest == 0x1000);
  CHECK(r.value().size_bytes == 48);
  CHECK(r.value().endian == pm4::kEndian8in32);
  // A size running past the allocation end is cut there.
  for (const auto& [reg, value] : vertex_fetch(kAllocEnd - 16, 1000)) regs.write(reg, value);
  auto cut = vk::vertex_range(regs, kSlot, mem);
  REQUIRE(cut);
  CHECK(cut.value().size_bytes == 16);
  // Another fetch-constant type reads nothing.
  regs.write(gpu::kShaderConstantFetchBase + 2 * kSlot, 0x1000u | 2u);
  auto other = vk::vertex_range(regs, kSlot, mem);
  REQUIRE(other);
  CHECK(other.value().size_bytes == 0);
}

// --- [vulkan] ----------------------------------------------------------------------------------------------------

TEST_CASE("triangle from IM_LOAD_IMMEDIATE microcode renders", "[unit][vulkan][draw]") {
  ValidationErrors vk_errors;
  {
    lr::testing::KernelFixture f;
    GpuRig rig(f);
    if (!rig.ok) SKIP("no Vulkan ICD installed");
    const Phys vb = phys_alloc(0x1000);
    const Phys dest = phys_alloc(0x10000);
    const std::vector<Vertex> tri = {vert(-1, 1, {0, 1, 0, 1}), vert(1, 1, {0, 1, 0, 1}), vert(-1, -1, {0, 1, 0, 1})};
    const std::vector<uint8_t> bytes = be_words(vertex_words(tri));
    std::memcpy(f.mem.base() + vb.guest, bytes.data(), bytes.size());
    std::memset(f.mem.base() + dest.guest, 0xEE, kSize * kSize * 4);

    lr::testing::Pm4Builder b;
    put_registers(b, default_state());
    put_registers(b, vertex_fetch(vb.phys, 24));
    im_load_immediate(b, ShaderStage::Vertex, vs_position_color());
    im_load_immediate(b, ShaderStage::Pixel, ps_interpolated());
    b.type0(gpu::RB_MODECONTROL, {4});
    b.type3(pm4::DRAW_INDX_2, {4u | (2u << 6) | (3u << 16)});  // auto-index triangle list, 3 vertices
    put_registers(b, resolve_state(dest.phys));
    b.type0(gpu::RB_MODECONTROL, {6});
    b.type3(pm4::DRAW_INDX_2, {8u | (2u << 6) | (3u << 16)});  // the resolve
    rig.run(b);
    INFO(rig.fatal());
    REQUIRE(rig.fatal().empty());
    auto px = [&](uint32_t x0, uint32_t y) {
      uint32_t w;
      std::memcpy(&w, f.mem.base() + dest.guest + lr::testing::xenos_tiled_offset_2d(x0, y, kSize, 2), 4);
      return w;
    };
    CHECK(px(2, 2) == kGreen);   // inside the top-left half
    CHECK(px(20, 5) == kGreen);
    CHECK(px(29, 29) == 0);      // outside: the zero-filled target
    CHECK(px(20, 20) == 0);
  }
  CHECK(vk_errors.errors().empty());
}

TEST_CASE("blend state from registers", "[unit][vulkan][draw]") {
  ValidationErrors vk_errors;
  {
    Fixture f;
    if (!f.ok) SKIP("no Vulkan ICD installed");
    f.clear(kBlue);
    f.vertices(fullscreen());
    f.ps_constant(0, {1.0f, 0.0f, 0.0f, 0.5f});
    // colour: src * SRC_ALPHA + dst * ONE_MINUS_SRC_ALPHA; alpha: src * ONE + dst * ZERO.
    f.regs.write(gpu::RB_BLENDCONTROL0, 6u | (0u << 5) | (7u << 8) | (1u << 16) | (0u << 21) | (0u << 24));
    SECTION("all components") {
      f.draw(vk::prim::kTriangleList, 3, vs_position_color(), ps_constant());
      const auto px = f.read();
      const uint32_t p = at(px, 16, 16);
      INFO(hex(p));
      CHECK(std::abs(int(channel(p, 0)) - 128) <= 1);  // R: 0.5 * 1 + 0.5 * 0
      CHECK(channel(p, 1) == 0);
      CHECK(std::abs(int(channel(p, 2)) - 128) <= 1);  // B: 0.5 * 0 + 0.5 * 1
      CHECK(std::abs(int(channel(p, 3)) - 128) <= 1);  // A: the source alpha
    }
    SECTION("RB_COLOR_MASK keeps alpha") {
      f.regs.write(gpu::RB_COLOR_MASK, 0x7);
      f.draw(vk::prim::kTriangleList, 3, vs_position_color(), ps_constant());
      const uint32_t p = at(f.read(), 16, 16);
      INFO(hex(p));
      CHECK(std::abs(int(channel(p, 0)) - 128) <= 1);
      CHECK(channel(p, 3) == 0xFF);  // not written
    }
    CHECK(f.fatals.empty());
  }
  CHECK(vk_errors.errors().empty());
}

TEST_CASE("quad list expansion", "[unit][vulkan][draw]") {
  ValidationErrors vk_errors;
  {
    Fixture f;
    if (!f.ok) SKIP("no Vulkan ICD installed");
    // One quad, corners in order TL, TR, BR, BL: pixels 8..23 when drawn as (TL, TR, BR) + (TL, BR, BL).
    f.vertices({vert(-0.5f, 0.5f), vert(0.5f, 0.5f), vert(0.5f, -0.5f), vert(-0.5f, -0.5f)});
    f.draw(vk::prim::kQuadList, 4, vs_position_color(), ps_interpolated());
    const auto px = f.read();
    CHECK(at(px, 20, 10) == kRed);  // the first triangle
    CHECK(at(px, 10, 20) == kRed);  // the second one
    CHECK(at(px, 9, 9) == kRed);
    CHECK(at(px, 22, 22) == kRed);
    CHECK(at(px, 2, 2) == 0);
    CHECK(at(px, 26, 26) == 0);
    CHECK(f.fatals.empty());
  }
  CHECK(vk_errors.errors().empty());
}

TEST_CASE("rect list expansion", "[unit][vulkan][draw]") {
  ValidationErrors vk_errors;
  {
    Fixture f;
    if (!f.ok) SKIP("no Vulkan ICD installed");
    // Three corners (TL black, TR red, BL green): the fourth, BR, is TR + BL - TL: position (0.5, -0.5), yellow.
    f.vertices({vert(-0.5f, 0.5f, {0, 0, 0, 1}), vert(0.5f, 0.5f, {1, 0, 0, 1}), vert(-0.5f, -0.5f, {0, 1, 0, 1})});
    f.draw(vk::prim::kRectangleList, 3, vs_position_color(), ps_interpolated());
    INFO((f.fatals.empty() ? std::string() : f.fatals.front()));
    REQUIRE(f.fatals.empty());
    const auto px = f.read();
    const uint32_t br = at(px, 23, 23);  // only the generated triangle covers this corner
    INFO(hex(br));
    CHECK(channel(br, 0) > 200);
    CHECK(channel(br, 1) > 200);
    CHECK(channel(br, 2) == 0);
    const uint32_t tl = at(px, 8, 8);
    INFO(hex(tl));
    CHECK(channel(tl, 0) < 40);
    CHECK(channel(tl, 1) < 40);
    CHECK(at(px, 26, 26) == 0);
    CHECK(at(px, 5, 5) == 0);
  }
  CHECK(vk_errors.errors().empty());
}

TEST_CASE("vertex k_16_16_FLOAT with 8in32 swap through the fetch table", "[unit][vulkan][draw]") {
  ValidationErrors vk_errors;
  {
    Fixture f;
    if (!f.ok) SKIP("no Vulkan ICD installed");
    // Half floats (x in the low half): (-1, 1), (1, 1), (-1, -1): the top-left half of the target.
    constexpr uint32_t kOne = 0x3C00, kMinusOne = 0xBC00;
    const std::vector<uint32_t> words = {kMinusOne | (kOne << 16), kOne | (kOne << 16), kMinusOne | (kMinusOne << 16)};
    f.guest.put(kVertexBase, be_words(words));  // big-endian dwords: the 8in32 swap restores them
    f.set(vertex_fetch(kVertexBase, 3));
    f.ps_constant(0, {0, 1, 0, 1});
    f.draw(vk::prim::kTriangleList, 3, vs_half2(), ps_constant());
    const auto px = f.read();
    CHECK(at(px, 2, 2) == kGreen);
    CHECK(at(px, 20, 5) == kGreen);
    CHECK(at(px, 29, 29) == 0);
    CHECK(f.fatals.empty());
  }
  CHECK(vk_errors.errors().empty());
}

TEST_CASE("16-bit indices with 8in16 swap", "[unit][vulkan][draw]") {
  ValidationErrors vk_errors;
  {
    Fixture f;
    if (!f.ok) SKIP("no Vulkan ICD installed");
    f.vertices({vert(-1, 1), vert(1, 1), vert(-1, -1), vert(1, -1)});
    // Indices 3, 1, 2 big-endian: the bottom-right half (BR, TR, BL).
    f.draw_indexed(vk::prim::kTriangleList, {0x00, 0x03, 0x00, 0x01, 0x00, 0x02}, 2, pm4::kEndian8in16,
                   vs_position_color(), ps_interpolated());
    const auto px = f.read();
    CHECK(at(px, 29, 29) == kRed);
    CHECK(at(px, 25, 10) == kRed);
    CHECK(at(px, 2, 2) == 0);
    CHECK(f.fatals.empty());
  }
  CHECK(vk_errors.errors().empty());
}

TEST_CASE("32-bit indices with VGT_INDX_OFFSET reach r0.x", "[unit][vulkan][draw]") {
  ValidationErrors vk_errors;
  {
    Fixture f;
    if (!f.ok) SKIP("no Vulkan ICD installed");
    // Vertices 0..3 cover the top-left half, 4..6 the bottom-right half; indices 0, 1, 2 + offset 4 use 4..6.
    f.vertices({vert(-1, 1), vert(1, 1), vert(-1, -1), vert(-1, -1), vert(1, 1), vert(1, -1), vert(-1, -1)});
    f.regs.write(gpu::VGT_INDX_OFFSET, 4);
    f.draw_indexed(vk::prim::kTriangleList, {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 2}, 4, pm4::kEndian8in32,
                   vs_position_color(), ps_interpolated());
    const auto px = f.read();
    CHECK(at(px, 29, 29) == kRed);
    CHECK(at(px, 2, 2) == 0);
    CHECK(f.fatals.empty());
  }
  CHECK(vk_errors.errors().empty());
}

TEST_CASE("reset index becomes primitive restart on a strip", "[unit][vulkan][draw]") {
  ValidationErrors vk_errors;
  {
    Fixture f;
    if (!f.ok) SKIP("no Vulkan ICD installed");
    // Strip 0, 1, 2 (top-left corner), reset, strip 3, 4, 5 (bottom-right corner). Without the restart the strip
    // would also draw (1, 2, 3), which covers the centre-left of the target.
    f.vertices({vert(-1, 1), vert(-1, 0), vert(0, 1), vert(0, -1), vert(1, 0), vert(1, -1)});
    f.regs.write(gpu::PA_SU_SC_MODE_CNTL, 1u << 21);  // multi_prim_ib_ena
    f.regs.write(gpu::VGT_MULTI_PRIM_IB_RESET_INDX, 0xFFFF);
    f.draw_indexed(vk::prim::kTriangleStrip, {0, 0, 0, 1, 0, 2, 0xFF, 0xFF, 0, 3, 0, 4, 0, 5}, 2, pm4::kEndian8in16,
                   vs_position_color(), ps_interpolated());
    const auto px = f.read();
    CHECK(at(px, 2, 2) == kRed);
    CHECK(at(px, 29, 29) == kRed);
    CHECK(at(px, 11, 16) == 0);
    CHECK(f.fatals.empty());
  }
  CHECK(vk_errors.errors().empty());
}

TEST_CASE("indices outside min and max are clamped", "[unit][vulkan][draw]") {
  ValidationErrors vk_errors;
  {
    Fixture f;
    if (!f.ok) SKIP("no Vulkan ICD installed");
    // Index 9 is past the three vertices (it would fetch zeros: a degenerate triangle); clamped to max 2 it is the
    // third corner of the top-left half.
    f.vertices({vert(-1, 1), vert(1, 1), vert(-1, -1)});
    f.regs.write(gpu::VGT_MAX_VTX_INDX, 2);
    f.draw_indexed(vk::prim::kTriangleList, {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 9}, 4, pm4::kEndian8in32,
                   vs_position_color(), ps_interpolated());
    const auto px = f.read();
    CHECK(at(px, 2, 2) == kRed);
    CHECK(at(px, 29, 29) == 0);
    CHECK(f.fatals.empty());
  }
  CHECK(vk_errors.errors().empty());
}

TEST_CASE("depth-only draw has no fragment stage", "[unit][vulkan][draw]") {
  ValidationErrors vk_errors;
  {
    Fixture f;
    if (!f.ok) SKIP("no Vulkan ICD installed");
    f.vertices(fullscreen({1, 0, 0, 1}, 0.5f));
    f.regs.write(gpu::RB_MODECONTROL, 5);                                     // depth only
    f.regs.write(gpu::RB_DEPTHCONTROL, (1u << 1) | (1u << 2) | (7u << 4));  // z on, z write, always
    f.draw(vk::prim::kTriangleList, 3, vs_position_color(), ps_interpolated());  // the PS is ignored
    INFO((f.fatals.empty() ? std::string() : f.fatals.front()));
    REQUIRE(f.fatals.empty());
    const vk::DrawStats s = f.backend->draw_stats();
    CHECK(s.draws == 1);
    CHECK_FALSE(s.last_fragment_stage);
    const auto depth = f.read(true);
    const uint32_t d24 = at(depth, 16, 16) >> 8;  // the EDRAM depth word: d24 << 8 | stencil
    INFO(hex(at(depth, 16, 16)));
    CHECK(std::abs(int64_t(d24) - 0x800000) <= 2);
  }
  CHECK(vk_errors.errors().empty());
}

TEST_CASE("sampler state from the fetch constant reaches the shader", "[unit][vulkan][draw]") {
  ValidationErrors vk_errors;
  {
    Fixture f;
    if (!f.ok) SKIP("no Vulkan ICD installed");
    // A 2x1 k_8_8_8_8 linear texture (8in32): texel 0 red, texel 1 green. Sampled at u = 1.25 (point filter): wrap
    // reads texel 0, clamp texel 1.
    f.guest.put(kTextureBase, be_words({kRed, kGreen}));
    const uint32_t clamp = GENERATE(0u /* repeat */, 2u /* clamp to edge */);
    const uint32_t tf = gpu::kShaderConstantFetchBase;  // texture fetch constant 0
    f.regs.write(tf + 0, 2u | (clamp << 10) | (clamp << 13) | (1u << 22));  // type 2, pitch 32 texels, linear
    f.regs.write(tf + 1, 6u | (pm4::kEndian8in32 << 6) | kTextureBase);     // k_8_8_8_8
    f.regs.write(tf + 2, (2u - 1) | ((1u - 1) << 13));                        // 2 x 1
    f.regs.write(tf + 3, (0u | (1u << 3) | (2u << 6) | (3u << 9)) << 1);     // swizzle XYZW, point filters
    f.regs.write(tf + 4, 0);
    f.regs.write(tf + 5, 1u << 9);  // 2D
    f.vertices(fullscreen());
    f.ps_constant(0, {1.25f, 0.5f, 0.0f, 0.0f});
    f.draw(vk::prim::kTriangleList, 3, vs_position_color(), ps_texture());
    INFO((f.fatals.empty() ? std::string() : f.fatals.front()));
    REQUIRE(f.fatals.empty());
    const uint32_t p = at(f.read(), 16, 16);
    INFO("clamp " << clamp << " " << hex(p));
    CHECK(p == (clamp == 0 ? kRed : kGreen));
  }
  CHECK(vk_errors.errors().empty());
}

TEST_CASE("alpha test greater-equal discards", "[unit][vulkan][draw]") {
  ValidationErrors vk_errors;
  {
    Fixture f;
    if (!f.ok) SKIP("no Vulkan ICD installed");
    f.clear(kBlue);
    f.vertices(fullscreen());
    f.regs.write(gpu::RB_COLORCONTROL, 6u | (1u << 3));  // alpha test on, greater-or-equal
    f.set_float(gpu::RB_ALPHA_REF, 0.5f);
    f.ps_constant(0, {1, 0, 0, 0.25f});
    f.draw(vk::prim::kTriangleList, 3, vs_position_color(), ps_constant());
    CHECK(at(f.read(), 16, 16) == kBlue);  // 0.25 >= 0.5 fails: discarded
    f.ps_constant(0, {1, 0, 0, 0.75f});
    f.draw(vk::prim::kTriangleList, 3, vs_position_color(), ps_constant());
    const uint32_t p = at(f.read(), 16, 16);
    INFO(hex(p));
    CHECK(channel(p, 0) == 0xFF);
    CHECK(channel(p, 2) == 0);
    CHECK(f.fatals.empty());
  }
  CHECK(vk_errors.errors().empty());
}

TEST_CASE("ndc transform matches the D3D pixel center", "[unit][vulkan][draw]") {
  ValidationErrors vk_errors;
  {
    Fixture f;
    if (!f.ok) SKIP("no Vulkan ICD installed");
    // A full-height band from window x = 0.25 to 4.25. With the Direct3D pixel centre (pix_center 0) pixel i is
    // at x = i, so pixels 1..4 are covered; with pix_center 1 (centres at .5) pixels 0..3.
    const float x0 = (0.25f - 16.0f) / 16.0f, x1 = (4.25f - 16.0f) / 16.0f;
    f.vertices({vert(x0, 1), vert(x1, 1), vert(x0, -1), vert(x1, 1), vert(x1, -1), vert(x0, -1)});
    const uint32_t pix_center = GENERATE(0u, 1u);
    f.regs.write(gpu::PA_SU_VTX_CNTL, pix_center);
    f.draw(vk::prim::kTriangleList, 6, vs_position_color(), ps_interpolated());
    const auto px = f.read();
    INFO("pix_center " << pix_center);
    CHECK(at(px, 0, 16) == (pix_center == 0 ? 0u : kRed));
    CHECK(at(px, 2, 16) == kRed);
    CHECK(at(px, 4, 16) == (pix_center == 0 ? kRed : 0u));
    CHECK(at(px, 5, 16) == 0);
    CHECK(f.fatals.empty());
  }
  CHECK(vk_errors.errors().empty());
}

TEST_CASE("pipeline is reused for the same state", "[unit][vulkan][draw]") {
  ValidationErrors vk_errors;
  {
    Fixture f;
    if (!f.ok) SKIP("no Vulkan ICD installed");
    f.vertices(fullscreen());
    f.draw(vk::prim::kTriangleList, 3, vs_position_color(), ps_interpolated());
    f.draw(vk::prim::kTriangleList, 3, vs_position_color(), ps_interpolated());
    CHECK(f.backend->draw_stats().draws == 2);
    CHECK(f.backend->draw_stats().pipelines == 1);
    f.regs.write(gpu::RB_BLENDCONTROL0, 6u | (7u << 8) | (1u << 16));  // other state: a second pipeline
    f.draw(vk::prim::kTriangleList, 3, vs_position_color(), ps_interpolated());
    CHECK(f.backend->draw_stats().pipelines == 2);
    f.regs.write(gpu::RB_BLENDCONTROL0, 0x00010001u);  // back to the first state
    f.draw(vk::prim::kTriangleList, 3, vs_position_color(), ps_interpolated());
    CHECK(f.backend->draw_stats().pipelines == 2);
    CHECK(at(f.read(), 16, 16) == kRed);
    CHECK(f.fatals.empty());
  }
  CHECK(vk_errors.errors().empty());
}

TEST_CASE("untranslatable shader is fatal with the Vulkan backend", "[unit][vulkan][draw]") {
  ValidationErrors vk_errors;
  {
    lr::testing::KernelFixture f;
    GpuRig rig(f);
    if (!rig.ok) SKIP("no Vulkan ICD installed");
    lr::testing::Pm4Builder b;
    put_registers(b, default_state());
    im_load_immediate(b, ShaderStage::Vertex, vs_position_color());
    im_load_immediate(b, ShaderStage::Pixel, ps_memexport());
    b.type0(gpu::RB_MODECONTROL, {4});
    b.type3(pm4::DRAW_INDX_2, {4u | (2u << 6) | (3u << 16)});
    rig.run(b);
    const std::string fatal = rig.fatal();
    INFO(fatal);
    CHECK(fatal.find("(pixel, 6 dwords) cannot be translated") != std::string::npos);
    CHECK(fatal.find("memexport") != std::string::npos);
  }
  CHECK(vk_errors.errors().empty());
}

TEST_CASE("vertex fetch constant at an unallocated physical address is fatal", "[unit][vulkan][draw]") {
  ValidationErrors vk_errors;
  {
    Fixture f;
    if (!f.ok) SKIP("no Vulkan ICD installed");
    f.set(vertex_fetch(kAllocEnd + 0x80000, 24));  // in guest memory, in no physical allocation
    f.draw(vk::prim::kTriangleList, 3, vs_position_color(), ps_interpolated());
    REQUIRE(f.fatals.size() == 1);
    INFO(f.fatals.front());
    CHECK(f.fatals.front().find("vertex fetch constant 3") != std::string::npos);
    CHECK(f.fatals.front().find(hex(kAllocEnd + 0x80000)) != std::string::npos);
    CHECK(f.fatals.front().find("is not in a physical allocation") != std::string::npos);
    CHECK(f.backend->draw_stats().draws == 0);
    // After a fatal the backend does no more work.
    f.vertices(fullscreen());
    f.draw(vk::prim::kTriangleList, 3, vs_position_color(), ps_interpolated());
    CHECK(f.backend->draw_stats().draws == 0);
    CHECK(f.fatals.size() == 1);
  }
  CHECK(vk_errors.errors().empty());
}

TEST_CASE("reset index in a list is fatal", "[unit][vulkan][draw]") {
  ValidationErrors vk_errors;
  {
    Fixture f;
    if (!f.ok) SKIP("no Vulkan ICD installed");
    f.vertices({vert(-1, 1), vert(1, 1), vert(-1, -1)});
    f.regs.write(gpu::PA_SU_SC_MODE_CNTL, 1u << 21);  // multi_prim_ib_ena
    f.regs.write(gpu::VGT_MULTI_PRIM_IB_RESET_INDX, 0xFFFF);
    f.draw_indexed(vk::prim::kTriangleList, {0, 0, 0, 1, 0xFF, 0xFF, 0, 2}, 2, pm4::kEndian8in16, vs_position_color(),
                   ps_interpolated());
    REQUIRE(f.fatals.size() == 1);
    INFO(f.fatals.front());
    CHECK(f.fatals.front().find("GPU draw: primitive reset index 0xFFFF in a triangle list") != std::string::npos);
    CHECK(f.backend->draw_stats().draws == 0);
  }
  CHECK(vk_errors.errors().empty());
}

TEST_CASE("rect list with the right angle at any vertex", "[unit][vulkan][draw]") {
  ValidationErrors vk_errors;
  {
    Fixture f;
    if (!f.ok) SKIP("no Vulkan ICD installed");
    // The same rectangle (TL black, TR red, BL green; the right angle at TL), its corners rotated so the right angle
    // is vertex 0, 1 or 2: the fourth corner is always BR, yellow.
    const std::vector<Vertex> corners = {vert(-0.5f, 0.5f, {0, 0, 0, 1}), vert(0.5f, 0.5f, {1, 0, 0, 1}),
                                         vert(-0.5f, -0.5f, {0, 1, 0, 1})};
    const uint32_t right_angle = GENERATE(0u, 1u, 2u);
    std::vector<Vertex> vs(3);
    for (uint32_t i = 0; i < 3; ++i) vs[(i + right_angle) % 3] = corners[i];
    f.vertices(vs);
    f.draw(vk::prim::kRectangleList, 3, vs_position_color(), ps_interpolated());
    INFO("right angle at vertex " << right_angle);
    REQUIRE(f.fatals.empty());
    const auto px = f.read();
    const uint32_t br = at(px, 23, 23);
    INFO(hex(br));
    CHECK(channel(br, 0) > 200);
    CHECK(channel(br, 1) > 200);
    CHECK(channel(br, 2) == 0);
    const uint32_t tl = at(px, 8, 8);
    CHECK(channel(tl, 0) < 40);
    CHECK(channel(tl, 1) < 40);
    CHECK(at(px, 26, 26) == 0);
    CHECK(at(px, 5, 5) == 0);
  }
  CHECK(vk_errors.errors().empty());
}

TEST_CASE("a backend fatal reaches GpuHooks::fatal through GpuSystem", "[unit][vulkan][draw]") {
  ValidationErrors vk_errors;
  {
    lr::testing::KernelFixture f;
    GpuRig rig(f);
    if (!rig.ok) SKIP("no Vulkan ICD installed");
    constexpr uint32_t kUnallocated = 0x1F000000;  // physical, never allocated by the fixture
    lr::testing::Pm4Builder b;
    put_registers(b, default_state());
    put_registers(b, vertex_fetch(kUnallocated, 24));
    im_load_immediate(b, ShaderStage::Vertex, vs_position_color());
    im_load_immediate(b, ShaderStage::Pixel, ps_interpolated());
    b.type0(gpu::RB_MODECONTROL, {4});
    b.type3(pm4::DRAW_INDX_2, {4u | (2u << 6) | (3u << 16)});
    rig.run(b);
    const std::string fatal = rig.fatal();
    INFO(fatal);
    CHECK(fatal.find("GPU draw: vertex fetch constant 3: base physical address " + hex(kUnallocated)) !=
          std::string::npos);
    std::lock_guard<std::mutex> lock(rig.m);
    CHECK(rig.fatals.size() == 1);
  }
  CHECK(vk_errors.errors().empty());
}
