// SPDX-License-Identifier: MIT
// The scalar ALU of the plan B translator (translator_alu.cpp): every scalar opcode, the *_CONST forms, the
// previous scalar result `ps`, the predicate ops, MOVA/MOVA_FLOOR into a0, scalar kills, the Xenos multiply rule (a
// product with a 0 factor is +0) and the merge of vector and scalar results. Shaders are synthetic microcode
// (support/xenos_shader_builder.h); every module goes through spirv-val, and every value is checked on lavapipe through
// support/vk_shader_harness.h. Expected values follow the Xenos semantics as Xenia's ucode.h and SPIR-V translator
// describe them (reference only).
//
// Scalar operands: a = src3's W (after the swizzle), b = src3's X. The *_CONST ops take a from the constant src3 and b
// from a temporary whose index is spread over the opcode bit 0, src3's select bit and src3's swizzle bits 2..5.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <regex>
#include <set>
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
constexpr float kMax = FLT_MAX;
constexpr float kPi = 3.14159265358979f;

// Source swizzles (component i reads ((swizzle >> 2i) + i) & 3).
constexpr uint32_t kWWWW = 0x1B;  // every component reads w: a and b are the same component

// --- microcode ----------------------------------------------------------------------------------------------------

xe::AluSrc r(uint32_t reg, uint32_t swizzle = 0) { return {reg, true, swizzle}; }
xe::AluSrc c(uint32_t reg, uint32_t swizzle = 0) { return {reg, false, swizzle}; }
xe::AluSrc neg(xe::AluSrc s) {
  s.negate = true;
  return s;
}

xe::Alu vop(uint32_t op, uint32_t dest, xe::AluSrc a, xe::AluSrc b, bool to_export = false) {
  xe::Alu alu;
  alu.vector_op = op;
  alu.vector_dest = dest;
  alu.export_data = to_export;
  alu.src1 = a;
  alu.src2 = b;
  return alu;
}
xe::Alu mov(uint32_t dest, xe::AluSrc s, bool to_export = false) { return vop(xe::kVectorMax, dest, s, s, to_export); }
// A scalar-only instruction: op(src3) into r<dest> under `mask` (the vector unit writes nothing).
xe::Alu sop(uint32_t op, uint32_t dest, xe::AluSrc s, uint32_t mask = 0xF) {
  xe::Alu alu;
  alu.vector_mask = 0;
  alu.scalar_op = op;
  alu.scalar_dest = dest;
  alu.scalar_mask = mask;
  alu.src3 = s;
  return alu;
}
// A *_CONST scalar op: `base` is the _0 opcode (MUL_CONST_0, ADD_CONST_0, SUB_CONST_0). a = c<constant>.w, b =
// r<temp>.x; the temporary's bit 0 picks _0 or _1, bit 1 is src3's select bit, bits 2..5 sit in src3's swizzle.
xe::Alu sconst(uint32_t base, uint32_t dest, uint32_t constant, uint32_t temp, uint32_t mask = 0xF) {
  xe::Alu alu = sop(base | (temp & 1), dest, c(constant, temp & 0x3C), mask);
  alu.src3.temp = ((temp >> 1) & 1) != 0;
  return alu;
}
xe::Alu predicated(xe::Alu alu, bool condition) {
  alu.predicated = true;
  alu.pred_condition = condition;
  return alu;
}
xe::Alu saturated(xe::Alu alu) {
  alu.scalar_saturate = true;
  return alu;
}

// An exec clause; assemble() splits one longer than kClauseMax into several (the count field has 3 bits).
struct Cf {
  bool end = false;
  std::vector<xe::Alu> alus;
};
constexpr size_t kClauseMax = 6;
Cf exec(std::vector<xe::Alu> alus, bool end = false) { return {end, std::move(alus)}; }
Cf exec_end(std::vector<xe::Alu> alus) { return exec(std::move(alus), true); }

std::vector<uint32_t> assemble(const std::vector<Cf>& program) {
  struct Clause {
    bool end;
    size_t first, count;
  };
  std::vector<Clause> clauses;
  std::vector<xe::Alu> alus;
  for (const Cf& cf : program) {
    for (size_t at = 0; at < cf.alus.size(); at += kClauseMax) {
      const size_t n = std::min(kClauseMax, cf.alus.size() - at);
      clauses.push_back({cf.end && at + n == cf.alus.size(), alus.size(), n});
      alus.insert(alus.end(), cf.alus.begin() + static_cast<std::ptrdiff_t>(at),
                  cf.alus.begin() + static_cast<std::ptrdiff_t>(at + n));
    }
  }
  const auto cf_slots = static_cast<uint32_t>((clauses.size() + 1) / 2);
  std::vector<uint64_t> words;
  for (const Clause& k : clauses)
    words.push_back(xe::cf_exec(k.end, cf_slots + static_cast<uint32_t>(k.first), static_cast<uint32_t>(k.count), 0));
  if (words.size() % 2) words.push_back(xe::cf_nop());
  std::vector<uint32_t> code;
  for (size_t i = 0; i < words.size(); i += 2) xe::emit(code, xe::cf_pair(words[i], words[i + 1]));
  for (const xe::Alu& a : alus) xe::emit(code, xe::alu(a));
  return code;
}

