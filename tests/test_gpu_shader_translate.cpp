// SPDX-License-Identifier: MIT
// The plan B translator skeleton (runtime/gpu/shader/translator.h): the stage interface, the runtime contract
// (contract.h) and the exports. Shaders are synthetic microcode (support/xenos_shader_builder.h); every module goes
// through spirv-val, and the value tests run on lavapipe through support/vk_shader_harness.h.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "runtime/gpu/shader/contract.h"
#include "runtime/gpu/shader/ir.h"
#include "runtime/gpu/shader/translator.h"
#include "support/spirv_tools.h"
#include "support/vk_shader_harness.h"
#include "support/xenos_shader_builder.h"

namespace sh = lr::gpu::shader;
namespace xe = lr::testing::xenos;
using lr::gpu::ShaderStage;
using lr::testing::HarnessInputs;
using lr::testing::HarnessResult;
using lr::testing::ShaderHarness;
using Pixel = std::array<float, 4>;

namespace {

// --- microcode ----------------------------------------------------------------------------------------------------

constexpr uint32_t kXXXX = 0x6C;  // source swizzle .xxxx (component i reads ((s >> 2i) + i) & 3)

xe::AluSrc r(uint32_t reg, uint32_t swizzle = 0) { return {reg, true, swizzle}; }
xe::AluSrc c(uint32_t reg, uint32_t swizzle = 0) { return {reg, false, swizzle}; }

xe::Alu vector_op(uint32_t op, uint32_t dest, xe::AluSrc a, xe::AluSrc b, bool to_export) {
  xe::Alu alu;
  alu.vector_op = op;
  alu.vector_dest = dest;
  alu.export_data = to_export;
  alu.src1 = a;
  alu.src2 = b;
  return alu;
}
xe::Alu mov(uint32_t dest, xe::AluSrc s, bool to_export = false) { return vector_op(xe::kVectorMax, dest, s, s, to_export); }
xe::Alu add(uint32_t dest, xe::AluSrc a, xe::AluSrc b, bool to_export = false) {
  return vector_op(xe::kVectorAdd, dest, a, b, to_export);
}
xe::Alu mul(uint32_t dest, xe::AluSrc a, xe::AluSrc b, bool to_export = false) {
  return vector_op(xe::kVectorMul, dest, a, b, to_export);
}

// The ALU instructions run straight-line: exec clauses of up to 6, the last one exec_end.
std::vector<uint32_t> straight_line(const std::vector<xe::Alu>& alus) {
  const auto n = static_cast<uint32_t>(alus.size());
  const uint32_t clauses = std::max<uint32_t>(1, (n + 5) / 6);
  const uint32_t cf_slots = (clauses + 1) / 2;
  std::vector<uint64_t> cf;
  for (uint32_t k = 0; k < clauses; ++k) {
    const uint32_t first = k * 6;
    cf.push_back(xe::cf_exec(k + 1 == clauses, cf_slots + first, std::min<uint32_t>(6, n - first), 0));
  }
  if (cf.size() % 2) cf.push_back(xe::cf_nop());
  std::vector<uint32_t> code;
  for (size_t i = 0; i < cf.size(); i += 2) xe::emit(code, xe::cf_pair(cf[i], cf[i + 1]));
  for (const xe::Alu& a : alus) xe::emit(code, xe::alu(a));
  return code;
}

// A VS position that depends on r0.x = i: export 62 = c0 + c1 * i + c2 * i * i (temporaries r1, r2). Callers put
// their own constants from c3 and temporaries from r3.
std::vector<xe::Alu> position_alus() {
  return {mul(1, r(0, kXXXX), c(1)), mul(2, r(0, kXXXX), r(0, kXXXX)), mul(2, r(2), c(2)), add(1, r(1), r(2)),
          add(sh::kExportVsPosition, r(1), c(0), true)};
}

// c0..c2 for position_alus() so that vertices 0, 1, 2 land at p0, p1, p2.
std::vector<Pixel> position_constants(const Pixel& p0, const Pixel& p1, const Pixel& p2) {
  Pixel a{}, b{}, q{};
  for (int k = 0; k < 4; ++k) {
    q[k] = (p2[k] - 2 * p1[k] + p0[k]) / 2;
    b[k] = p1[k] - p0[k] - q[k];
    a[k] = p0[k];
  }
  return {a, b, q};
}

// One triangle covering the whole target (as fullscreen_triangle_vs), at depth z / w.
std::vector<Pixel> fullscreen(float z = 0.0f, float w = 1.0f) {
  return position_constants({-w, -w, z, w}, {3 * w, -w, z, w}, {-w, 3 * w, z, w});
}

// Vertex i of a point list at the centre of pixel i of a `width` x 1 target.
std::vector<Pixel> points(uint32_t width) {
  const float s = 2.0f / static_cast<float>(width);
  return {Pixel{s / 2 - 1, 0, 0, 1}, Pixel{s, 0, 0, 0}, Pixel{0, 0, 0, 0}};
}

std::vector<uint32_t> vs_code(std::vector<xe::Alu> extra = {}) {
  std::vector<xe::Alu> alus = position_alus();
  alus.insert(alus.end(), extra.begin(), extra.end());
  return straight_line(alus);
}

std::vector<uint32_t> ps_code(std::vector<xe::Alu> alus) { return straight_line(alus); }

// --- translation and checks ---------------------------------------------------------------------------------------

void check_valid_if_possible(const std::vector<uint32_t>& module) {
  if (!lr::testing::spirv_tools_available()) return;
  if (auto err = lr::testing::spirv_val(module)) FAIL("spirv-val: " << *err << "\n" << lr::testing::spirv_dis(module));
}

void require_valid(const std::vector<uint32_t>& module) {
  REQUIRE_FALSE(module.empty());
  if (!lr::testing::spirv_tools_available()) SKIP("spirv-val/spirv-dis not installed (Arch: spirv-tools)");
  if (auto err = lr::testing::spirv_val(module)) FAIL("spirv-val: " << *err << "\n" << lr::testing::spirv_dis(module));
}

lr::Result<sh::TranslatedShader> try_translate(ShaderStage stage, const std::vector<uint32_t>& code,
                                               const sh::TranslateOptions& options = {}) {
  auto program = sh::decode(stage, code);
  if (!program) return program.error();
  return sh::translate(program.value(), options);
}

sh::TranslatedShader translate_ok(ShaderStage stage, const std::vector<uint32_t>& code,
                                  const sh::TranslateOptions& options = {}) {
  auto t = try_translate(stage, code, options);
  if (!t) FAIL("translate: " << t.error().message);
  check_valid_if_possible(t.value().spirv);
  return std::move(t.value());
}

ShaderHarness make_harness() {
  auto h = ShaderHarness::create();
  if (!h) {
    if (h.error().code == lr::ErrorCode::ToolMissing) SKIP("no Vulkan ICD: " << h.error().message);
    FAIL("harness: " << h.error().message);
  }
  return std::move(h.value());
}

HarnessResult draw_ok(ShaderHarness& h, const sh::TranslatedShader& vs, const sh::TranslatedShader& ps,
                      const HarnessInputs& in) {
  auto res = h.draw(vs.spirv, ps.spirv, in);
  if (!res) FAIL("draw: " << res.error().message);
  return std::move(res.value());
}

uint32_t bits(float f) { return std::bit_cast<uint32_t>(f); }

std::string pixel_text(const Pixel& p) {
  std::ostringstream s;
  s << "(" << p[0] << ", " << p[1] << ", " << p[2] << ", " << p[3] << ")";
  return s.str();
}

std::vector<std::string> lines_of(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) out.push_back(line);
  return out;
}

