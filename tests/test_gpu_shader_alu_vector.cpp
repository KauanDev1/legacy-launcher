// SPDX-License-Identifier: MIT
// The vector ALU of the plan B translator (translator_alu.cpp): every vector opcode, source modifiers, relative
// constants and temporaries, write masks, saturate, MAXA's a0, kills and the Xenos multiply rule (a product with a 0
// factor is +0). Shaders are synthetic microcode (support/xenos_shader_builder.h); every module goes through spirv-val,
// and every value is checked on lavapipe through support/vk_shader_harness.h. Expected values follow the Xenos
// semantics as Xenia's ucode.h and SPIR-V translator describe them (reference only).
//
// Most tests are pixel shaders over a full-screen triangle: they compute into r1 from constants and export to oC0..3.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
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

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

// Source swizzles (component i reads ((swizzle >> 2i) + i) & 3).
constexpr uint32_t kXXXX = 0x6C;
constexpr uint32_t kWZYX = 0x77;
constexpr uint32_t kZZXY = 0xA6;  // CUBE's src1
constexpr uint32_t kYXZZ = 0xCD;  // CUBE's src2
constexpr uint32_t kYXZW = 0x0D;  // y, x, z, w

// --- microcode ----------------------------------------------------------------------------------------------------

xe::AluSrc r(uint32_t reg, uint32_t swizzle = 0) { return {reg, true, swizzle}; }
xe::AluSrc c(uint32_t reg, uint32_t swizzle = 0) { return {reg, false, swizzle}; }
xe::AluSrc neg(xe::AluSrc s) {
  s.negate = true;
  return s;
}
xe::AluSrc abs_(xe::AluSrc s) {
  s.abs = true;
  return s;
}

xe::Alu vop(uint32_t op, uint32_t dest, xe::AluSrc a, xe::AluSrc b, xe::AluSrc c3 = {}, bool to_export = false) {
  xe::Alu alu;
  alu.vector_op = op;
  alu.vector_dest = dest;
  alu.export_data = to_export;
  alu.src1 = a;
  alu.src2 = b;
  alu.src3 = c3;
  return alu;
}
xe::Alu mov(uint32_t dest, xe::AluSrc s, bool to_export = false) { return vop(xe::kVectorMax, dest, s, s, {}, to_export); }
xe::Alu add(uint32_t dest, xe::AluSrc a, xe::AluSrc b, bool to_export = false) {
  return vop(xe::kVectorAdd, dest, a, b, {}, to_export);
}
xe::Alu mul(uint32_t dest, xe::AluSrc a, xe::AluSrc b, bool to_export = false) {
  return vop(xe::kVectorMul, dest, a, b, {}, to_export);
}
xe::Alu masked(xe::Alu a, uint32_t mask) {
  a.vector_mask = mask;
  return a;
}
xe::Alu predicated(xe::Alu alu, bool condition) {
  alu.predicated = true;
  alu.pred_condition = condition;
  return alu;
}
// Every constant operand relative (const0 and const1), to aL or (a0) to a0.
xe::Alu const_relative(xe::Alu a, bool a0) {
  a.const0_relative = a.const1_relative = true;
  a.address_register_relative = a0;
  return a;
}

struct Cf {
  uint64_t raw = 0;
  std::function<uint64_t(uint32_t)> make;
  std::vector<xe::Alu> alus;
};
Cf raw(uint64_t word) { return {word, nullptr, {}}; }
Cf exec(std::vector<xe::Alu> alus, bool end = false) {
  const auto n = static_cast<uint32_t>(alus.size());
  return {0, [=](uint32_t a) { return xe::cf_exec(end, a, n, 0); }, std::move(alus)};
}
Cf exec_end(std::vector<xe::Alu> alus) { return exec(std::move(alus), true); }

std::vector<uint32_t> assemble(const std::vector<Cf>& program) {
  std::vector<uint64_t> words;
  std::vector<xe::Alu> alus;
  const auto cf_slots = static_cast<uint32_t>((program.size() + 1) / 2);
  for (const Cf& cf : program) {
    if (cf.make) {
      words.push_back(cf.make(cf_slots + static_cast<uint32_t>(alus.size())));
      alus.insert(alus.end(), cf.alus.begin(), cf.alus.end());
    } else {
      words.push_back(cf.raw);
    }
  }
  if (words.size() % 2) words.push_back(xe::cf_nop());
  std::vector<uint32_t> code;
  for (size_t i = 0; i < words.size(); i += 2) xe::emit(code, xe::cf_pair(words[i], words[i + 1]));
  for (const xe::Alu& a : alus) xe::emit(code, xe::alu(a));
  return code;
}