// --- the vertex shader: one triangle over the whole target --------------------------------------------------------

// export 62 = c0 + c1 * i + c2 * i * i, i = r0.x (as test_gpu_shader_alu_vector.cpp); `extra` runs before the export.
std::vector<uint32_t> fullscreen_vs(std::vector<xe::Alu> extra = {}) {
  std::vector<xe::Alu> alus = {vop(xe::kVectorMul, 1, r(0, 0x6C), c(1)), vop(xe::kVectorMul, 2, r(0, 0x6C), r(0, 0x6C)),
                               vop(xe::kVectorMul, 2, r(2), c(2)), vop(xe::kVectorAdd, 1, r(1), r(2))};
  alus.insert(alus.end(), extra.begin(), extra.end());
  alus.push_back(vop(xe::kVectorAdd, sh::kExportVsPosition, r(1), c(0), true));
  return assemble({exec_end(alus)});
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
  s.precision(9);
  s << "(" << p[0] << ", " << p[1] << ", " << p[2] << ", " << p[3] << ")";
  return s.str();
}

bool same_bits(const Pixel& a, const Pixel& b) { return std::memcmp(a.data(), b.data(), sizeof(Pixel)) == 0; }

// Draws the full-screen triangle with the pixel shader `program` (and the vertex shader `vs`); returns pixel 0 of each
// colour target (every pixel of a target must be bit-identical to it).
std::array<Pixel, 4> draw(ShaderHarness& h, const std::vector<Cf>& program, HarnessInputs in,
                          const std::vector<uint32_t>& vs_code = fullscreen_vs()) {
  const sh::TranslatedShader ps = translate_ok(ShaderStage::Pixel, assemble(program));
  const sh::TranslatedShader vs = translate_ok(ShaderStage::Vertex, vs_code);
  if (in.vs_constants.empty()) in.vs_constants = fullscreen_constants();
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

// Exact: the same value and sign (NaN matches NaN). With a tolerance: relative to the expected magnitude.
bool matches(float got, float want, float tolerance) {
  if (std::isnan(want)) return std::isnan(got);
  if (tolerance == 0.0f) return got == want && std::signbit(got) == std::signbit(want);
  return std::fabs(got - want) <= tolerance * std::fmax(1.0f, std::fabs(want));
}

void check_pixel(const Pixel& got, const Pixel& want, float tolerance = 0.0f) {
  INFO("got " << text(got) << ", want " << text(want));
  for (uint32_t k = 0; k < 4; ++k) CHECK(matches(got[k], want[k], tolerance));
}

Pixel splat(float v) { return {v, v, v, v}; }

}  // namespace

// --- the op table -------------------------------------------------------------------------------------------------