// The id a line "OpDecorate %<id> <decoration>" names, or "".
std::string decorated_id(const std::string& dis, const std::string& decoration) {
  for (const std::string& line : lines_of(dis)) {
    const auto at = line.find("OpDecorate %");
    if (at == std::string::npos || line.find(" " + decoration) == std::string::npos) continue;
    const auto start = at + 11;
    return line.substr(start, line.find(' ', start) - start);
  }
  return "";
}

// The PS that copies r<reg> to oC0.
sh::TranslatedShader ps_copy(uint32_t reg, const sh::TranslateOptions& options = {}) {
  return translate_ok(ShaderStage::Pixel, ps_code({mov(0, r(reg), true)}), options);
}

}  // namespace

// --- executed on lavapipe -----------------------------------------------------------------------------------------

TEST_CASE("vs exports position and interpolator 0 from constants", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  const auto vs = translate_ok(ShaderStage::Vertex, vs_code({mov(0, c(3), true)}));
  const auto ps = ps_copy(0);
  HarnessInputs in;
  in.vs_constants = fullscreen();
  in.vs_constants.push_back({0.25f, 0.5f, 0.75f, 1.0f});
  const HarnessResult res = draw_ok(h, vs, ps, in);
  REQUIRE(res.color[0].size() == 4);
  for (const Pixel& p : res.color[0]) CHECK(p == Pixel{0.25f, 0.5f, 0.75f, 1.0f});
}