uint32_t loop_constant(uint32_t count, uint32_t start, int32_t step) {
  return (count & 0xFF) | ((start & 0xFF) << 8) | ((static_cast<uint32_t>(step) & 0xFF) << 16);
}

// --- the vertex shader: one triangle over the whole target --------------------------------------------------------

std::vector<uint32_t> fullscreen_vs() {
  // export 62 = c0 + c1 * i + c2 * i * i, i = r0.x (as test_gpu_shader_cf.cpp).
  return assemble({exec_end({mul(1, r(0, kXXXX), c(1)), mul(2, r(0, kXXXX), r(0, kXXXX)), mul(2, r(2), c(2)),
                             add(1, r(1), r(2)), add(sh::kExportVsPosition, r(1), c(0), true)})});
}
std::vector<Pixel> fullscreen_constants() {
  const Pixel p0{-1, -1, 0, 1}, p1{3, -1, 0, 1}, p2{-1, 3, 0, 1};
  Pixel a{}, b{}, q{};
  for (int k = 0; k < 4; ++k) {
    q[k] = (p2[k] - 2 * p1[k] + p0[k]) / 2;
    b[k] = p1[k] - p0[k] - q[k];
    a[k] = p0[k];
  }
  return {a, b, q};
}

// --- translation and execution ------------------------------------------------------------------------------------

void require_valid(const std::vector<uint32_t>& module) {
  REQUIRE_FALSE(module.empty());
  if (!lr::testing::spirv_tools_available()) return;
  if (auto err = lr::testing::spirv_val(module)) FAIL("spirv-val: " << *err << "\n" << lr::testing::spirv_dis(module));
}

sh::TranslatedShader translate_ok(ShaderStage stage, const std::vector<uint32_t>& code) {
  auto program = sh::decode(stage, code);
  if (!program) FAIL("decode: " << program.error().message);
  auto t = sh::translate(program.value(), {});
  if (!t) FAIL("translate: " << t.error().message);
  require_valid(t.value().spirv);
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

std::string text(const Pixel& p) {
  std::ostringstream s;
  s << "(" << p[0] << ", " << p[1] << ", " << p[2] << ", " << p[3] << ")";
  return s.str();
}

bool same_bits(const Pixel& a, const Pixel& b) { return std::memcmp(a.data(), b.data(), sizeof(Pixel)) == 0; }

// Draws the full-screen triangle with the pixel shader `program`; returns pixel 0 of each colour target (every pixel
// of a target must be bit-identical to it).
std::array<Pixel, 4> draw(ShaderHarness& h, const std::vector<Cf>& program, HarnessInputs in) {
  const sh::TranslatedShader ps = translate_ok(ShaderStage::Pixel, assemble(program));
  const sh::TranslatedShader vs = translate_ok(ShaderStage::Vertex, fullscreen_vs());
  in.vs_constants = fullscreen_constants();
  auto res = h.draw(vs.spirv, ps.spirv, in);
  if (!res) FAIL("draw: " << res.error().message);
  const HarnessResult& out = res.value();
  std::array<Pixel, 4> first{};
  for (uint32_t t = 0; t < in.color_targets; ++t) {
    REQUIRE(out.color[t].size() == in.width);
    first[t] = out.color[t][0];
    for (const Pixel& p : out.color[t]) {
      INFO("target " << t << ": " << text(p) << " vs " << text(first[t]));
      REQUIRE(same_bits(p, first[t]));
    }
  }
  return first;
}

void check_pixel(const Pixel& got, const Pixel& want) {
  INFO("got " << text(got) << ", want " << text(want));
  CHECK(got == want);
}

}  // namespace

// --- the op table -------------------------------------------------------------------------------------------------