TEST_CASE("scalar op table", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // ps = c2.w (MAXs of c2.wwww into r3); r2 = (1, 0, 0, 0); then r1 = op(c1) (a = c1.w, b = c1.x), or for the CONST
  // ops op(c1.w, r6.x / r7.x) with r6 = r7 = c6; r2 = (1, 1, 1, 1) through a predicated mov when p0 is set. oC0 = r1,
  // oC1 = r2 (oC1.x = 1 also says the pixel survived).
  struct Row {
    const char* name;
    uint32_t op;
    float a, b;
    float expected;
    bool p0 = false;
    float ps = 0.0f;
    float tolerance = 0.0f;
    uint32_t swizzle = 0;
  };
  const float kTol = 1e-6f;
  const float kTrig = 1e-3f;
  const std::vector<Row> rows = {
      {"ADDs", xe::kScalarAdd, 1.5f, 2, 3.5f},
      {"ADD_PREVs", xe::kScalarAddPrev, 1.5f, 99, 5.5f, false, 4},
      {"MULs", xe::kScalarMul, 3, 4, 12},
      {"MULs 0 * inf", xe::kScalarMul, 0, kInf, 0},
      {"MULs NaN * -0", xe::kScalarMul, kNan, -0.0f, 0},
      {"MULs inf * inf, same component", xe::kScalarMul, kInf, 0, kInf, false, 0, 0, kWWWW},
      {"MUL_PREVs", xe::kScalarMulPrev, 3, 99, -6, false, -2},
      {"MUL_PREVs 0 * inf", xe::kScalarMulPrev, 0, 99, 0, false, kInf},
      {"MUL_PREVs inf * 0", xe::kScalarMulPrev, kInf, 99, 0, false, 0},
      // a * ps unless ps is -FLT_MAX or not finite, or b is not finite or <= 0: then -FLT_MAX
      {"MUL_PREV2s", xe::kScalarMulPrev2, 3, 1, 6, false, 2},
      {"MUL_PREV2s 0 * ps", xe::kScalarMulPrev2, 0, 1, 0, false, 5},
      {"MUL_PREV2s b = 0", xe::kScalarMulPrev2, 3, 0, -kMax, false, 2},
      {"MUL_PREV2s b < 0", xe::kScalarMulPrev2, 3, -1, -kMax, false, 2},
      {"MUL_PREV2s b = inf", xe::kScalarMulPrev2, 3, kInf, -kMax, false, 2},
      {"MUL_PREV2s b = NaN", xe::kScalarMulPrev2, 3, kNan, -kMax, false, 2},
      {"MUL_PREV2s ps = inf", xe::kScalarMulPrev2, 0, 1, -kMax, false, kInf},
      {"MUL_PREV2s ps = -FLT_MAX", xe::kScalarMulPrev2, 0.5f, 1, -kMax, false, -kMax},
      {"MUL_PREV2s ps = NaN", xe::kScalarMulPrev2, 3, 1, -kMax, false, kNan},
      // a >= b ? a : b and a < b ? a : b: a NaN on either side picks b (FMax/FMin would not)
      {"MAXs", xe::kScalarMax, 1, 2, 2},
      {"MAXs a NaN", xe::kScalarMax, kNan, 2, 2},
      {"MAXs b NaN", xe::kScalarMax, 2, kNan, kNan},
      {"MAXs +0, -0", xe::kScalarMax, 0.0f, -0.0f, 0.0f},
      {"MAXs -0, +0", xe::kScalarMax, -0.0f, 0.0f, -0.0f},
      {"MINs", xe::kScalarMin, 1, 2, 1},
      {"MINs a NaN", xe::kScalarMin, kNan, 2, 2},
      {"MINs b NaN", xe::kScalarMin, 2, kNan, kNan},
      {"MINs +0, -0", xe::kScalarMin, 0.0f, -0.0f, -0.0f},  // ucode.h: +0 < -0 is false, so b
      {"SETEs", xe::kScalarSetEq, 0, 9, 1},
      {"SETEs -0", xe::kScalarSetEq, -0.0f, 9, 1},
      {"SETEs false", xe::kScalarSetEq, 2, 0, 0},
      {"SETEs NaN", xe::kScalarSetEq, kNan, 0, 0},
      {"SETGTs", xe::kScalarSetGt, 1, 0, 1},
      {"SETGTs false", xe::kScalarSetGt, 0, 9, 0},
      {"SETGTEs", xe::kScalarSetGe, 0, -9, 1},
      {"SETGTEs false", xe::kScalarSetGe, -1, 9, 0},
      {"SETNEs", xe::kScalarSetNe, 0, 9, 0},
      {"SETNEs NaN", xe::kScalarSetNe, kNan, 0, 1},
      {"FRACs", xe::kScalarFrac, -1.25f, 0, 0.75f},
      {"TRUNCs", xe::kScalarTrunc, -1.5f, 0, -1},
      {"FLOORs", xe::kScalarFloor, -1.5f, 0, -2},
      {"EXP_IEEE", xe::kScalarExp, 3, 0, 8, false, 0, kTol},
      {"EXP_IEEE 0", xe::kScalarExp, 0, 0, 1},
      {"EXP_IEEE -0", xe::kScalarExp, -0.0f, 0, 1},
      {"EXP_IEEE inf", xe::kScalarExp, kInf, 0, kInf},
      {"EXP_IEEE -inf", xe::kScalarExp, -kInf, 0, 0},
      {"LOG_CLAMP", xe::kScalarLogClamp, 8, 0, 3, false, 0, kTol},
      {"LOG_CLAMP 1", xe::kScalarLogClamp, 1, 0, 0},
      {"LOG_CLAMP 0", xe::kScalarLogClamp, 0, 0, -kMax},
      {"LOG_CLAMP -0", xe::kScalarLogClamp, -0.0f, 0, -kMax},
      {"LOG_CLAMP inf", xe::kScalarLogClamp, kInf, 0, kInf},
      {"LOG_IEEE", xe::kScalarLog, 8, 0, 3, false, 0, kTol},
      {"LOG_IEEE 1", xe::kScalarLog, 1, 0, 0},
      {"LOG_IEEE 0", xe::kScalarLog, 0, 0, -kInf},
      {"LOG_IEEE -0", xe::kScalarLog, -0.0f, 0, -kInf},
      {"LOG_IEEE inf", xe::kScalarLog, kInf, 0, kInf},
      {"LOG_IEEE -1", xe::kScalarLog, -1, 0, kNan},
      {"RECIP_CLAMP", xe::kScalarRecipClamp, 4, 0, 0.25f},
      {"RECIP_CLAMP 0", xe::kScalarRecipClamp, 0, 0, kMax},
      {"RECIP_CLAMP -0", xe::kScalarRecipClamp, -0.0f, 0, -kMax},
      {"RECIP_CLAMP inf", xe::kScalarRecipClamp, kInf, 0, 0},
      {"RECIP_CLAMP -inf", xe::kScalarRecipClamp, -kInf, 0, -0.0f},
      {"RECIP_CLAMP denormal", xe::kScalarRecipClamp, 1e-40f, 0, kMax},
      {"RECIP_IEEE inf", xe::kScalarRecip, kInf, 0, 0},
      {"RECIP_FF", xe::kScalarRecipFixedFunc, 4, 0, 0.25f},
      {"RECIP_FF 0", xe::kScalarRecipFixedFunc, 0, 0, 0},
      {"RECIP_FF -0", xe::kScalarRecipFixedFunc, -0.0f, 0, -0.0f},
      {"RECIP_FF inf", xe::kScalarRecipFixedFunc, kInf, 0, 0},
      {"RECIP_FF -inf", xe::kScalarRecipFixedFunc, -kInf, 0, -0.0f},
      {"RECIP_IEEE", xe::kScalarRecip, 4, 0, 0.25f},
      {"RECIP_IEEE 1", xe::kScalarRecip, 1, 0, 1},
      {"RECIP_IEEE 0", xe::kScalarRecip, 0, 0, kInf},
      {"RECIP_IEEE -0", xe::kScalarRecip, -0.0f, 0, -kInf},
      {"RECIP_IEEE -inf", xe::kScalarRecip, -kInf, 0, -0.0f},
      {"RECIPSQ_CLAMP", xe::kScalarRecipSqrtClamp, 4, 0, 0.5f, false, 0, kTol},
      {"RECIPSQ_CLAMP 0", xe::kScalarRecipSqrtClamp, 0, 0, kMax},
      {"RECIPSQ_CLAMP inf", xe::kScalarRecipSqrtClamp, kInf, 0, 0},
      {"RECIPSQ_CLAMP -0", xe::kScalarRecipSqrtClamp, -0.0f, 0, -kMax},
      {"RECIPSQ_FF", xe::kScalarRecipSqrtFixedFunc, 4, 0, 0.5f, false, 0, kTol},
      {"RECIPSQ_FF 0", xe::kScalarRecipSqrtFixedFunc, 0, 0, 0},
      {"RECIPSQ_FF inf", xe::kScalarRecipSqrtFixedFunc, kInf, 0, 0},
      {"RECIPSQ_FF -0", xe::kScalarRecipSqrtFixedFunc, -0.0f, 0, -0.0f},
      {"RECIPSQ_IEEE", xe::kScalarRecipSqrt, 4, 0, 0.5f, false, 0, kTol},
      {"RECIPSQ_IEEE 1", xe::kScalarRecipSqrt, 1, 0, 1},
      {"RECIPSQ_IEEE 0", xe::kScalarRecipSqrt, 0, 0, kInf},
      {"RECIPSQ_IEEE inf", xe::kScalarRecipSqrt, kInf, 0, 0},
      {"RECIPSQ_IEEE -0", xe::kScalarRecipSqrt, -0.0f, 0, -kInf},
      {"RECIPSQ_IEEE -4", xe::kScalarRecipSqrt, -4, 0, kNan},
      // max(a, b) (a0 in "mova rounds and mova_floor floors into a0")
      {"MOVAs", xe::kScalarMovA, 2.5f, 1, 2.5f},
      {"MOVAs b greater", xe::kScalarMovA, 2.5f, 7, 7},
      {"MOVA_FLOORs", xe::kScalarMovAFloor, -1.5f, -9, -1.5f},
      {"SUBs", xe::kScalarSub, 5, 2, 3},
      {"SUB_PREVs", xe::kScalarSubPrev, 5, 99, 3, false, 2},
      // p0 = a <op> 0; result 0 when it holds, else 1
      {"PRED_SETEs", xe::kScalarPredSetEq, 0, 9, 0, true},
      {"PRED_SETEs false", xe::kScalarPredSetEq, 2, 0, 1, false},
      {"PRED_SETNEs", xe::kScalarPredSetNe, 2, 0, 0, true},
      {"PRED_SETNEs NaN", xe::kScalarPredSetNe, kNan, 0, 0, true},
      {"PRED_SETNEs false", xe::kScalarPredSetNe, 0, 9, 1, false},
      {"PRED_SETGTs", xe::kScalarPredSetGt, 1, 0, 0, true},
      {"PRED_SETGTs false", xe::kScalarPredSetGt, 0, 9, 1, false},
      {"PRED_SETGTEs", xe::kScalarPredSetGe, 0, -9, 0, true},
      {"PRED_SETGTEs false", xe::kScalarPredSetGe, -1, 9, 1, false},
      // p0 = a == 1; result 0 then, else (a == 0 ? 1 : a)
      {"PRED_SET_INVs", xe::kScalarPredSetInv, 1, 0, 0, true},
      {"PRED_SET_INVs 0", xe::kScalarPredSetInv, 0, 9, 1, false},
      {"PRED_SET_INVs 3", xe::kScalarPredSetInv, 3, 9, 3, false},
      // p0 = a - 1 <= 0; result 0 then, else a - 1
      {"PRED_SET_POPs", xe::kScalarPredSetPop, 1, 9, 0, true},
      {"PRED_SET_POPs 0.5", xe::kScalarPredSetPop, 0.5f, 9, 0, true},
      {"PRED_SET_POPs 3", xe::kScalarPredSetPop, 3, 9, 2, false},
      // p0 = 0; result FLT_MAX
      {"PRED_SET_CLRs", xe::kScalarPredSetClr, 3, 9, kMax, false},
      // p0 = a == 0; result 0 then, else a
      {"PRED_SET_RESTOREs", xe::kScalarPredSetRestore, 0, 9, 0, true},
      {"PRED_SET_RESTOREs 5", xe::kScalarPredSetRestore, 5, 9, 5, false},
      // no kill: result 0 (the kills themselves are in "scalar kill discards the pixel")
      {"KILLEs", xe::kScalarKillEq, 1, 0, 0},
      {"KILLGTs", xe::kScalarKillGt, 0, 9, 0},
      {"KILLGTEs", xe::kScalarKillGe, -1, 9, 0},
      {"KILLNEs", xe::kScalarKillNe, 0, 9, 0},
      {"KILLONEs", xe::kScalarKillOne, 2, 1, 0},
      {"SQRT_IEEE", xe::kScalarSqrt, 16, 0, 4, false, 0, kTol},
      {"SQRT_IEEE 0", xe::kScalarSqrt, 0, 0, 0},
      {"SQRT_IEEE inf", xe::kScalarSqrt, kInf, 0, kInf},
      {"SQRT_IEEE -0", xe::kScalarSqrt, -0.0f, 0, -0.0f},
      {"SQRT_IEEE -4", xe::kScalarSqrt, -4, 0, kNan},
      // the CONST ops: a = the constant, b = the temporary
      {"MUL_CONST_0", xe::kScalarMulConst0, 3, 4, 12},
      {"MUL_CONST_1", xe::kScalarMulConst1, 0, kInf, 0},
      {"ADD_CONST_0", xe::kScalarAddConst0, 3, 4, 7},
      {"ADD_CONST_1", xe::kScalarAddConst1, 3, 4, 7},
      {"SUB_CONST_0", xe::kScalarSubConst0, 3, 4, -1},
      {"SUB_CONST_1", xe::kScalarSubConst1, 3, 4, -1},
      // radians, no scaling; Vulkan only guarantees an absolute error of 2^-11, so 1e-3
      {"SIN", xe::kScalarSin, 0, 0, 0, false, 0, kTrig},
      {"SIN pi/2", xe::kScalarSin, kPi / 2, 0, 1, false, 0, kTrig},
      {"COS", xe::kScalarCos, 0, 0, 1, false, 0, kTrig},
      {"COS pi", xe::kScalarCos, kPi, 0, -1, false, 0, kTrig},
      {"RETAIN_PREV", xe::kScalarRetainPrev, 3, 4, 7, false, 7},
  };
  for (const Row& row : rows) {
    INFO(row.name);
    HarnessInputs in;
    in.color_targets = 2;
    in.ps_constants = {{0, 0, 0, 0}, {row.b, 0, 0, row.a}, {0, 0, 0, row.ps}, {}, {1, 1, 1, 1}, {1, 0, 0, 0},
                       {row.b, 0, 0, 0}};
    const bool is_const = row.op >= xe::kScalarMulConst0 && row.op <= xe::kScalarSubConst1;
    const xe::Alu op = is_const ? sconst(row.op & ~1u, 1, 1, 6 | (row.op & 1)) : sop(row.op, 1, c(1, row.swizzle));
    const std::vector<Cf> program = {
        exec_end({mov(6, c(6)), mov(7, c(6)), sop(xe::kScalarMax, 3, c(2, kWWWW)), mov(2, c(5)), op,
                  predicated(mov(2, c(4)), true), mov(0, r(1), true), mov(1, r(2), true)})};
    const auto out = draw(h, program, in);
    check_pixel(out[0], splat(row.expected), row.tolerance);
    const float p = row.p0 ? 1.0f : 0.0f;
    check_pixel(out[1], {1, p, p, p});
  }
}