TEST_CASE("ndc scale and offset are applied to the position", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  const auto vs = translate_ok(ShaderStage::Vertex, vs_code());
  const auto ps = translate_ok(ShaderStage::Pixel, ps_code({mov(0, c(0), true)}));
  HarnessInputs in;
  in.width = 8;
  in.depth_target = true;
  in.vs_constants = fullscreen(0.5f);
  in.ps_constants = {{1.0f, 1.0f, 1.0f, 1.0f}};
  // x' = 0.25x - 0.75 squeezes the triangle into x <= 0 (its edge crosses the pixel row at x = -0.25: pixels 0..2);
  // z' = 0.5z + 0.25 = 0.5.
  in.ndc_scale = {0.25f, 1.0f, 0.5f, 1.0f};
  in.ndc_offset = {-0.75f, 0.0f, 0.25f, 0.0f};
  const HarnessResult res = draw_ok(h, vs, ps, in);
  REQUIRE(res.color[0].size() == 8);
  REQUIRE(res.depth.size() == 8);
  for (uint32_t i = 0; i < 8; ++i) {
    INFO("pixel " << i << " " << pixel_text(res.color[0][i]) << " depth " << res.depth[i]);
    const bool covered = i < 3;
    CHECK(res.color[0][i] == (covered ? Pixel{1, 1, 1, 1} : Pixel{0, 0, 0, 0}));
    CHECK(res.depth[i] == (covered ? 0.5f : 1.0f));
  }
}

TEST_CASE("VTE flags undo the divide by W before the ndc transform", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  const auto vs = translate_ok(ShaderStage::Vertex, vs_code());
  const auto ps = translate_ok(ShaderStage::Pixel, ps_code({mov(0, c(0), true)}));
  HarnessInputs in;
  in.depth_target = true;
  in.ps_constants = {{1.0f, 1.0f, 1.0f, 1.0f}};
  // The clip position (x, y, 0.5, 2) exported as the Xenos does with every VTE format bit clear:
  // (x / w, y / w, z / w, 1 / w) = (x / 2, y / 2, 0.25, 0.5).
  in.vs_constants = position_constants({-1, -1, 0.25f, 0.5f}, {3, -1, 0.25f, 0.5f}, {-1, 3, 0.25f, 0.5f});

  SECTION("divided xy and z, W given as 1/W: the shader restores (x, y, 0.5, 2), depth 0.25") {
    in.vte_flags = sh::kVteXyDividedByW | sh::kVteZDividedByW;
    const HarnessResult res = draw_ok(h, vs, ps, in);
    for (uint32_t i = 0; i < in.width; ++i) {
      INFO("pixel " << i);
      CHECK(res.color[0][i] == Pixel{1, 1, 1, 1});
      CHECK(res.depth[i] == 0.25f);
    }
  }
  SECTION("the same export taken as is (W is W): depth 0.25 / 0.5") {
    in.vte_flags = sh::kVteW0NotReciprocal;
    const HarnessResult res = draw_ok(h, vs, ps, in);
    for (uint32_t i = 0; i < in.width; ++i) CHECK(res.depth[i] == 0.5f);
  }
}