TEST_CASE("vector op table", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // r1 = op(c1 [swizzle], c2, c3); r2 = (1, p0, p0, p0) through a predicated mov; oC0 = r1, oC1 = r2. oC1.x = 1 also
  // says the pixel survived.
  struct Row {
    const char* name;
    uint32_t op;
    Pixel a, b, c;
    Pixel expected;
    bool p0 = false;
    uint32_t a_swizzle = 0;
    uint32_t b_swizzle = 0;
  };
  const std::vector<Row> rows = {
      // per component a + b
      {"ADD", xe::kVectorAdd, {1, 2, 3, 4}, {10, 20, 30, 40}, {}, {11, 22, 33, 44}},
      // a * b; 0 * inf = +0 (the factors are different operands)
      {"MUL", xe::kVectorMul, {1, 2, -3, 0}, {2, 3, 4, kInf}, {}, {2, 6, -12, 0}},
      // a >= b ? a : b
      {"MAX", xe::kVectorMax, {1, 5, -3, 0}, {2, 4, -4, 0}, {}, {2, 5, -3, 0}},
      // a < b ? a : b
      {"MIN", xe::kVectorMin, {1, 5, -3, 0}, {2, 4, -4, 0}, {}, {1, 4, -4, 0}},
      // a == b ? 1 : 0, NaN: 0
      {"SETE", xe::kVectorSetEq, {1, 2, 3, kNan}, {1, 3, 3, kNan}, {}, {1, 0, 1, 0}},
      // a > b ? 1 : 0
      {"SETGT", xe::kVectorSetGt, {2, 1, 3, 0}, {1, 1, 4, 0}, {}, {1, 0, 0, 0}},
      // a >= b ? 1 : 0
      {"SETGTE", xe::kVectorSetGe, {2, 1, 3, 0}, {1, 1, 4, 0}, {}, {1, 1, 0, 1}},
      // a != b ? 1 : 0, NaN: 1 (unordered)
      {"SETNE", xe::kVectorSetNe, {1, 2, 3, kNan}, {1, 3, 3, kNan}, {}, {0, 1, 0, 1}},
      // a - floor(a)
      {"FRAC", xe::kVectorFrac, {1.25f, -1.25f, 3, 0}, {}, {}, {0.25f, 0.75f, 0, 0}},
      // toward zero
      {"TRUNC", xe::kVectorTrunc, {1.5f, -1.5f, 2, -0.25f}, {}, {}, {1, -1, 2, 0}},
      // toward -inf
      {"FLOOR", xe::kVectorFloor, {1.5f, -1.5f, 2, -0.25f}, {}, {}, {1, -2, 2, -1}},
      // a * b + c, the product with the 0 rule
      {"MULADD", xe::kVectorMulAdd, {1, 2, 0, 3}, {2, 3, kInf, 0}, {1, 1, 5, -1}, {3, 7, 5, -1}},
      // a == 0 ? b : c (NaN: c)
      {"CNDE", xe::kVectorCndEq, {0, 1, -0.0f, kNan}, {1, 2, 3, 4}, {5, 6, 7, 8}, {1, 6, 3, 8}},
      // a >= 0 ? b : c
      {"CNDGTE", xe::kVectorCndGe, {0, -1, 2, kNan}, {1, 2, 3, 4}, {5, 6, 7, 8}, {1, 6, 3, 8}},
      // a > 0 ? b : c
      {"CNDGT", xe::kVectorCndGt, {0, -1, 2, kNan}, {1, 2, 3, 4}, {5, 6, 7, 8}, {5, 6, 3, 8}},
      // a.xyzw . b.xyzw, replicated
      {"DOT4", xe::kVectorDot4, {1, 2, 3, 4}, {5, 6, 7, 8}, {}, {70, 70, 70, 70}},
      // a.xyz . b.xyz (w ignored, even NaN)
      {"DOT3", xe::kVectorDot3, {1, 2, 3, kNan}, {5, 6, 7, 8}, {}, {38, 38, 38, 38}},
      // a.xy . b.xy + c.x
      {"DOT2ADD", xe::kVectorDot2Add, {1, 2, kNan, kNan}, {5, 6, 7, 8}, {100, 0, 0, 0}, {117, 117, 117, 117}},
      // (x, y, z) = (1, 2, -3) read as src1.zzxy: major axis -Z: tc = -y, sc = -x, 2 * ma, face 5
      {"CUBE", xe::kVectorCube, {1, 2, -3, 0}, {1, 2, -3, 0}, {}, {-2, -1, -6, 5}, false, kZZXY, kYXZZ},
      // max of a's components, replicated
      {"MAX4", xe::kVectorMax4, {1, 7, 3, -2}, {}, {}, {7, 7, 7, 7}},
      // p0 = a.w == 0 && b.w == 0; result = (a.x == 0 && b.x == 0) ? 0 : a.x + 1, replicated
      {"PRED_SETE_PUSH", xe::kVectorPredSetEqPush, {0, 0, 0, 0}, {0, 0, 0, 0}, {}, {0, 0, 0, 0}, true},
      {"PRED_SETE_PUSH false", xe::kVectorPredSetEqPush, {2, 0, 0, 1}, {0, 0, 0, 0}, {}, {3, 3, 3, 3}, false},
      // ... b op 0 with !=
      {"PRED_SETNE_PUSH", xe::kVectorPredSetNePush, {0, 0, 0, 0}, {1, 1, 1, 1}, {}, {0, 0, 0, 0}, true},
      // ... with >: x fails (b.x = -1), w holds
      {"PRED_SETGT_PUSH", xe::kVectorPredSetGtPush, {0, 0, 0, 0}, {-1, 0, 0, 1}, {}, {1, 1, 1, 1}, true},
      // ... with >=: x fails (a.x = 3), w fails (b.w = -1)
      {"PRED_SETGTE_PUSH", xe::kVectorPredSetGePush, {3, 0, 0, 0}, {0, 0, 0, -1}, {}, {4, 4, 4, 4}, false},
      // no component kills: result 0 (the kills themselves are in "kill ops discard the pixel")
      {"KILLE", xe::kVectorKillEq, {1, 2, 3, 4}, {5, 6, 7, 8}, {}, {0, 0, 0, 0}},
      {"KILLGT", xe::kVectorKillGt, {1, 2, 3, 4}, {5, 6, 7, 8}, {}, {0, 0, 0, 0}},
      {"KILLGTE", xe::kVectorKillGe, {1, 2, 3, 4}, {5, 6, 7, 8}, {}, {0, 0, 0, 0}},
      {"KILLNE", xe::kVectorKillNe, {1, 2, 3, 4}, {1, 2, 3, 4}, {}, {0, 0, 0, 0}},
      // (1, a.y * b.y, a.z, b.w)
      {"DST", xe::kVectorDst, {9, 2, 3, 9}, {9, 4, 9, 5}, {}, {1, 8, 3, 5}},
      // max(a, b) (a0 = 3 from a.w, see "maxa sets a0 clamped and returns the max")
      {"MAXA", xe::kVectorMaxA, {1, 5, -3, 2.5f}, {2, 4, -4, 0}, {}, {2, 5, -3, 2.5f}},
  };
  for (const Row& row : rows) {
    INFO(row.name);
    HarnessInputs in;
    in.color_targets = 2;
    in.ps_constants = {{0, 0, 0, 0}, row.a, row.b, row.c, {1, 1, 1, 1}, {1, 0, 0, 0}};
    const std::vector<Cf> program = {
        exec_end({vop(row.op, 1, c(1, row.a_swizzle), c(2, row.b_swizzle), c(3)), mov(2, c(5)),
                  predicated(mov(2, c(4)), true), mov(0, r(1), true), mov(1, r(2), true)})};
    const auto out = draw(h, program, in);
    check_pixel(out[0], row.expected);
    const float p = row.p0 ? 1.0f : 0.0f;
    check_pixel(out[1], {1, p, p, p});
  }
}

