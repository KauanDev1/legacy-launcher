// SPDX-License-Identifier: MIT
// Control flow of the plan B translator (translator_cf.cpp): exec conditions, the predicate p0, loops and aL, jumps,
// calls and the iteration cap of the pc-switch loop. Shaders are synthetic microcode (support/xenos_shader_builder.h);
// every module goes through spirv-val, and the value tests run on lavapipe through support/vk_shader_harness.h.
//
// Most tests are pixel shaders that accumulate into r1 and export it to oC0. The relative-temp tests first load
// r10..r17 with 1, 2, 4, ..., 128 (c1..c8), so the sum of the registers a loop visits through r[aL] names them.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
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

constexpr uint32_t kXXXX = 0x6C;  // source swizzle .xxxx
constexpr uint32_t kScalarPredSetGe = 30;

// --- microcode ----------------------------------------------------------------------------------------------------

xe::AluSrc r(uint32_t reg, uint32_t swizzle = 0) { return {reg, true, swizzle}; }
xe::AluSrc c(uint32_t reg, uint32_t swizzle = 0) { return {reg, false, swizzle}; }
// r[aL + reg]
xe::AluSrc rel(uint32_t reg) {
  xe::AluSrc s{reg, true, 0};
  s.relative = true;
  return s;
}

xe::Alu vector_op(uint32_t op, uint32_t dest, xe::AluSrc a, xe::AluSrc b, bool to_export) {
  xe::Alu alu;
  alu.vector_op = op;
  alu.vector_dest = dest;
  alu.export_data = to_export;
  alu.src1 = a;
  alu.src2 = b;
  return alu;
}
xe::Alu mov(uint32_t dest, xe::AluSrc s, bool to_export = false) {
  return vector_op(xe::kVectorMax, dest, s, s, to_export);
}
xe::Alu add(uint32_t dest, xe::AluSrc a, xe::AluSrc b, bool to_export = false) {
  return vector_op(xe::kVectorAdd, dest, a, b, to_export);
}
xe::Alu mul(uint32_t dest, xe::AluSrc a, xe::AluSrc b, bool to_export = false) {
  return vector_op(xe::kVectorMul, dest, a, b, to_export);
}
// A scalar predicate set on s.w (no register written).
xe::Alu pred_set(uint32_t scalar_op, xe::AluSrc s) {
  xe::Alu alu;
  alu.vector_mask = 0;
  alu.scalar_op = scalar_op;
  alu.src3 = s;
  return alu;
}
xe::Alu predicated(xe::Alu alu, bool condition) {
  alu.predicated = true;
  alu.pred_condition = condition;
  return alu;
}

// One CF instruction; exec forms carry their clause, and `make` gets the clause's slot address.
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
Cf cond_exec(uint32_t b, bool condition, std::vector<xe::Alu> alus, bool end = false) {
  const auto n = static_cast<uint32_t>(alus.size());
  return {0, [=](uint32_t a) { return xe::cf_cond_exec(end, a, n, 0, b, condition); }, std::move(alus)};
}
Cf cond_exec_pred_clean(uint32_t b, bool condition, std::vector<xe::Alu> alus) {
  const auto n = static_cast<uint32_t>(alus.size());
  return {0, [=](uint32_t a) { return xe::cf_cond_exec_pred_clean(false, a, n, 0, b, condition); }, std::move(alus)};
}
Cf cond_exec_pred(bool condition, std::vector<xe::Alu> alus) {
  const auto n = static_cast<uint32_t>(alus.size());
  return {0, [=](uint32_t a) { return xe::cf_cond_exec_pred(false, a, n, 0, condition); }, std::move(alus)};
}

// The CF program, padded to whole pairs, then every clause's ALU instructions in CF order.
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

// r10..r17 = c1..c8 (1, 2, ..., 128) and r1 = c0 (0): two clauses.
std::vector<Cf> load_ladder() {
  return {exec({mov(10, c(1)), mov(11, c(2)), mov(12, c(3)), mov(13, c(4)), mov(14, c(5)), mov(15, c(6))}),
          exec({mov(16, c(7)), mov(17, c(8)), mov(1, c(0))})};
}