// --- state: ps, p0, a0 --------------------------------------------------------------------------------------------

TEST_CASE("previous scalar result carries across instructions", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  HarnessInputs in;
  in.color_targets = 4;
  in.ps_constants = {{0, 0, 0, 0}, {3, 0, 0, 2}, {0, 0, 0, 10}, {0, 0, 0, 0.25f}};
  const std::vector<Cf> program = {
      // ps = 2 + 3, though nothing is written; a vector-only instruction keeps it.
      exec({sop(xe::kScalarAdd, 1, c(1), 0), mov(5, c(0))}),
      // a later clause (each result is the next ps): r1 = 10 + 5 = 15, r2 = 10 * 15 = 150, r3 = ps = 150
      exec({sop(xe::kScalarAddPrev, 1, c(2)), sop(xe::kScalarMulPrev, 2, c(2)), sop(xe::kScalarRetainPrev, 3, c(0)),
            // a predicated-off instruction (p0 is 0) leaves ps alone
            predicated(sop(xe::kScalarAdd, 4, c(1)), true),
            // saturate clamps the written value only, as in Xenia: r4.y = clamp(0.25 * 150, 0, 1) = 1, r4.x = ps = 37.5
            saturated(sop(xe::kScalarMulPrev, 4, c(3), 0x2)), sop(xe::kScalarRetainPrev, 4, c(0), 0x1)}),
      exec_end({mov(0, r(1), true), mov(1, r(2), true), mov(2, r(3), true), mov(3, r(4), true)})};
  const auto out = draw(h, program, in);
  check_pixel(out[0], splat(15));
  check_pixel(out[1], splat(150));
  check_pixel(out[2], splat(150));
  check_pixel(out[3], {37.5f, 1, 0, 0});
}