// --- the Xenos multiply rule --------------------------------------------------------------------------------------

TEST_CASE("mul of zero by infinity is zero", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  HarnessInputs in;
  in.color_targets = 4;
  in.ps_constants = {{0, 0, 0, 0}, {0, -0.0f, kInf, 0}, {kInf, kInf, -0.0f, kNan}, {kInf, 0, -2, kNan}, {0, -0.0f, 3, 4}};
  const auto out = draw(h, {exec_end({mul(0, c(1), c(2), true),           // every product has a 0 factor: +0
                                      mul(1, c(3), c(3), true),           // x * x: inf, 0, 4, NaN
                                      mul(2, c(3), c(3, kYXZW), true),    // inf * 0 and 0 * inf differ: +0
                                      mul(3, c(4), neg(c(4)), true)})},   // 0 * -0: not the same operand, +0
                        in);
  for (uint32_t i = 0; i < 4; ++i) {
    INFO("component " << i);
    CHECK(out[0][i] == 0.0f);
    CHECK_FALSE(std::signbit(out[0][i]));
  }
  CHECK(out[1][0] == kInf);  // the same-operand exception: inf * inf
  CHECK(out[1][1] == 0.0f);
  CHECK(out[1][2] == 4.0f);
  CHECK(std::isnan(out[1][3]));
  CHECK(out[2][0] == 0.0f);
  CHECK_FALSE(std::signbit(out[2][0]));
  CHECK(out[2][1] == 0.0f);
  CHECK(out[2][2] == 4.0f);
  CHECK(std::isnan(out[2][3]));
  CHECK(out[3][0] == 0.0f);
  CHECK_FALSE(std::signbit(out[3][0]));
  CHECK_FALSE(std::signbit(out[3][1]));
  CHECK(out[3][2] == -9.0f);
  CHECK(out[3][3] == -16.0f);
}