TEST_CASE("r0.x is the vertex index plus the index offset, clamped", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // Points: vertex index v draws at pixel clamp((v + offset) & 0xFFFFFF, 2, 3) the value r0. With offset 2^24 + 1 the
  // sum passes 2^24 and the 24-bit mask leaves v + 1, as with offset 1.
  const auto vs = translate_ok(ShaderStage::Vertex, vs_code({mov(0, r(0), true)}));
  const auto ps = ps_copy(0);
  HarnessInputs in;
  in.width = 6;
  in.vertex_count = 6;
  in.topology = lr::testing::HarnessTopology::PointList;
  in.vs_constants = points(6);
  in.min_index = 2;
  in.max_index = 3;
  for (const int32_t offset : {1, 0x1000001}) {
    in.index_offset = offset;
    const HarnessResult res = draw_ok(h, vs, ps, in);
    REQUIRE(res.color[0].size() == 6);
    for (uint32_t i = 0; i < 6; ++i) {
      INFO("offset " << offset << " pixel " << i << " " << pixel_text(res.color[0][i]));
      const float expected = (i == 2 || i == 3) ? static_cast<float>(i) : 0.0f;
      CHECK(res.color[0][i] == Pixel{expected, 0, 0, 0});
    }
  }
}

TEST_CASE("ps reads interpolator 5 by register index", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  const auto vs = translate_ok(ShaderStage::Vertex, vs_code({mov(0, c(3), true), mov(5, c(4), true)}));
  const auto ps = ps_copy(5);
  CHECK(ps.info.interpolators_read == (1u << 5));
  HarnessInputs in;
  in.vs_constants = fullscreen();
  in.vs_constants.push_back({9, 9, 9, 9});
  in.vs_constants.push_back({0.5f, -1.5f, 2.5f, 8.0f});
  const HarnessResult res = draw_ok(h, vs, ps, in);
  for (const Pixel& p : res.color[0]) CHECK(p == Pixel{0.5f, -1.5f, 2.5f, 8.0f});
}

TEST_CASE("ps registers past the interpolators start at zero", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  const auto vs = translate_ok(ShaderStage::Vertex, vs_code({mov(0, c(3), true)}));
  const auto ps = translate_ok(ShaderStage::Pixel, ps_code({add(0, r(20), c(0), true)}));
  CHECK(ps.info.interpolators_read == 0);
  HarnessInputs in;
  in.vs_constants = fullscreen();
  in.vs_constants.push_back({100, 100, 100, 100});
  in.ps_constants = {{1, 2, 3, 4}};
  const HarnessResult res = draw_ok(h, vs, ps, in);
  for (const Pixel& p : res.color[0]) CHECK(p == Pixel{1, 2, 3, 4});
}

TEST_CASE("ps writes oC1 and depth", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  const auto vs = translate_ok(ShaderStage::Vertex, vs_code());
  const auto ps =
      translate_ok(ShaderStage::Pixel, ps_code({mov(1, c(0), true), mov(sh::kExportPsDepth, c(1), true)}));
  CHECK(ps.info.colors_written == 2);
  CHECK(ps.info.writes_depth);
  HarnessInputs in;
  in.width = 2;
  in.color_targets = 2;
  in.depth_target = true;
  in.vs_constants = fullscreen();
  in.ps_constants = {{0.25f, -2.0f, 4.0f, 1.0f}, {0.375f, 9.0f, 9.0f, 9.0f}};
  const HarnessResult res = draw_ok(h, vs, ps, in);
  REQUIRE(res.color[1].size() == 2);
  REQUIRE(res.depth.size() == 2);
  for (uint32_t i = 0; i < 2; ++i) {
    CHECK(res.color[1][i] == Pixel{0.25f, -2.0f, 4.0f, 1.0f});
    CHECK(res.depth[i] == 0.375f);
  }
}