TEST_CASE("pred_set ops set p0 and the scalar result", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // Each case: a chain of scalar predicate ops on c1..c3 (results to r1), then oC0 = r1 and oC1 = (1, p0, p0, p0)
  // through a predicated mov.
  struct Case {
    const char* name;
    std::vector<xe::Alu> chain;
    float result;
    bool p0;
  };
  // c1.w = 0, c2.w = 1, c3.w = 3
  const std::vector<Case> cases = {
      {"SETEs sets, CLRs clears", {sop(xe::kScalarPredSetEq, 1, c(1)), sop(xe::kScalarPredSetClr, 1, c(1))}, kMax,
       false},
      {"POPs counts down", {sop(xe::kScalarPredSetPop, 1, c(3)), sop(xe::kScalarPredSetPop, 1, r(1, kWWWW))}, 1,
       false},
      {"POPs reaches zero",
       {sop(xe::kScalarPredSetPop, 1, c(3)), sop(xe::kScalarPredSetPop, 1, r(1)), sop(xe::kScalarPredSetPop, 1, r(1))},
       0, true},
      {"INVs of 1", {sop(xe::kScalarPredSetInv, 1, c(2))}, 0, true},
      {"RESTOREs of 0 after a clear", {sop(xe::kScalarPredSetClr, 1, c(1)), sop(xe::kScalarPredSetRestore, 1, c(1))},
       0, true},
      {"a zero write mask still sets p0", {sop(xe::kScalarPredSetEq, 1, c(1), 0)}, 0, true},
  };
  for (const Case& k : cases) {
    INFO(k.name);
    HarnessInputs in;
    in.color_targets = 2;
    in.ps_constants = {{0, 0, 0, 0}, {9, 9, 9, 0}, {9, 9, 9, 1}, {9, 9, 9, 3}, {1, 1, 1, 1}, {1, 0, 0, 0}};
    std::vector<xe::Alu> alus = k.chain;
    alus.push_back(mov(2, c(5)));
    alus.push_back(predicated(mov(2, c(4)), true));
    alus.push_back(mov(0, r(1), true));
    alus.push_back(mov(1, r(2), true));
    const auto out = draw(h, {exec_end(alus)}, in);
    check_pixel(out[0], splat(k.result));
    const float p = k.p0 ? 1.0f : 0.0f;
    check_pixel(out[1], {1, p, p, p});
  }
  {
    INFO("the scalar p0 wins over the vector push in the same instruction");
    HarnessInputs in;
    in.color_targets = 2;
    in.ps_constants = {{0, 0, 0, 0}, {9, 9, 9, 0}, {9, 9, 9, 1}, {9, 9, 9, 3}, {1, 1, 1, 1}, {1, 0, 0, 0}};
    // vector PRED_SETE_PUSH(c0, c0) sets p0 (0 == 0); scalar PRED_SETEs(c2.w = 1) clears it.
    xe::Alu both = vop(xe::kVectorPredSetEqPush, 3, c(0), c(0));
    both.vector_mask = 0;
    both.scalar_op = xe::kScalarPredSetEq;
    both.scalar_dest = 1;
    both.scalar_mask = 0xF;
    both.src3 = c(2);
    const auto out = draw(
        h, {exec_end({both, mov(2, c(5)), predicated(mov(2, c(4)), true), mov(0, r(1), true), mov(1, r(2), true)})},
        in);
    check_pixel(out[0], splat(1));
    check_pixel(out[1], {1, 0, 0, 0});
  }
}