TEST_CASE("muladd keeps the addend when the product is zeroed", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  HarnessInputs in;
  in.ps_constants = {{0, 0, 0, 0}, {0, 0, kInf, -0.0f}, {kInf, kNan, 0, 5}, {7, -3, kInf, -0.0f}};
  const auto out = draw(h, {exec_end({vop(xe::kVectorMulAdd, 0, c(1), c(2), c(3), true)})}, in);
  check_pixel(out[0], {7, -3, kInf, 0});
  CHECK_FALSE(std::signbit(out[0][3]));  // +0 product + -0 addend = +0
}

TEST_CASE("dot products and dst zero their products", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  HarnessInputs in;
  in.color_targets = 4;
  in.ps_constants = {{0, 0, 0, 0},     {0, 1, 2, 0},  {kInf, 1, 1, kNan}, {0, 1, 1, kNan}, {kInf, 2, 3, 5},
                     {0, 2, 0, 0},     {kNan, 3, 0, 0}, {10, 0, 0, 0},   {9, 0, 3, 9},    {9, kInf, 9, 5}};
  const auto out = draw(h,
                        {exec_end({vop(xe::kVectorDot4, 0, c(1), c(2), {}, true),       // 0 + 1 + 2 + 0
                                   vop(xe::kVectorDot3, 1, c(3), c(4), {}, true),       // 0 + 2 + 3
                                   vop(xe::kVectorDot2Add, 2, c(5), c(6), c(7), true),  // 0 + 6 + 10
                                   vop(xe::kVectorDst, 3, c(8), c(9), {}, true)})},     // (1, 0 * inf, 3, 5)
                        in);
  check_pixel(out[0], {3, 3, 3, 3});
  check_pixel(out[1], {5, 5, 5, 5});
  check_pixel(out[2], {16, 16, 16, 16});
  check_pixel(out[3], {1, 0, 3, 5});
  CHECK_FALSE(std::signbit(out[3][1]));
}

// --- MAXA and relative constants ----------------------------------------------------------------------------------

namespace {
// c[k] = k + 1 for every k, except c250 and c251, the MAXA operands.
std::vector<Pixel> index_constants(Pixel a, Pixel b) {
  std::vector<Pixel> out;
  for (uint32_t k = 0; k < sh::kFloatConstantCount; ++k) {
    const auto v = static_cast<float>(k + 1);
    out.push_back({v, v, v, v});
  }
  out[250] = a;
  out[251] = b;
  return out;
}
}  // namespace