TEST_CASE("param_gen encodes position, back-facing and point flags", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  const auto vs = translate_ok(ShaderStage::Vertex, vs_code());
  HarnessInputs in;
  in.width = 4;

  SECTION("triangles: x = pixel x, sign bit when back-facing; y = pixel y; z = w = 0") {
    sh::TranslateOptions options;
    options.ps_param_gen = 2;
    const auto ps = ps_copy(2, options);
    CHECK(ps.info.interpolators_read == 0);  // the param_gen register is not an input
    // Counter-clockwise front faces in framebuffer coordinates (y down): the vertex order of fullscreen() has a
    // negative signed area, so it is back-facing; swapping two vertices makes it front-facing.
    const Pixel p0{-1, -1, 0, 1}, p1{3, -1, 0, 1}, p2{-1, 3, 0, 1};
    for (const bool back : {true, false}) {
      in.vs_constants = back ? position_constants(p0, p1, p2) : position_constants(p0, p2, p1);
      const HarnessResult res = draw_ok(h, vs, ps, in);
      for (uint32_t i = 0; i < 4; ++i) {
        const Pixel& p = res.color[0][i];
        INFO((back ? "back" : "front") << " pixel " << i << " " << pixel_text(p));
        CHECK(bits(p[0]) == (bits(static_cast<float>(i)) | (back ? 0x80000000u : 0u)));
        CHECK(bits(p[1]) == 0u);
        CHECK(bits(p[2]) == 0u);
        CHECK(bits(p[3]) == 0u);
      }
    }
  }
  SECTION("points: sign bit of y set, z and w the point coordinate") {
    sh::TranslateOptions options;
    options.ps_param_gen = 2;
    options.ps_point = true;
    const auto ps = ps_copy(2, options);
    in.topology = lr::testing::HarnessTopology::PointList;
    in.vertex_count = 4;
    in.vs_constants = points(4);
    const HarnessResult res = draw_ok(h, vs, ps, in);
    for (uint32_t i = 0; i < 4; ++i) {
      const Pixel& p = res.color[0][i];
      INFO("pixel " << i << " " << pixel_text(p));
      CHECK(bits(p[0]) == bits(static_cast<float>(i)));  // points are front-facing
      CHECK(bits(p[1]) == 0x80000000u);                 // -0.0: row 0 with the point flag
      CHECK(p[2] == 0.5f);
      CHECK(p[3] == 0.5f);
    }
  }
}

TEST_CASE("flat interpolator is not interpolated", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // Interpolator 0 = 7 + 10 * vertex index: 7, 17, 27 at the three vertices.
  const auto vs = translate_ok(ShaderStage::Vertex, vs_code({mul(3, r(0, kXXXX), c(3)), add(0, r(3), c(4), true)}));
  HarnessInputs in;
  in.vs_constants = fullscreen();
  in.vs_constants.push_back({10, 10, 10, 10});
  in.vs_constants.push_back({7, 7, 7, 7});

  sh::TranslateOptions flat;
  flat.flat_mask = 1;
  const HarnessResult res = draw_ok(h, vs, ps_copy(0, flat), in);
  for (const Pixel& p : res.color[0]) CHECK(p == Pixel{7, 7, 7, 7});  // the provoking (first) vertex

  const HarnessResult smooth = draw_ok(h, vs, ps_copy(0), in);
  CHECK(smooth.color[0][0] != smooth.color[0][3]);  // the same input, interpolated, varies along the row
}