std::vector<Cf> concat(std::vector<Cf> a, const std::vector<Cf>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

uint32_t loop_constant(uint32_t count, uint32_t start, int32_t step) {
  return (count & 0xFF) | ((start & 0xFF) << 8) | ((static_cast<uint32_t>(step) & 0xFF) << 16);
}

// --- the vertex shader: one triangle over the whole target --------------------------------------------------------

std::vector<uint32_t> fullscreen_vs() {
  // export 62 = c0 + c1 * i + c2 * i * i, i = r0.x (as test_gpu_shader_translate.cpp).
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

lr::Result<sh::TranslatedShader> try_translate(ShaderStage stage, const std::vector<uint32_t>& code) {
  auto program = sh::decode(stage, code);
  if (!program) return program.error();
  return sh::translate(program.value(), {});
}

sh::TranslatedShader translate_ok(ShaderStage stage, const std::vector<uint32_t>& code) {
  auto t = try_translate(stage, code);
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

std::string pixel_text(const Pixel& p) {
  std::ostringstream s;
  s << "(" << p[0] << ", " << p[1] << ", " << p[2] << ", " << p[3] << ")";
  return s.str();
}

// PS constants: c0 = 0, c1..c8 = 1, 2, ..., 128, then `extra` from c9.
std::vector<Pixel> ladder_constants(std::vector<Pixel> extra = {}) {
  std::vector<Pixel> out{{0, 0, 0, 0}};
  for (uint32_t k = 0; k < 8; ++k) {
    const auto v = static_cast<float>(1u << k);
    out.push_back({v, v, v, v});
  }
  out.insert(out.end(), extra.begin(), extra.end());
  return out;
}

// Draws the full-screen triangle with `ps` and returns the value every pixel of oC0 has (all four components).
float draw_value(ShaderHarness& h, const sh::TranslatedShader& ps, HarnessInputs in) {
  const sh::TranslatedShader vs = translate_ok(ShaderStage::Vertex, fullscreen_vs());
  in.vs_constants = fullscreen_constants();
  auto res = h.draw(vs.spirv, ps.spirv, in);
  if (!res) FAIL("draw: " << res.error().message);
  const HarnessResult& out = res.value();
  REQUIRE(out.color[0].size() == in.width);
  const Pixel first = out.color[0][0];
  for (const Pixel& p : out.color[0]) {
    INFO(pixel_text(p));
    REQUIRE(p == Pixel{first[0], first[0], first[0], first[0]});
  }
  return first[0];
}

float run_ps(ShaderHarness& h, const std::vector<Cf>& program, HarnessInputs in) {
  return draw_value(h, translate_ok(ShaderStage::Pixel, assemble(program)), std::move(in));
}

std::string disassemble(const std::vector<uint32_t>& code) {
  return lr::testing::spirv_dis(translate_ok(ShaderStage::Pixel, code).spirv);
}

}  // namespace

// --- exec conditions and the predicate ----------------------------------------------------------------------------

TEST_CASE("cond_exec follows bool constant 130", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  HarnessInputs in;
  in.ps_constants = ladder_constants();
  // Bool 130 is bit 2 of word 4. The middle clause sets r1 = 2 over r1 = 1 when it runs.
  const auto check = [&](const Cf& middle, bool runs_when_set) {
    const std::vector<Cf> program = {exec({mov(1, c(1))}), middle, exec_end({mov(0, r(1), true)})};
    for (const bool set : {false, true}) {
      INFO("bool 130 = " << set);
      in.bools = {};
      if (set) in.bools[4] = 1u << 2;
      CHECK(run_ps(h, program, in) == (set == runs_when_set ? 2.0f : 1.0f));
    }
  };
  SECTION("cond_exec") { check(cond_exec(130, true, {mov(1, c(2))}), true); }
  SECTION("cond_exec on false") { check(cond_exec(130, false, {mov(1, c(2))}), false); }
  SECTION("cond_exec_pred_clean takes a bool constant too") {
    check(cond_exec_pred_clean(130, true, {mov(1, c(2))}), true);
  }
}

TEST_CASE("cond_exec_end ends the shader only when its clause runs", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  HarnessInputs in;
  in.ps_constants = ladder_constants();
  const std::vector<Cf> body = {cond_exec(130, true, {mov(0, c(2), true)}, true), exec_end({mov(0, c(3), true)})};
  for (const bool switch_form : {false, true}) {
    // A forward jmp to the next instruction forces the pc-switch form without changing the program.
    const std::vector<Cf> program = switch_form ? concat({raw(xe::cf_jmp(1))}, body) : body;
    for (const bool set : {false, true}) {
      INFO("switch form " << switch_form << ", bool 130 = " << set);
      in.bools = {};
      if (set) in.bools[4] = 1u << 2;
      CHECK(run_ps(h, program, in) == (set ? 2.0f : 4.0f));
    }
  }
}

TEST_CASE("cond_exec_pred skips when the predicate is false", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // p0 = (c9.w == 0), set in one clause and read by the next (p0 lives across clauses).
  const std::vector<Cf> program = {exec({pred_set(xe::kScalarPredSetEq, c(9)), mov(1, c(1))}),
                                   cond_exec_pred(true, {mov(1, c(2))}), exec_end({mov(0, r(1), true)})};
  HarnessInputs in;
  in.ps_constants = ladder_constants({{0, 0, 0, 1}});
  CHECK(run_ps(h, program, in) == 1.0f);  // c9.w = 1: p0 false, the clause is skipped
  in.ps_constants = ladder_constants({{0, 0, 0, 0}});
  CHECK(run_ps(h, program, in) == 2.0f);  // c9.w = 0: p0 true
}

TEST_CASE("predicated alu instruction", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  const std::vector<Cf> program = {
      exec({mov(1, c(2)), pred_set(xe::kScalarPredSetEq, c(9)), predicated(mov(1, c(3)), true),
            predicated(mov(1, c(4)), false)}),
      exec_end({mov(0, r(1), true)})};
  HarnessInputs in;
  in.ps_constants = ladder_constants({{0, 0, 0, 0}});
  CHECK(run_ps(h, program, in) == 4.0f);  // p0 true: only the pred_condition = true instruction runs
  in.ps_constants = ladder_constants({{0, 0, 0, 1}});
  CHECK(run_ps(h, program, in) == 8.0f);  // p0 false: only the pred_condition = false instruction runs
}

// --- loops --------------------------------------------------------------------------------------------------------

TEST_CASE("loop runs the count of loop constant 3 with its start and step", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // 2 loop_start 3 (skip to 5); 3 r1 += r[aL]; 4 loop_end 3 (body 3); 5 export r1.
  const auto ps = translate_ok(
      ShaderStage::Pixel,
      assemble(concat(load_ladder(), {raw(xe::cf_loop_start(3, 5)), exec({add(1, r(1), rel(0))}),
                                      raw(xe::cf_loop_end(3, 3)), exec_end({mov(0, r(1), true)})})));
  HarnessInputs in;
  in.ps_constants = ladder_constants();
  struct Case {
    uint32_t count, start;
    int32_t step;
    float expected;
  };
  for (const Case& k : {Case{3, 10, 1, 1 + 2 + 4}, Case{2, 11, 2, 2 + 8}, Case{3, 14, -2, 16 + 4 + 1},
                        Case{0, 10, 1, 0}, Case{1, 17, 1, 128}}) {
    INFO("count " << k.count << " start " << k.start << " step " << k.step);
    in.loops = {};
    in.loops[3] = loop_constant(k.count, k.start, k.step);
    CHECK(draw_value(h, ps, in) == k.expected);
  }
}

TEST_CASE("is_repeat loop keeps aL", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // Outer loop 0 sets aL = 12; the inner repeat loop 1 (start 10 in its constant) keeps it: r12 + r13.
  auto program = [](bool repeat) {
    return concat(load_ladder(), {raw(xe::cf_loop_start(0, 7)), raw(xe::cf_loop_start(1, 6, repeat)),
                                  exec({add(1, r(1), rel(0))}), raw(xe::cf_loop_end(1, 4)),
                                  raw(xe::cf_loop_end(0, 3)), exec_end({mov(0, r(1), true)})});
  };
  HarnessInputs in;
  in.ps_constants = ladder_constants();
  in.loops[0] = loop_constant(1, 12, 0);
  in.loops[1] = loop_constant(2, 10, 1);
  CHECK(run_ps(h, program(true), in) == 4.0f + 8.0f);
  CHECK(run_ps(h, program(false), in) == 1.0f + 2.0f);
}

TEST_CASE("nested loops restore aL", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // Outer loop 0: aL = 10, 14. Each iteration runs the inner loop 1 once (aL = 12), then reads r[aL] with the outer
  // aL restored: (4 + 1) + (4 + 16).
  const std::vector<Cf> program =
      concat(load_ladder(), {raw(xe::cf_loop_start(0, 8)), raw(xe::cf_loop_start(1, 6)), exec({add(1, r(1), rel(0))}),
                             raw(xe::cf_loop_end(1, 4)), exec({add(1, r(1), rel(0))}), raw(xe::cf_loop_end(0, 3)),
                             exec_end({mov(0, r(1), true)})});
  HarnessInputs in;
  in.ps_constants = ladder_constants();
  in.loops[0] = loop_constant(2, 10, 4);
  in.loops[1] = loop_constant(1, 12, 1);
  CHECK(run_ps(h, program, in) == 25.0f);
}

TEST_CASE("loop_end with a predicated break exits and restores aL", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // Inner loop 1 (count 5, aL = 12, 13, ...) counts r2 up from -2 and sets p0 = r2 >= 0, which breaks it after two
  // iterations (r12 + r13). The outer loop then reads r[aL] with its own aL (10) restored: 4 + 8 + 1.
  const std::vector<Cf> program = concat(
      load_ladder(),
      {exec({mov(2, c(9))}), raw(xe::cf_loop_start(0, 9)), raw(xe::cf_loop_start(1, 7)),
       exec({add(1, r(1), rel(0)), add(2, r(2), c(10)), pred_set(kScalarPredSetGe, r(2))}),
       raw(xe::cf_loop_end(1, 5, true, true)), exec({add(1, r(1), rel(0))}), raw(xe::cf_loop_end(0, 4)),
       exec_end({mov(0, r(1), true)})});
  HarnessInputs in;
  in.ps_constants = ladder_constants({{-2, -2, -2, -2}, {1, 1, 1, 1}});
  in.loops[0] = loop_constant(1, 10, 0);
  in.loops[1] = loop_constant(5, 12, 1);
  CHECK(run_ps(h, program, in) == 13.0f);
}

TEST_CASE("r[aL+1] read inside a loop", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  const auto ps = translate_ok(
      ShaderStage::Pixel,
      assemble(concat(load_ladder(), {exec({mov(63, c(9))}), raw(xe::cf_loop_start(0, 6)),
                                      exec({add(1, r(1), rel(1))}), raw(xe::cf_loop_end(0, 4)),
                                      exec_end({mov(0, r(1), true)})})));
  HarnessInputs in;
  in.ps_constants = ladder_constants({{1000, 1000, 1000, 1000}});
  in.loops[0] = loop_constant(3, 10, 1);
  CHECK(draw_value(h, ps, in) == 2.0f + 4.0f + 8.0f);  // r11, r12, r13
  in.loops[0] = loop_constant(1, 200, 0);
  CHECK(draw_value(h, ps, in) == 1000.0f);  // aL + 1 = 201 clamps to r63
}

// --- jumps and calls ----------------------------------------------------------------------------------------------

TEST_CASE("jmp skips a clause", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  HarnessInputs in;
  in.ps_constants = ladder_constants();
  CHECK(run_ps(h, {exec({mov(1, c(1))}), raw(xe::cf_jmp(3)), exec({mov(1, c(2))}), exec_end({mov(0, r(1), true)})},
               in) == 1.0f);
}

TEST_CASE("cond_jmp on a bool constant", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  const std::vector<Cf> program = {exec({mov(1, c(1))}), raw(xe::cf_cond_jmp(3, 5, true)), exec({mov(1, c(2))}),
                                   exec_end({mov(0, r(1), true)})};
  HarnessInputs in;
  in.ps_constants = ladder_constants();
  in.bools[0] = 1u << 5;
  CHECK(run_ps(h, program, in) == 1.0f);  // taken
  in.bools[0] = 0;
  CHECK(run_ps(h, program, in) == 2.0f);  // not taken
}

TEST_CASE("call and return", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  HarnessInputs in;
  in.ps_constants = ladder_constants();
  SECTION("one call: (1 * 4) + 8") {
    const std::vector<Cf> program = {exec({mov(1, c(1))}), raw(xe::cf_call(3)),
                                     exec_end({add(1, r(1), c(4)), mov(0, r(1), true)}), exec({mul(1, r(1), c(3))}),
                                     raw(xe::cf_ret())};
    CHECK(run_ps(h, program, in) == 12.0f);
  }
  SECTION("nested calls: (1 + 2) * 4, then 8 added after both return") {
    const std::vector<Cf> program = {exec({mov(1, c(1))}),
                                     raw(xe::cf_call(3)),
                                     exec_end({add(1, r(1), c(4)), mov(0, r(1), true)}),
                                     exec({add(1, r(1), c(2))}),
                                     raw(xe::cf_call(6)),
                                     raw(xe::cf_ret()),
                                     exec({mul(1, r(1), c(3))}),
                                     raw(xe::cf_ret())};
    CHECK(run_ps(h, program, in) == 20.0f);
  }
}

TEST_CASE("cond_jmp out of a loop restores aL", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // Inner loop 1 (aL = 12) runs once and leaves through cond_jmp to the outer body, which reads r[aL] with the outer
  // aL (10, then 14) restored: (4 + 1) + (4 + 16). Xenia leaves the inner aL in place here; we pin Xenos semantics.
  const std::vector<Cf> program =
      concat(load_ladder(), {raw(xe::cf_loop_start(0, 9)), raw(xe::cf_loop_start(1, 7)), exec({add(1, r(1), rel(0))}),
                             raw(xe::cf_cond_jmp(7, 0, true)), raw(xe::cf_loop_end(1, 4)),
                             exec({add(1, r(1), rel(0))}), raw(xe::cf_loop_end(0, 3)), exec_end({mov(0, r(1), true)})});
  HarnessInputs in;
  in.ps_constants = ladder_constants();
  in.bools[0] = 1;
  in.loops[0] = loop_constant(2, 10, 4);
  in.loops[1] = loop_constant(5, 12, 1);
  CHECK(run_ps(h, program, in) == 25.0f);
}

TEST_CASE("runaway jump loop stops at the iteration cap", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // 1: r1 += 1, export it; 2: jmp 1, forever. The draw completes (no device lost) with the export as written by the
  // last pass: the clause runs once, then once per followed backward branch, kCfBackwardBranchCap times in all.
  const std::vector<Cf> program = {exec({mov(1, c(0))}), exec({add(1, r(1), c(1)), mov(0, r(1), true)}),
                                   raw(xe::cf_jmp(1)), raw(xe::cf_exec(true, 0, 0, 0))};
  HarnessInputs in;
  in.width = 1;
  in.ps_constants = ladder_constants();
  CHECK(sh::kCfBackwardBranchCap == (1u << 24));  // every count up to 2^24 is exact in a float
  CHECK(run_ps(h, program, in) == static_cast<float>(sh::kCfBackwardBranchCap));
}

TEST_CASE("cond_jmp onto its own loop_end continues the loop", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // Loop 0 (aL = 10, 11, 12): r1 += r[aL], then a jump to the loop's own loop_end skips r1 += 128. Staying inside the
  // body pops nothing, so the loop runs its three iterations either way.
  const std::vector<Cf> program =
      concat(load_ladder(), {raw(xe::cf_loop_start(0, 7)), exec({add(1, r(1), rel(0))}),
                             raw(xe::cf_cond_jmp(6, 0, true)), exec({add(1, r(1), c(8))}), raw(xe::cf_loop_end(0, 3)),
                             exec_end({mov(0, r(1), true)})});
  HarnessInputs in;
  in.ps_constants = ladder_constants();
  in.loops[0] = loop_constant(3, 10, 1);
  in.bools[0] = 1;
  CHECK(run_ps(h, program, in) == 1.0f + 2.0f + 4.0f);  // taken: a continue
  in.bools[0] = 0;
  CHECK(run_ps(h, program, in) == 1.0f + 2.0f + 4.0f + 3 * 128.0f);  // not taken
}

TEST_CASE("conditional cond_call", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // r1 = 1; call 3 (r1 *= 4) when bool 3 is set; then r1 += 8.
  const std::vector<Cf> program = {exec({mov(1, c(1))}), raw(xe::cf_cond_call(3, 3, true)),
                                   exec_end({add(1, r(1), c(4)), mov(0, r(1), true)}), exec({mul(1, r(1), c(3))}),
                                   raw(xe::cf_ret())};
  HarnessInputs in;
  in.ps_constants = ladder_constants();
  in.bools[0] = 1u << 3;
  CHECK(run_ps(h, program, in) == 12.0f);
  in.bools[0] = 0;
  CHECK(run_ps(h, program, in) == 9.0f);
}

TEST_CASE("cond_jmp on the predicate", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // p0 = (c9.w == 0); a jump on p0 == true skips r1 = 2.
  const std::vector<Cf> program = {exec({pred_set(xe::kScalarPredSetEq, c(9)), mov(1, c(1))}),
                                   raw(xe::cf_cond_jmp(3, 0, true, true)), exec({mov(1, c(2))}),
                                   exec_end({mov(0, r(1), true)})};
  HarnessInputs in;
  in.ps_constants = ladder_constants({{0, 0, 0, 0}});
  CHECK(run_ps(h, program, in) == 1.0f);  // p0 true: taken
  in.ps_constants = ladder_constants({{0, 0, 0, 1}});
  CHECK(run_ps(h, program, in) == 2.0f);  // p0 false: not taken
}

TEST_CASE("predicated break whose condition stays false runs the full count", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // p0 = (c9.w == 0) before loop 0 (aL = 10, 11, 12), whose loop_end breaks when p0 is true.
  const std::vector<Cf> program =
      concat(load_ladder(), {exec({pred_set(xe::kScalarPredSetEq, c(9))}), raw(xe::cf_loop_start(0, 6)),
                             exec({add(1, r(1), rel(0))}), raw(xe::cf_loop_end(0, 4, true, true)),
                             exec_end({mov(0, r(1), true)})});
  HarnessInputs in;
  in.loops[0] = loop_constant(3, 10, 1);
  in.ps_constants = ladder_constants({{0, 0, 0, 1}});
  CHECK(run_ps(h, program, in) == 1.0f + 2.0f + 4.0f);  // p0 false throughout: three iterations
  in.ps_constants = ladder_constants({{0, 0, 0, 0}});
  CHECK(run_ps(h, program, in) == 1.0f);  // p0 true: breaks after the first
}

TEST_CASE("top-level return and an unmatched loop_end fall through", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  HarnessInputs in;
  in.ps_constants = ladder_constants();
  // A return at call depth 0 is a no-op: r1 = 1, then r1 += 2.
  CHECK(run_ps(h, {exec({mov(1, c(1))}), raw(xe::cf_ret()), exec_end({add(1, r(1), c(2)), mov(0, r(1), true)})},
               in) == 3.0f);
  // A loop_end with no loop_start sees a count of 0 and leaves (no wrap to 0xFFFFFFFF): the body runs once.
  in.loops[2] = loop_constant(5, 0, 1);
  CHECK(run_ps(h, {exec({mov(1, c(1))}), exec({add(1, r(1), c(2))}), raw(xe::cf_loop_end(2, 1)),
                   exec_end({mov(0, r(1), true)})},
               in) == 3.0f);
}

// --- structure (spirv-dis) ----------------------------------------------------------------------------------------

TEST_CASE("straight-line program has no switch", "[unit]") {
  if (!lr::testing::spirv_tools_available()) SKIP("spirv-val/spirv-dis not installed (Arch: spirv-tools)");
  const std::string straight = disassemble(assemble({exec({mov(1, c(1))}), cond_exec(130, true, {mov(1, c(2))}),
                                                     cond_exec_pred(false, {mov(1, c(3))}),
                                                     exec_end({mov(0, r(1), true)})}));
  INFO(straight);
  CHECK(straight.find("OpSwitch") == std::string::npos);
  CHECK(straight.find("OpLoopMerge") == std::string::npos);
  CHECK(straight.find("OpSelectionMerge") != std::string::npos);

  const std::string jumps = disassemble(assemble({exec({mov(1, c(1))}), raw(xe::cf_jmp(3)), exec({mov(1, c(2))}),
                                                  exec_end({mov(0, r(1), true)})}));
  INFO(jumps);
  CHECK(jumps.find("OpSwitch") != std::string::npos);
  CHECK(jumps.find("OpLoopMerge") != std::string::npos);
}

TEST_CASE("malformed control flow still translates to valid SPIR-V", "[unit]") {
  // Guest-controlled control flow never aborts and never makes an invalid module: whatever the targets, nesting or
  // stack depths, the pc-switch form is well-formed and the runtime stacks stay in bounds.
  std::vector<std::pair<std::string, std::vector<Cf>>> programs;
  programs.push_back({"return at depth 0 (a no-op)", {exec({mov(1, c(1))}), raw(xe::cf_ret()), exec_end({mov(0, r(1), true)})}});
  programs.push_back({"loop_end without loop_start",
                      {exec({mov(1, c(1))}), raw(xe::cf_loop_end(2, 0)), exec_end({mov(0, r(1), true)})}});
  programs.push_back({"loop_start without loop_end, skipping past the end",
                      {raw(xe::cf_loop_start(2, 4)), exec({add(1, r(1), rel(0))}), exec_end({mov(0, r(1), true)})}});
  {
    std::vector<Cf> deep;
    for (uint32_t k = 0; k < 6; ++k) deep.push_back(raw(xe::cf_loop_start(k, 13 - k)));
    deep.push_back(exec({add(1, r(1), rel(0))}));
    for (uint32_t k = 0; k < 6; ++k) deep.push_back(raw(xe::cf_loop_end(5 - k, 6 - k)));
    deep.push_back(exec_end({mov(0, r(1), true)}));
    programs.push_back({"six nested loops", deep});
  }
  programs.push_back({"jmp to itself", {raw(xe::cf_jmp(0)), exec_end({mov(0, c(1), true)})}});
  programs.push_back({"recursive call", {raw(xe::cf_call(0)), exec_end({mov(0, c(1), true)})}});
  programs.push_back({"call cycle through a jmp",
                      {exec({mov(0, c(1), true)}), raw(xe::cf_jmp(2)), raw(xe::cf_call(0))}});
  programs.push_back({"predicated jumps, calls and break",
                      {exec({pred_set(xe::kScalarPredSetEq, c(1))}), raw(xe::cf_loop_start(0, 6)),
                       raw(xe::cf_cond_jmp(6, 0, true, true)), raw(xe::cf_cond_call(7, 0, false, true)),
                       exec({add(1, r(1), c(1))}), raw(xe::cf_loop_end(0, 2, true, false)),
                       exec_end({mov(0, r(1), true)}), raw(xe::cf_ret())}});
  for (const auto& [name, program] : programs) {
    INFO(name);
    translate_ok(ShaderStage::Pixel, assemble(program));
  }
  // The vertex stage builds the same structures.
  translate_ok(ShaderStage::Vertex,
               assemble({raw(xe::cf_loop_start(0, 3)), exec({add(1, r(1), rel(0))}), raw(xe::cf_loop_end(0, 1)),
                         exec_end({add(sh::kExportVsPosition, r(1), c(0), true)})}));
}