namespace {
// c[k] = k + 1 for every k, except c250, the MOVA operand.
std::vector<Pixel> index_constants(Pixel a) {
  std::vector<Pixel> out;
  for (uint32_t k = 0; k < sh::kFloatConstantCount; ++k) {
    const auto v = static_cast<float>(k + 1);
    out.push_back({v, v, v, v});
  }
  out[250] = a;
  return out;
}
}  // namespace

TEST_CASE("mova rounds and mova_floor floors into a0", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // r1 = mova(c250.ww); oC0 = r1; oC1..3 = c[a0 + 20], c[a0 + 0], c[a0 + 255] (k + 1, or 0 out of range).
  struct Case {
    bool floor;
    float w;
    int a0;
  };
  const auto read = [](int index) { return index >= 0 && index <= 255 ? static_cast<float>(index + 1) : 0.0f; };
  for (const Case& k : {Case{false, 2.5f, 3}, Case{false, 0.5f, 1}, Case{false, -0.5f, 0}, Case{false, 1.49f, 1},
                        Case{false, -1.5f, -1}, Case{false, 1000, 255}, Case{false, -255.2f, -255},
                        Case{false, -1000, -256}, Case{true, 2.5f, 2}, Case{true, 2.99f, 2}, Case{true, -0.5f, -1},
                        Case{true, -1.5f, -2}, Case{true, 255.9f, 255}, Case{true, 300, 255}, Case{true, -256.5f, -256},
                        Case{true, -kInf, -256}}) {
    INFO((k.floor ? "MOVA_FLOORs " : "MOVAs ") << k.w);
    const xe::Alu mova = sop(k.floor ? xe::kScalarMovAFloor : xe::kScalarMovA, 1, c(250, kWWWW));
    xe::Alu rel20 = mov(1, c(20), true), rel0 = mov(2, c(0), true), rel255 = mov(3, c(255), true);
    for (xe::Alu* a : {&rel20, &rel0, &rel255}) {
      a->const0_relative = a->const1_relative = true;
      a->address_register_relative = true;
    }
    HarnessInputs in;
    in.color_targets = 4;
    in.ps_constants = index_constants({0, 0, 0, k.w});
    const auto out = draw(h, {exec_end({mova, mov(0, r(1), true), rel20, rel0, rel255})}, in);
    check_pixel(out[0], splat(k.w));
    check_pixel(out[1], splat(read(k.a0 + 20)));
    check_pixel(out[2], splat(read(k.a0)));
    check_pixel(out[3], splat(read(k.a0 + 255)));
  }
}