TEST_CASE("alpha test greater-equal discards", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  const auto vs = translate_ok(ShaderStage::Vertex, vs_code());
  const auto ps = translate_ok(ShaderStage::Pixel, ps_code({mov(0, c(0), true)}));
  HarnessInputs in;
  in.vs_constants = fullscreen();
  in.alpha_ref = 0.5f;

  SECTION("alpha >= ref passes, the rest is discarded; a disabled test keeps everything") {
    in.spec_alpha = sh::kAlphaTestEnable | (6u << sh::kAlphaTestFuncShift);  // pass when alpha >= ref
    for (const float alpha : {0.25f, 0.5f, 0.75f}) {
      in.ps_constants = {{1, 1, 1, alpha}};
      const HarnessResult res = draw_ok(h, vs, ps, in);
      INFO("alpha " << alpha);
      const Pixel expected = alpha >= 0.5f ? Pixel{1, 1, 1, alpha} : Pixel{0, 0, 0, 0};
      for (const Pixel& p : res.color[0]) CHECK(p == expected);
    }
    in.spec_alpha = 6u << sh::kAlphaTestFuncShift;  // the same function, test disabled
    in.ps_constants = {{1, 1, 1, 0.25f}};
    const HarnessResult disabled = draw_ok(h, vs, ps, in);
    for (const Pixel& p : disabled.color[0]) CHECK(p == Pixel{1, 1, 1, 0.25f});
  }
  SECTION("a NaN alpha passes only not-equal (5) and always (7)") {
    in.ps_constants = {{1, 1, 1, std::numeric_limits<float>::quiet_NaN()}};
    for (uint32_t func = 0; func < 8; ++func) {
      in.spec_alpha = sh::kAlphaTestEnable | (func << sh::kAlphaTestFuncShift);
      const HarnessResult res = draw_ok(h, vs, ps, in);
      INFO("function " << func);
      const bool pass = func == 5 || func == 7;
      for (const Pixel& p : res.color[0]) {
        CHECK(p[0] == (pass ? 1.0f : 0.0f));  // a discarded pixel keeps the clear colour
        CHECK(std::isnan(p[3]) == pass);
      }
    }
  }
}

TEST_CASE("zero-mask exports are skipped", "[unit]") {
  // Exports with every resolved mask 0 write nothing, even to a register the stage does not have (62 in a PS) or
  // to eA; they are neither errors nor memexport.
  xe::Alu to_position = mov(sh::kExportVsPosition, c(0), true);
  to_position.vector_mask = 0;
  xe::Alu to_ea = mov(sh::kExportAddress, c(0), true);
  to_ea.vector_mask = 0;
  const auto ps = translate_ok(ShaderStage::Pixel, ps_code({to_position, to_ea, mov(0, c(1), true)}));
  CHECK(ps.info.colors_written == 1);
  require_valid(ps.spirv);
}

// --- checked with spirv-dis ---------------------------------------------------------------------------------------

TEST_CASE("vs declares all 16 interpolator outputs and a point size of 1", "[unit]") {
  // Writes only interpolator 3 and a point size (dropped).
  const auto vs = translate_ok(ShaderStage::Vertex, vs_code({mov(3, c(3), true), mov(sh::kExportVsPointSize, c(3), true)}));
  CHECK(vs.info.drops_point_size);
  require_valid(vs.spirv);
  const std::string dis = lr::testing::spirv_dis(vs.spirv);
  INFO(dis);
  size_t locations = 0;
  for (const std::string& line : lines_of(dis))
    if (line.find("OpDecorate") != std::string::npos && line.find(" Location ") != std::string::npos) ++locations;
  CHECK(locations == 16);
  for (uint32_t i = 0; i < 16; ++i) CHECK(dis.find("\"out_interp" + std::to_string(i) + "\"") != std::string::npos);

  const std::string point_size = decorated_id(dis, "BuiltIn PointSize");
  REQUIRE_FALSE(point_size.empty());
  std::string value;
  for (const std::string& line : lines_of(dis)) {
    const auto at = line.find("OpStore " + point_size + " ");
    if (at != std::string::npos) value = line.substr(at + 9 + point_size.size());
  }
  REQUIRE_FALSE(value.empty());
  bool is_one = false;
  for (const std::string& line : lines_of(dis))
    if (line.find(value + " = OpConstant ") != std::string::npos && line.ends_with(" 1")) is_one = true;
  CHECK(is_one);
}