TEST_CASE("maxa sets a0 clamped and returns the max", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // r1 = maxa(c250, c251); oC0 = r1; oC1..3 = c[a0 + 20], c[a0 + 0], c[a0 + 255] (k + 1, or 0 out of range).
  const std::vector<Cf> program = {exec_end({vop(xe::kVectorMaxA, 1, c(250), c(251)), mov(0, r(1), true),
                                             const_relative(mov(1, c(20), true), true),
                                             const_relative(mov(2, c(0), true), true),
                                             const_relative(mov(3, c(255), true), true)})};
  struct Case {
    float w;
    float c20, c0, c255;  // the expected reads
  };
  for (const Case& k : {Case{2.5f, 24, 4, 0},           // a0 = floor(3.0) = 3
                        Case{0.5f, 22, 2, 0},           // round half up: 1
                        Case{-0.5f, 21, 1, 256},        // floor(0.0) = 0
                        Case{1.49f, 22, 2, 0},          // 1
                        Case{-1.5f, 20, 0, 255},        // floor(-1.0) = -1
                        Case{1000, 0, 256, 0},          // clamped to 255
                        Case{-255.2f, 0, 0, 1},         // -255
                        Case{-1000, 0, 0, 0}}) {        // clamped to -256: c[-1] reads 0
    INFO("w = " << k.w);
    HarnessInputs in;
    in.color_targets = 4;
    in.ps_constants = index_constants({1, 5, -3, k.w}, {2, 4, -4, -2000});
    const auto out = draw(h, program, in);
    check_pixel(out[0], {2, 5, -3, k.w});
    check_pixel(out[1], {k.c20, k.c20, k.c20, k.c20});
    check_pixel(out[2], {k.c0, k.c0, k.c0, k.c0});
    check_pixel(out[3], {k.c255, k.c255, k.c255, k.c255});
  }
}

TEST_CASE("relative constant c[a0+2]", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // MAXA with a zero write mask only sets a0 (= 3 from c250.w); then oC0 = c[a0 + 2] = c5 = 6. An instruction reads
  // a0 as it was before the instruction: the MAXA's own operands are not relative.
  HarnessInputs in;
  in.ps_constants = index_constants({0, 0, 0, 3}, {0, 0, 0, 0});
  const auto out = draw(h,
                        {exec_end({masked(vop(xe::kVectorMaxA, 1, c(250), c(251)), 0),
                                   const_relative(mov(0, c(2), true), true)})},
                        in);
  check_pixel(out[0], {6, 6, 6, 6});
}

TEST_CASE("relative constant with aL", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // Loop 0 runs once with aL = 4: oC0 = c[aL + 3] = c7 = 8.
  const std::vector<Cf> program = {raw(xe::cf_loop_start(0, 3)), exec({const_relative(mov(1, c(3)), false)}),
                                   raw(xe::cf_loop_end(0, 1)), exec_end({mov(0, r(1), true)})};
  HarnessInputs in;
  in.ps_constants = index_constants({}, {});
  in.loops[0] = loop_constant(1, 4, 0);
  check_pixel(draw(h, program, in)[0], {8, 8, 8, 8});
}

TEST_CASE("second constant operand uses const_1 addressing", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // aL = 3 inside loop 0. The first constant operand takes const_0_rel, any later one const_1_rel.
  const auto run = [&](xe::Alu alu) {
    alu.vector_dest = 1;
    const std::vector<Cf> program = {exec({mov(2, c(100))}), raw(xe::cf_loop_start(0, 4)), exec({alu}),
                                     raw(xe::cf_loop_end(0, 2)), exec_end({mov(0, r(1), true)})};
    HarnessInputs in;
    in.ps_constants = index_constants({}, {});
    in.loops[0] = loop_constant(1, 3, 0);
    return draw(h, program, in)[0][0];
  };
  xe::Alu only_second = add(1, c(1), c(10));
  only_second.const1_relative = true;
  CHECK(run(only_second) == 2.0f + 14.0f);  // c1 + c[aL + 10]
  xe::Alu only_first = add(1, c(1), c(10));
  only_first.const0_relative = true;
  CHECK(run(only_first) == 5.0f + 11.0f);  // c[aL + 1] + c10
  xe::Alu temp_first = add(1, r(2), c(10));  // r2 = c100: the constant is the first one, const_0_rel
  temp_first.const0_relative = true;
  CHECK(run(temp_first) == 101.0f + 14.0f);
}