// --- the Xenos multiply rule --------------------------------------------------------------------------------------

TEST_CASE("mul_const of zero by infinity is zero", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // a = the constant's w, b = r6.x / r7.x (from c<n+10>); results to oC0..3 x.
  HarnessInputs in;
  in.color_targets = 4;
  in.ps_constants.resize(20);
  const auto set = [&](uint32_t k, float constant, float temp) {
    in.ps_constants[k] = {0, 0, 0, constant};
    in.ps_constants[k + 10] = {temp, 0, 0, 0};
  };
  set(1, 0, kInf);       // 0 * inf: +0
  set(2, kInf, -0.0f);   // inf * -0: +0
  set(3, -0.0f, kNan);   // -0 * NaN: +0
  set(4, -2, 3);         // -6
  xe::Alu negated = sconst(xe::kScalarMulConst0, 4, 5, 6);  // -(2) * -(-3): the negate applies to both: -6
  negated.src3.negate = true;
  set(5, 2, -3);
  xe::Alu absolute = sconst(xe::kScalarMulConst0, 4, 7, 6, 0x2);  // |-2| * |-3| = 6, into r4.y
  absolute.abs_constants = true;
  set(7, -2, -3);
  const std::vector<Cf> program = {exec_end({
      mov(6, c(11)), sconst(xe::kScalarMulConst0, 1, 1, 6, 0x1),   // r1.x
      mov(7, c(12)), sconst(xe::kScalarMulConst1, 1, 2, 7, 0x2),   // r1.y
      mov(6, c(13)), sconst(xe::kScalarMulConst0, 1, 3, 6, 0x4),   // r1.z
      mov(7, c(14)), sconst(xe::kScalarMulConst1, 1, 4, 7, 0x8),   // r1.w
      mov(6, c(15)), negated,                                      // r4 = -6
      mov(6, c(17)), absolute,                                     // r4.y = 6
      sop(xe::kScalarMul, 2, c(11)),                               // c11: 0 * inf (different components): +0
      sop(xe::kScalarMul, 3, c(2, kWWWW)),                         // inf * inf (the same component): inf
      mov(0, r(1), true), mov(1, r(2), true), mov(2, r(3), true), mov(3, r(4), true)})};
  const auto out = draw(h, program, in);
  check_pixel(out[0], {0, 0, 0, -6});
  check_pixel(out[1], splat(0));
  check_pixel(out[2], splat(kInf));
  check_pixel(out[3], {-6, 6, -6, -6});
}

// --- merging and kills --------------------------------------------------------------------------------------------

TEST_CASE("vector and scalar results merge by write mask", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  HarnessInputs in;
  in.color_targets = 3;
  in.ps_constants = {{0, 0, 0, 0}, {1, 2, 3, 4}, {10, 20, 30, 40}, {5, 0, 0, 7}};
  // One instruction: vector r1.xy = c1 + c2, scalar r1.zw = c3.w + c3.x = 12.
  xe::Alu both = vop(xe::kVectorAdd, 1, c(1), c(2));
  both.vector_mask = 0x3;
  both.scalar_op = xe::kScalarAdd;
  both.scalar_dest = 1;
  both.scalar_mask = 0xC;
  both.src3 = c(3);
  // Different registers: vector r2.xz, scalar r3.yw; r2 and r3 start as c1.
  xe::Alu apart = both;
  apart.vector_dest = 2;
  apart.vector_mask = 0x5;
  apart.scalar_dest = 3;
  apart.scalar_mask = 0xA;
  // An export: oC2.x from the vector unit, .y from the scalar unit, .zw neither (stay 0).
  xe::Alu exported = both;
  exported.export_data = true;
  exported.vector_dest = 2;
  exported.vector_mask = 0x1;
  exported.scalar_mask = 0x2;
  const auto out = draw(h,
                        {exec_end({mov(1, c(1)), mov(2, c(1)), mov(3, c(1)), both, apart, mov(0, r(1), true),
                                   vop(xe::kVectorMax, 1, r(2), r(2), true), exported})},
                        in);
  check_pixel(out[0], {11, 22, 12, 12});
  check_pixel(out[1], {11, 2, 33, 4});
  check_pixel(out[2], {11, 12, 0, 0});
}