TEST_CASE("ps declares only the interpolators it reads", "[unit]") {
  const auto ps = translate_ok(ShaderStage::Pixel, ps_code({add(0, r(5), r(20), true)}));
  CHECK(ps.info.interpolators_read == (1u << 5));
  require_valid(ps.spirv);
  const std::string dis = lr::testing::spirv_dis(ps.spirv);
  INFO(dis);
  for (uint32_t i = 0; i < 16; ++i) {
    INFO("r" << i);
    CHECK((dis.find("\"in_r" + std::to_string(i) + "\"") != std::string::npos) == (i == 5));
  }
  const std::string input = decorated_id(dis, "Location 5");
  REQUIRE_FALSE(input.empty());
  CHECK(dis.find(input + " = OpVariable") != std::string::npos);
}

// --- other --------------------------------------------------------------------------------------------------------

TEST_CASE("memexport fails translation with its name", "[unit]") {
  // Slot 6: the five position instructions take slots 1..5.
  auto eM0 = mov(sh::kExportData0, c(3), true);
  const auto vs = try_translate(ShaderStage::Vertex, vs_code({eM0}));
  REQUIRE_FALSE(vs.ok());
  CHECK(vs.error().code == lr::ErrorCode::InvalidFormat);
  CHECK(vs.error().message.find("memexport (eA/eM) is not supported") != std::string::npos);
  CHECK(vs.error().message.find("instruction 6 (alu)") != std::string::npos);

  const auto ps = try_translate(ShaderStage::Pixel, ps_code({mov(0, c(0), true), mov(sh::kExportAddress, c(1), true)}));
  REQUIRE_FALSE(ps.ok());
  CHECK(ps.error().message == "instruction 2 (alu): memexport (eA/eM) is not supported");
}

TEST_CASE("every translated test module passes spirv-val", "[unit]") {
  if (!lr::testing::spirv_tools_available()) SKIP("spirv-val/spirv-dis not installed (Arch: spirv-tools)");
  std::vector<std::pair<std::string, sh::TranslatedShader>> modules;
  const auto add_module = [&](std::string name, ShaderStage stage, const std::vector<uint32_t>& code,
                              const sh::TranslateOptions& options = {}) {
    auto t = try_translate(stage, code, options);
    if (!t) FAIL(name << ": " << t.error().message);
    modules.emplace_back(std::move(name), std::move(t.value()));
  };

  add_module("vs position only", ShaderStage::Vertex, vs_code());
  add_module("vs without a position", ShaderStage::Vertex, straight_line({mov(0, c(0), true)}));
  {
    std::vector<xe::Alu> all;
    for (uint32_t i = 0; i < 16; ++i) all.push_back(mov(i, r(0, kXXXX), true));
    all.push_back(mov(sh::kExportVsPointSize, c(3), true));
    add_module("vs every export", ShaderStage::Vertex, vs_code(all));
  }
  add_module("vs saturate and masks", ShaderStage::Vertex, [] {
    xe::Alu sat = add(4, r(0), c(3));
    sat.vector_saturate = true;
    xe::Alu partial = mul(4, r(4), c(4));
    partial.vector_mask = 0x5;
    xe::Alu exp = mov(1, r(4), true);
    exp.vector_mask = 0x3;
    return vs_code({sat, partial, exp});
  }());
  add_module("ps copy", ShaderStage::Pixel, ps_code({mov(0, r(0), true)}));
  {
    // Reads all 16 interpolators; every color, depth; flat/centroid on alternate registers; param_gen for points.
    std::vector<xe::Alu> alus;
    for (uint32_t i = 1; i < 16; ++i) alus.push_back(add(0, r(0), r(i)));
    alus.push_back(mov(0, r(0), true));
    for (uint32_t i = 1; i < 4; ++i) alus.push_back(mov(i, r(i), true));
    alus.push_back(mov(sh::kExportPsDepth, r(4), true));
    sh::TranslateOptions o;
    o.flat_mask = 0x5555;
    o.centroid_mask = 0xAAAA;
    add_module("ps every interpolator, color and depth", ShaderStage::Pixel, ps_code(alus), o);
    o.ps_param_gen = 3;
    o.ps_point = true;
    add_module("ps param_gen point", ShaderStage::Pixel, ps_code(alus), o);
    o.ps_param_gen = 40;  // past every register the shader has: ignored
    add_module("ps param_gen unused", ShaderStage::Pixel, ps_code(alus), o);
  }
  {
    // An export of constants only: the const0/const1 masks (vector and scalar masks overlapping, scalar_dest_rel).
    xe::Alu consts = mov(0, c(0), true);
    consts.vector_mask = 0x3;
    consts.scalar_mask = 0x2;
    consts.scalar_dest_relative = true;  // components 2, 3: 0.0; component 1: 1.0; component 0: the vector result
    add_module("ps constant export masks", ShaderStage::Pixel, ps_code({consts}));
  }
  for (const auto& [name, t] : modules) {
    INFO(name);
    require_valid(t.spirv);
  }
}