TEST_CASE("out-of-range relative constant reads zero", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  HarnessInputs in;
  SECTION("aL past 255") {
    // aL = 250: c[aL + 10] = c260 does not exist, c[aL + 5] = c255 does.
    const std::vector<Cf> program = {raw(xe::cf_loop_start(0, 3)),
                                     exec({const_relative(mov(1, c(10)), false), const_relative(mov(2, c(5)), false)}),
                                     raw(xe::cf_loop_end(0, 1)), exec_end({mov(0, r(1), true), mov(1, r(2), true)})};
    in.color_targets = 2;
    in.ps_constants = index_constants({}, {});
    in.loops[0] = loop_constant(1, 250, 0);
    const auto out = draw(h, program, in);
    check_pixel(out[0], {0, 0, 0, 0});
    check_pixel(out[1], {256, 256, 256, 256});
  }
  SECTION("negative a0") {
    // a0 = -5: c[a0 + 2] reads 0.
    in.ps_constants = index_constants({0, 0, 0, -5}, {0, 0, 0, -9});
    const auto out = draw(h,
                          {exec_end({masked(vop(xe::kVectorMaxA, 1, c(250), c(251)), 0),
                                     const_relative(mov(0, c(2), true), true)})},
                          in);
    check_pixel(out[0], {0, 0, 0, 0});
  }
}

// --- operands and destinations ------------------------------------------------------------------------------------

TEST_CASE("aL-relative destination register is written", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // Loop 0 runs once with aL = start: r[aL + 2] = c1. With start 5 that is r7; with 100 the index clamps to r63.
  xe::Alu write = mov(2, c(1));
  write.vector_dest_relative = true;
  const std::vector<Cf> program = {exec({mov(7, c(0)), mov(63, c(0))}), raw(xe::cf_loop_start(0, 4)), exec({write}),
                                   raw(xe::cf_loop_end(0, 2)), exec_end({mov(0, r(7), true), mov(1, r(63), true)})};
  HarnessInputs in;
  in.color_targets = 2;
  in.ps_constants = {{0, 0, 0, 0}, {1, 2, 3, 4}};
  in.loops[0] = loop_constant(1, 5, 0);
  auto out = draw(h, program, in);
  check_pixel(out[0], {1, 2, 3, 4});
  check_pixel(out[1], {0, 0, 0, 0});
  in.loops[0] = loop_constant(1, 100, 0);
  out = draw(h, program, in);
  check_pixel(out[0], {0, 0, 0, 0});
  check_pixel(out[1], {1, 2, 3, 4});
}

TEST_CASE("swizzle and negate", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  HarnessInputs in;
  in.color_targets = 3;
  in.ps_constants = {{0, 0, 0, 0}, {1, 2, 3, 4}, {-10, 20, -30, 40}};
  xe::Alu abs_constants = add(1, c(2), neg(c(1)), true);  // |c2| - c1
  abs_constants.abs_constants = true;
  const auto out = draw(h,
                        {exec_end({mov(2, c(1)), mov(3, c(2)),
                                   add(0, neg(r(2, kWZYX)), abs_(r(3)), true),  // -r2.wzyx + |r3|
                                   abs_constants,
                                   add(2, neg(abs_(r(3))), c(0), true)})},  // -|r3|
                        in);
  check_pixel(out[0], {6, 17, 28, 39});
  check_pixel(out[1], {9, 18, 27, 36});
  check_pixel(out[2], {-10, -20, -30, -40});
}

TEST_CASE("write mask keeps the other components", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  HarnessInputs in;
  in.color_targets = 2;
  in.ps_constants = {{0, 0, 0, 0}, {1, 2, 3, 4}, {10, 20, 30, 40}};
  const auto out = draw(h,
                        {exec_end({mov(1, c(1)), masked(add(1, c(2), c(0)), 0xA), mov(0, r(1), true),
                                   masked(mov(1, r(1), true), 0x5)})},
                        in);
  check_pixel(out[0], {1, 20, 3, 40});
  check_pixel(out[1], {1, 0, 3, 0});  // the export's other components stay 0
}

TEST_CASE("vector saturate", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  HarnessInputs in;
  in.ps_constants = {{0, 0, 0, 0}, {-2, 0.5f, 3, kNan}, {0, 0, 0, 0}};
  xe::Alu sat = add(0, c(1), c(2), true);
  sat.vector_saturate = true;
  const auto out = draw(h, {exec_end({sat})}, in);
  check_pixel(out[0], {0, 0.5f, 1, 0});  // NaN saturates to 0
}