TEST_CASE("scalar kill discards the pixel", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // kill(c1.w) with a zero write mask, then oC0 = c3 = 1: a killed pixel keeps the clear colour 0.
  struct Case {
    const char* name;
    uint32_t op;
    float a;
    bool kills;
  };
  const std::vector<Case> cases = {
      {"KILLEs", xe::kScalarKillEq, 0, true},         {"KILLEs -0", xe::kScalarKillEq, -0.0f, true},
      {"KILLEs none", xe::kScalarKillEq, 1, false},   {"KILLEs NaN", xe::kScalarKillEq, kNan, false},
      {"KILLGTs", xe::kScalarKillGt, 1, true},        {"KILLGTs none", xe::kScalarKillGt, 0, false},
      {"KILLGTEs", xe::kScalarKillGe, 0, true},       {"KILLGTEs none", xe::kScalarKillGe, -1, false},
      {"KILLNEs", xe::kScalarKillNe, 2, true},        {"KILLNEs NaN", xe::kScalarKillNe, kNan, true},
      {"KILLNEs none", xe::kScalarKillNe, 0, false},  {"KILLONEs", xe::kScalarKillOne, 1, true},
      {"KILLONEs none", xe::kScalarKillOne, 0, false},
  };
  for (const Case& k : cases) {
    INFO(k.name);
    HarnessInputs in;
    in.ps_constants = {{0, 0, 0, 0}, {9, 9, 9, k.a}, {}, {1, 1, 1, 1}};
    const float v = k.kills ? 0.0f : 1.0f;
    check_pixel(draw(h, {exec_end({sop(k.op, 1, c(1), 0), mov(0, c(3), true)})}, in)[0], splat(v));
  }
  {
    // The result is 1.0 when it kills (ucode.h; Xenia returns 0.0). In a vertex shader a kill discards nothing and
    // only gives its result: KILLEs(0) = 1 and KILLGTs(0) = 0 go to interpolator 0 x and y; the pixel shader exports
    // floor(r0 + 0.5), so the interpolation's rounding cannot blur the value.
    INFO("the kill result is 1.0 when it kills");
    HarnessInputs in;
    in.vs_constants = fullscreen_constants();
    in.vs_constants.push_back({0, 0, 0, 0});  // c3
    in.ps_constants = {{0, 0, 0, 0}, {0.5f, 0.5f, 0.5f, 0.5f}};
    const std::vector<uint32_t> vs = fullscreen_vs({sop(xe::kScalarKillEq, 3, c(3), 0x1),
                                                    sop(xe::kScalarKillGt, 3, c(3), 0x2), mov(0, r(3), true)});
    const std::vector<Cf> ps = {
        exec_end({vop(xe::kVectorAdd, 1, r(0), c(1)), vop(xe::kVectorFloor, 0, r(1), r(1), true)})};
    check_pixel(draw(h, ps, in, vs)[0], {1, 0, 0, 0});
  }
}

// --- invariance ---------------------------------------------------------------------------------------------------

TEST_CASE("alu arithmetic is NoContraction and the position is invariant", "[unit]") {
  if (!lr::testing::spirv_tools_available()) SKIP("spirv-dis is not available");
  // A vertex shader with vector and scalar arithmetic (add, mul, muladd, dot, negate, sub, rcp, the prev ops) and the
  // position transform: every float add, sub, mul, div and negate result is decorated NoContraction.
  xe::Alu mad = vop(xe::kVectorMulAdd, 4, r(1), neg(c(3)));
  mad.src3 = c(4);
  const std::vector<uint32_t> code = fullscreen_vs(
      {mad, vop(xe::kVectorDot4, 5, r(4), c(3)), sop(xe::kScalarSub, 6, c(5)), sop(xe::kScalarMulPrev, 6, c(5)),
       sop(xe::kScalarRecip, 6, r(6)), sop(xe::kScalarAddPrev, 6, r(6)), sconst(xe::kScalarMulConst0, 6, 3, 6),
       mov(0, r(6), true)});
  const auto vs = translate_ok(ShaderStage::Vertex, code);
  const std::string dis = lr::testing::spirv_dis(vs.spirv);
  INFO(dis);
  const std::regex arith(R"((%\d+) = (OpFAdd|OpFSub|OpFMul|OpFDiv|OpFNegate|OpVectorTimesScalar) )");
  const std::regex no_contraction(R"(OpDecorate (%\d+) NoContraction)");
  std::set<std::string> decorated;
  for (auto it = std::sregex_iterator(dis.begin(), dis.end(), no_contraction); it != std::sregex_iterator(); ++it)
    decorated.insert((*it)[1]);
  size_t count = 0;
  for (auto it = std::sregex_iterator(dis.begin(), dis.end(), arith); it != std::sregex_iterator(); ++it) {
    ++count;
    INFO((*it)[0]);
    CHECK(decorated.count((*it)[1]) == 1);
  }
  CHECK(count >= 10);
  std::smatch position;
  REQUIRE(std::regex_search(dis, position, std::regex(R"(OpDecorate (%\d+) BuiltIn Position)")));
  CHECK(dis.find("OpDecorate " + position[1].str() + " Invariant") != std::string::npos);
}