TEST_CASE("export constant masks write 0 and 1", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  const auto vs = translate_ok(ShaderStage::Vertex, vs_code());
  xe::Alu consts = mov(0, c(0), true);
  consts.vector_mask = 0x3;          // x, y
  consts.scalar_mask = 0x2;          // y overlaps: 1.0
  consts.scalar_dest_relative = true;  // z, w (in neither mask): 0.0
  const auto ps = translate_ok(ShaderStage::Pixel, ps_code({consts}));
  HarnessInputs in;
  in.vs_constants = fullscreen();
  in.ps_constants = {{5, 6, 7, 8}};
  const HarnessResult res = draw_ok(h, vs, ps, in);
  for (const Pixel& p : res.color[0]) CHECK(p == Pixel{5, 1, 0, 0});
}

TEST_CASE("a scalar op with a zero write mask still reads its sources", "[unit]") {
  // Every scalar op but RETAIN_PREV runs whatever its write mask (its result becomes ps), so its operands count.
  SECTION("a temporary operand") {
    xe::Alu add_s = mov(0, c(0), true);
    add_s.scalar_op = xe::kScalarAdd;
    add_s.scalar_mask = 0;
    add_s.src3 = r(5);
    const std::vector<uint32_t> code = ps_code({add_s});
    auto program = sh::decode(ShaderStage::Pixel, code);
    REQUIRE(program);
    CHECK(program.value().temp_count == 6);
    const auto ps = translate_ok(ShaderStage::Pixel, code);
    CHECK(ps.info.interpolators_read == (1u << 5));  // r5 is an input at location 5
  }
  SECTION("a CONST op's temporary") {
    // MUL_CONST_1 with temporary 9: bit 0 in the opcode, bit 1 (0) in src3's select bit, bits 2..5 in its swizzle.
    xe::Alu mulc = mov(0, c(0), true);
    mulc.scalar_op = xe::kScalarMulConst1;
    mulc.scalar_mask = 0;
    mulc.src3 = c(3, 9 & 0x3C);
    const std::vector<uint32_t> code = ps_code({mulc});
    auto program = sh::decode(ShaderStage::Pixel, code);
    REQUIRE(program);
    CHECK(program.value().temp_count == 10);
    const auto ps = translate_ok(ShaderStage::Pixel, code);
    CHECK(ps.info.interpolators_read == (1u << 9));
  }
}