// --- CUBE and kills -----------------------------------------------------------------------------------------------

TEST_CASE("cube op output layout", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // (tc, sc, 2 * major axis, face id) for (x, y, z) read as src1.zzxy (Xenia's layout; the fetch half is Task 15.9).
  struct Case {
    Pixel xyz;
    Pixel expected;
  };
  for (const Case& k : {Case{{3, 1, 2, 0}, {-1, -2, 6, 0}},   // +X
                        Case{{-3, 1, 2, 0}, {-1, 2, -6, 1}},  // -X
                        Case{{1, 3, 2, 0}, {2, 1, 6, 2}},     // +Y
                        Case{{1, -3, 2, 0}, {-2, 1, -6, 3}},  // -Y
                        Case{{1, 2, 3, 0}, {-2, 1, 6, 4}},    // +Z
                        Case{{1, 2, -3, 0}, {-2, -1, -6, 5}},  // -Z
                        Case{{2, 2, 2, 0}, {-2, 2, 4, 4}},    // a tie picks Z
                        Case{{2, 2, 1, 0}, {1, 2, 4, 2}}}) {  // then Y over X
    INFO(text(k.xyz));
    HarnessInputs in;
    in.ps_constants = {{0, 0, 0, 0}, k.xyz};
    check_pixel(draw(h, {exec_end({vop(xe::kVectorCube, 0, c(1, kZZXY), c(1, kYXZZ), {}, true)})}, in)[0],
                k.expected);
  }
}

TEST_CASE("kill ops discard the pixel", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // kill(c1, c2) with a zero write mask, then oC0 = c3 = 1: a killed pixel keeps the clear colour 0.
  struct Case {
    const char* name;
    uint32_t op;
    Pixel a, b;
    bool kills;
  };
  const std::vector<Case> cases = {
      {"KILLE one equal component", xe::kVectorKillEq, {1, 2, 3, 4}, {5, 2, 7, 8}, true},
      {"KILLE none", xe::kVectorKillEq, {1, 2, 3, kNan}, {5, 6, 7, kNan}, false},
      {"KILLGT", xe::kVectorKillGt, {1, 2, 3, 9}, {5, 6, 7, 8}, true},
      {"KILLGT none", xe::kVectorKillGt, {1, 2, 3, 8}, {5, 6, 7, 8}, false},
      {"KILLGTE", xe::kVectorKillGe, {1, 2, 3, 8}, {5, 6, 7, 8}, true},
      {"KILLGTE none", xe::kVectorKillGe, {1, 2, 3, 7}, {5, 6, 7, 8}, false},
      {"KILLNE", xe::kVectorKillNe, {1, 2, 3, 4}, {1, 2, 3, 5}, true},
      {"KILLNE NaN", xe::kVectorKillNe, {1, 2, 3, kNan}, {1, 2, 3, kNan}, true},
      {"KILLNE none", xe::kVectorKillNe, {1, 2, 3, 4}, {1, 2, 3, 4}, false},
  };
  for (const Case& k : cases) {
    INFO(k.name);
    HarnessInputs in;
    in.ps_constants = {{0, 0, 0, 0}, k.a, k.b, {1, 1, 1, 1}};
    const float v = k.kills ? 0.0f : 1.0f;
    check_pixel(draw(h, {exec_end({masked(vop(k.op, 1, c(1), c(2)), 0), mov(0, c(3), true)})}, in)[0], {v, v, v, v});
  }
  // The kill result is 1 when it kills (the pixel is discarded at the end, so later instructions still run).
  if (lr::testing::spirv_tools_available()) {
    const auto ps = translate_ok(ShaderStage::Pixel,
                                 assemble({exec_end({vop(xe::kVectorKillEq, 1, c(1), c(2)), mov(0, r(1), true)})}));
    const std::string dis = lr::testing::spirv_dis(ps.spirv);
    INFO(dis);
    // A single OpKill, in the epilogue: nothing after the kill instruction loses its derivatives. (A fetch after the
    // kill is checked once fetches translate, Tasks 15.8/15.9.)
    size_t kills = 0;
    for (size_t at = dis.find("OpKill"); at != std::string::npos; at = dis.find("OpKill", at + 1)) ++kills;
    CHECK(kills == 1);
  }
}
