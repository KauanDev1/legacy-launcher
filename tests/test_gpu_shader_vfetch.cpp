// SPDX-License-Identifier: MIT
// Vertex fetch inside the translated shaders (plan B, Task 15.8; translator_fetch.cpp): the per-draw vertex-fetch
// table read through buffer device addresses, the address computation (index rounding, stride, signed dword offset),
// bounds checks on both sides, the endian swaps, every vertex format with its conversion (signed, normalized,
// signed_rf_mode, exp_adjust), the destination swizzle, mini fetches, predicated fetches and a fetch after a kill.
// Shaders are synthetic microcode (support/xenos_shader_builder.h); every module goes through spirv-val, and every
// value is checked on lavapipe through support/vk_shader_harness.h. Expected values follow the Xenos semantics as
// Xenia's SPIR-V translator and xenos.h describe them (reference only, nothing copied).
//
// Most draws are point lists: vertex i lands on pixel i, fetches with index r0.x = i and exports the fetched
// registers as interpolators, which the pixel shader copies to its colour targets.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
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
using lr::testing::HarnessFetchSlot;
using lr::testing::HarnessInputs;
using lr::testing::HarnessResult;
using lr::testing::ShaderHarness;
using Pixel = std::array<float, 4>;

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

// Xenos vertex formats (xenos.h numbering).
constexpr uint32_t k_8_8_8_8 = 6;
constexpr uint32_t k_2_10_10_10 = 7;
constexpr uint32_t k_10_11_11 = 16;
constexpr uint32_t k_11_11_10 = 17;
constexpr uint32_t k_16_16 = 25;
constexpr uint32_t k_16_16_16_16 = 26;
constexpr uint32_t k_16_16_FLOAT = 31;
constexpr uint32_t k_16_16_16_16_FLOAT = 32;
constexpr uint32_t k_32 = 33;
constexpr uint32_t k_32_32 = 34;
constexpr uint32_t k_32_32_32_32 = 35;
constexpr uint32_t k_32_FLOAT = 36;
constexpr uint32_t k_32_32_FLOAT = 37;
constexpr uint32_t k_32_32_32_32_FLOAT = 38;
constexpr uint32_t k_32_32_32_FLOAT = 57;

// Endian swap modes (vertex fetch constant dword 1, bits 0..1).
constexpr uint32_t kEndianNone = 0;
constexpr uint32_t k8in16 = 1;
constexpr uint32_t k8in32 = 2;
constexpr uint32_t k16in32 = 3;

constexpr uint32_t kXXXX = 0x6C;  // ALU source swizzle .xxxx

// --- microcode ----------------------------------------------------------------------------------------------------

xe::AluSrc r(uint32_t reg, uint32_t swizzle = 0) { return {reg, true, swizzle}; }
xe::AluSrc c(uint32_t reg, uint32_t swizzle = 0) { return {reg, false, swizzle}; }

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
xe::Alu add(uint32_t dest, xe::AluSrc a, xe::AluSrc b, bool to_export = false) {
  return vop(xe::kVectorAdd, dest, a, b, to_export);
}
xe::Alu mul(uint32_t dest, xe::AluSrc a, xe::AluSrc b, bool to_export = false) {
  return vop(xe::kVectorMul, dest, a, b, to_export);
}
// A scalar predicate set on s.w (no register written).
xe::Alu pred_set(uint32_t op, xe::AluSrc s) {
  xe::Alu alu;
  alu.vector_mask = 0;
  alu.scalar_op = op;
  alu.scalar_mask = 0;
  alu.src3 = s;
  return alu;
}

// A full vertex fetch: r<dst> from `slot`, index r<src>.x, stride and offset in dwords.
xe::VFetch vf(uint32_t dst, uint32_t slot, uint32_t format, uint32_t stride, int32_t offset = 0, uint32_t src = 0) {
  xe::VFetch f;
  f.dst = dst;
  f.src = src;
  f.slot = slot;
  f.format = format;
  f.stride_dwords = stride;
  f.offset_dwords = offset;
  return f;
}
xe::VFetch mini(uint32_t dst, uint32_t format, int32_t offset) {
  xe::VFetch f;
  f.dst = dst;
  f.format = format;
  f.offset_dwords = offset;
  f.mini = true;
  return f;
}

using Instr = std::variant<xe::Alu, xe::VFetch>;
struct Cf {
  bool end = false;
  std::vector<Instr> instrs;
};
constexpr size_t kClauseMax = 6;
Cf exec(std::vector<Instr> instrs, bool end = false) { return {end, std::move(instrs)}; }
Cf exec_end(std::vector<Instr> instrs) { return exec(std::move(instrs), true); }

// Exec clauses (split at kClauseMax), the sequence bits marking the fetches.
std::vector<uint32_t> assemble(const std::vector<Cf>& program) {
  struct Clause {
    bool end;
    size_t first, count;
  };
  std::vector<Clause> clauses;
  std::vector<Instr> instrs;
  for (const Cf& cf : program) {
    for (size_t at = 0; at < cf.instrs.size(); at += kClauseMax) {
      const size_t n = std::min(kClauseMax, cf.instrs.size() - at);
      clauses.push_back({cf.end && at + n == cf.instrs.size(), instrs.size(), n});
      instrs.insert(instrs.end(), cf.instrs.begin() + static_cast<std::ptrdiff_t>(at),
                    cf.instrs.begin() + static_cast<std::ptrdiff_t>(at + n));
    }
  }
  const auto cf_slots = static_cast<uint32_t>((clauses.size() + 1) / 2);
  std::vector<uint64_t> words;
  for (const Clause& k : clauses) {
    uint32_t sequence = 0;
    for (size_t i = 0; i < k.count; ++i)
      if (std::holds_alternative<xe::VFetch>(instrs[k.first + i])) sequence |= 1u << (2 * i);
    words.push_back(
        xe::cf_exec(k.end, cf_slots + static_cast<uint32_t>(k.first), static_cast<uint32_t>(k.count), sequence));
  }
  if (words.size() % 2) words.push_back(xe::cf_nop());
  std::vector<uint32_t> code;
  for (size_t i = 0; i < words.size(); i += 2) xe::emit(code, xe::cf_pair(words[i], words[i + 1]));
  for (const Instr& in : instrs) {
    if (const auto* a = std::get_if<xe::Alu>(&in))
      xe::emit(code, xe::alu(*a));
    else
      xe::emit(code, xe::vfetch(std::get<xe::VFetch>(in)));
  }
  return code;
}

// --- the point-list vertex shader ---------------------------------------------------------------------------------

// Position of vertex i = c0 + c1 * r0.x: the centre of pixel i (constants from point_constants()).
std::vector<Instr> position() {
  return {mul(1, r(0, kXXXX), c(1)), add(sh::kExportVsPosition, r(1), c(0), true)};
}
// c0, c1 for position() on a `width` x 1 target. Tests put their own constants from c2.
std::vector<Pixel> point_constants(uint32_t width) {
  const float s = 2.0f / static_cast<float>(width);
  return {Pixel{s / 2 - 1, 0, 0, 1}, Pixel{s, 0, 0, 0}};
}
// Interpolator k = r<first + k>, for k < n.
std::vector<Instr> exports(uint32_t n, uint32_t first = 3) {
  std::vector<Instr> out;
  for (uint32_t k = 0; k < n; ++k) out.push_back(mov(k, r(first + k), true));
  return out;
}
// The usual VS: position (and `pre`), then the fetches, then interpolators 0..n-1 = r3..r(3+n-1).
std::vector<Cf> fetch_vs(std::vector<Instr> fetches, uint32_t n = 1, std::vector<Instr> pre = {}) {
  std::vector<Instr> first = position();
  first.insert(first.end(), pre.begin(), pre.end());
  return {exec(first), exec(std::move(fetches)), exec_end(exports(n))};
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

std::string text(const Pixel& p) {
  std::ostringstream s;
  s.precision(9);
  s << "(" << p[0] << ", " << p[1] << ", " << p[2] << ", " << p[3] << ")";
  return s.str();
}

// Exact (same value and sign, NaN matches NaN) or, with a tolerance, relative to the expected magnitude.
bool matches(float got, float want, float tolerance) {
  if (std::isnan(want)) return std::isnan(got);
  if (tolerance == 0.0f) return got == want && std::signbit(got) == std::signbit(want);
  return std::fabs(got - want) <= tolerance * std::fmax(1.0f, std::fabs(want));
}

void check_pixel(const Pixel& got, const Pixel& want, float tolerance = 0.0f) {
  INFO("got " << text(got) << ", want " << text(want));
  for (uint32_t k = 0; k < 4; ++k) CHECK(matches(got[k], want[k], tolerance));
}

// Draws in.width points with the VS `vs` and a PS that copies interpolators 0..targets-1 to oC0..; returns
// color[t][pixel]. in.vs_constants c0, c1 are set here (the caller's own constants start at c2).
std::array<std::vector<Pixel>, 4> draw_points(ShaderHarness& h, const std::vector<Cf>& vs, HarnessInputs in,
                                              uint32_t targets = 1) {
  const auto vs_t = translate_ok(ShaderStage::Vertex, assemble(vs));
  std::vector<Instr> ps;
  for (uint32_t t = 0; t < targets; ++t) ps.push_back(mov(t, r(t), true));
  const auto ps_t = translate_ok(ShaderStage::Pixel, assemble({exec_end(ps)}));
  in.vertex_count = in.width;
  in.topology = lr::testing::HarnessTopology::PointList;
  in.color_targets = targets;
  const auto pc = point_constants(in.width);
  if (in.vs_constants.size() < 2) in.vs_constants.resize(2);
  in.vs_constants[0] = pc[0];
  in.vs_constants[1] = pc[1];
  auto res = h.draw(vs_t.spirv, ps_t.spirv, in);
  if (!res) FAIL("draw: " << res.error().message);
  std::array<std::vector<Pixel>, 4> out;
  for (uint32_t t = 0; t < targets; ++t) {
    REQUIRE(res.value().color[t].size() == in.width);
    out[t] = res.value().color[t];
  }
  return out;
}

// --- vertex data --------------------------------------------------------------------------------------------------

uint32_t f2u(float f) { return std::bit_cast<uint32_t>(f); }

uint32_t swap(uint32_t w, uint32_t endian) {
  if (endian == k8in16 || endian == k8in32) w = ((w & 0x00FF00FFu) << 8) | ((w >> 8) & 0x00FF00FFu);
  if (endian == k8in32 || endian == k16in32) w = (w << 16) | (w >> 16);
  return w;
}

// The guest bytes of `words` for swap mode `endian`: the shader's load (little-endian) and swap give back the words.
std::vector<uint8_t> guest_bytes(const std::vector<uint32_t>& words, uint32_t endian = kEndianNone) {
  std::vector<uint8_t> out;
  for (uint32_t w : words) {
    const uint32_t s = swap(w, endian);
    for (int b = 0; b < 4; ++b) out.push_back(static_cast<uint8_t>(s >> (8 * b)));
  }
  return out;
}

HarnessFetchSlot fetch_slot(uint32_t slot, const std::vector<uint32_t>& words, uint32_t endian = kEndianNone) {
  HarnessFetchSlot s;
  s.slot = slot;
  s.bytes = guest_bytes(words, endian);
  s.endian = endian;
  return s;
}

// Packs field values (two's complement, masked) at bit offsets.
uint32_t pack(std::initializer_list<std::pair<int64_t, uint32_t>> fields_and_widths) {
  uint32_t w = 0, at = 0;
  for (const auto& [v, width] : fields_and_widths) {
    const uint32_t mask = width == 32 ? 0xFFFFFFFFu : (1u << width) - 1;
    w |= (static_cast<uint32_t>(v) & mask) << at;
    at += width;
  }
  return w;
}

// One element per row: vertex i fetches row i (index r0.x, stride = the row's word count) with `f`'s format and
// conversion fields; pixel i must match the row's expected value.
struct Row {
  std::vector<uint32_t> words;
  Pixel expected;
};
void check_rows(ShaderHarness& h, xe::VFetch f, const std::vector<Row>& rows, float tolerance = 0.0f) {
  REQUIRE_FALSE(rows.empty());
  const auto stride = static_cast<uint32_t>(rows[0].words.size());
  std::vector<uint32_t> data;
  for (const Row& row : rows) {
    REQUIRE(row.words.size() == stride);
    data.insert(data.end(), row.words.begin(), row.words.end());
  }
  f.dst = 3;
  f.src = 0;
  f.slot = 0;
  f.stride_dwords = stride;
  HarnessInputs in;
  in.width = static_cast<uint32_t>(rows.size());
  in.fetch_slots = {fetch_slot(0, data)};
  const auto out = draw_points(h, fetch_vs({f}), in);
  for (size_t i = 0; i < rows.size(); ++i) {
    INFO("row " << i);
    check_pixel(out[0][i], rows[i].expected, tolerance);
  }
}

xe::VFetch conv(uint32_t format, bool is_signed, bool normalized, bool signed_rf_mode = false, int32_t exp_adjust = 0) {
  xe::VFetch f;
  f.format = format;
  f.is_signed = is_signed;
  f.normalized = normalized;
  f.signed_rf_mode = signed_rf_mode;
  f.exp_adjust = exp_adjust;
  return f;
}

constexpr float kTol = 1e-6f;

// Four float4 vertices with distinct components.
std::vector<uint32_t> float4_data(uint32_t count) {
  std::vector<uint32_t> w;
  for (uint32_t i = 0; i < count; ++i) {
    const float fi = static_cast<float>(i);
    for (float v : {fi + 0.25f, -(fi + 1) * 1.5f, 2 * fi, 100 + fi}) w.push_back(f2u(v));
  }
  return w;
}
Pixel float4_value(int32_t i) {
  const float fi = static_cast<float>(i);
  return {fi + 0.25f, -(fi + 1) * 1.5f, 2 * fi, 100 + fi};
}

}  // namespace

// --- address and endianness ---------------------------------------------------------------------------------------

TEST_CASE("float4 vertex fetch with 8in32 swap", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // Slot 7, stride 4: vertex i reads element i. The guest bytes are big-endian (8in32); the other modes are checked
  // with the same values stored for them.
  for (const uint32_t endian : {k8in32, kEndianNone, k8in16, k16in32}) {
    INFO("endian " << endian);
    HarnessInputs in;
    in.fetch_slots = {fetch_slot(7, float4_data(4), endian)};
    const auto out = draw_points(h, fetch_vs({vf(3, 7, k_32_32_32_32_FLOAT, 4)}), in);
    for (int32_t i = 0; i < 4; ++i) {
      INFO("vertex " << i);
      check_pixel(out[0][static_cast<size_t>(i)], float4_value(i));
    }
  }
  {
    // 8in32 on the raw big-endian bytes, written out by hand: 1.0f = 3F 80 00 00.
    INFO("big-endian bytes");
    HarnessInputs in;
    in.width = 1;
    HarnessFetchSlot s;
    s.slot = 7;
    s.endian = k8in32;
    s.bytes = {0x3F, 0x80, 0, 0, 0x40, 0, 0, 0, 0xC0, 0x40, 0, 0, 0x41, 0x20, 0, 0};  // 1, 2, -3, 10
    in.fetch_slots = {s};
    const auto out = draw_points(h, fetch_vs({vf(3, 7, k_32_32_32_32_FLOAT, 4)}), in);
    check_pixel(out[0][0], {1, 2, -3, 10});
  }
}

TEST_CASE("index register is rounded", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // Index r5.x = r0.x + c2.x; rounded: floor(x + 0.5), otherwise floor(x). Element k = float4_value(k) for k = 0..4;
  // index -1 is before the slot start and reads 0.
  struct Case {
    float add;
    bool rounded;
    int32_t shift;  // vertex i reads element i + shift
  };
  for (const Case& k : {Case{0.6f, true, 1}, Case{0.6f, false, 0}, Case{0.5f, true, 1}, Case{0.49f, true, 0},
                        Case{-0.4f, true, 0}, Case{-0.4f, false, -1}}) {
    INFO("index + " << k.add << (k.rounded ? " rounded" : " floored"));
    HarnessInputs in;
    in.vs_constants = {{}, {}, {k.add, 0, 0, 0}};
    in.fetch_slots = {fetch_slot(0, float4_data(5))};
    xe::VFetch f = vf(3, 0, k_32_32_32_32_FLOAT, 4, 0, 5);
    f.rounded = k.rounded;
    const auto out = draw_points(h, fetch_vs({f}, 1, {add(5, r(0), c(2))}), in);
    for (int32_t i = 0; i < 4; ++i) {
      INFO("vertex " << i);
      const int32_t e = i + k.shift;
      check_pixel(out[0][static_cast<size_t>(i)], e < 0 ? Pixel{0, 0, 0, 0} : float4_value(e));
    }
  }
}

TEST_CASE("fetch past the slot size reads zero", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // The buffer always holds 8 float4 elements; the slot's size_bytes decides what the shader may read. Every element
  // that is not wholly inside the slot reads (0, 0, 0, 0), even though the bytes exist (Review Focus 7).
  SECTION("index * stride past size_bytes") {
    for (const uint32_t size : {32u, 40u, 44u}) {  // 2, 2.5 and 2.75 elements
      INFO("size_bytes " << size);
      HarnessInputs in;
      in.fetch_slots = {fetch_slot(0, float4_data(8))};
      in.fetch_slots[0].size_bytes = size;
      const auto out = draw_points(h, fetch_vs({vf(3, 0, k_32_32_32_32_FLOAT, 4)}), in);
      for (int32_t i = 0; i < 4; ++i) {
        INFO("vertex " << i);
        check_pixel(out[0][static_cast<size_t>(i)], i < 2 ? float4_value(i) : Pixel{0, 0, 0, 0});
      }
    }
  }
  SECTION("a huge, infinite or NaN index") {
    // Index r5.x = r0.x * c2.x: vertex 0 gets 0 (the Xenos product 0 * x is 0), the others a value no slot reaches.
    for (const float scale : {1e9f, -1e9f, 3e38f, kInf, -kInf, kNan}) {
      INFO("index scale " << scale);
      HarnessInputs in;
      in.vs_constants = {{}, {}, {scale, 0, 0, 0}};
      in.fetch_slots = {fetch_slot(0, float4_data(8))};
      for (const uint32_t stride : {4u, 255u}) {
        INFO("stride " << stride);
        const auto out = draw_points(h, fetch_vs({vf(3, 0, k_32_32_32_32_FLOAT, stride, 0, 5)}, 1,
                                                 {mul(5, r(0), c(2))}), in);
        check_pixel(out[0][0], float4_value(0));
        for (size_t i = 1; i < 4; ++i) check_pixel(out[0][i], {0, 0, 0, 0});
      }
    }
  }
  SECTION("the largest positive offset") {
    HarnessInputs in;
    in.fetch_slots = {fetch_slot(0, float4_data(8))};
    const auto out = draw_points(h, fetch_vs({vf(3, 0, k_32_32_32_32_FLOAT, 4, 4194303)}), in);
    for (size_t i = 0; i < 4; ++i) check_pixel(out[0][i], {0, 0, 0, 0});
  }
  SECTION("an unbound slot (address 0, size 0)") {
    HarnessInputs in;
    in.fetch_slots = {fetch_slot(0, float4_data(8))};
    const auto out = draw_points(h, fetch_vs({vf(3, 1, k_32_32_32_32_FLOAT, 4)}), in);
    for (size_t i = 0; i < 4; ++i) check_pixel(out[0][i], {0, 0, 0, 0});
  }
  SECTION("every component is 0, the missing w included") {
    // k_32_FLOAT has one component: in range (x, 0, 0, 1), past the size (0, 0, 0, 0).
    HarnessInputs in;
    in.fetch_slots = {fetch_slot(0, {f2u(5), f2u(6), f2u(7), f2u(8)})};
    in.fetch_slots[0].size_bytes = 8;
    const auto out = draw_points(h, fetch_vs({vf(3, 0, k_32_FLOAT, 1)}), in);
    check_pixel(out[0][0], {5, 0, 0, 1});
    check_pixel(out[0][1], {6, 0, 0, 1});
    check_pixel(out[0][2], {0, 0, 0, 0});
    check_pixel(out[0][3], {0, 0, 0, 0});
  }
}

TEST_CASE("negative offset before the slot start reads zero", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // 32 bytes of guard data (555.0) lie right before the slot start in the same buffer: a fetch must never see them.
  auto slot_with_guard = [] {
    std::vector<uint32_t> words(8, f2u(555.0f));
    const auto data = float4_data(4);
    words.insert(words.end(), data.begin(), data.end());
    HarnessFetchSlot s = fetch_slot(0, words);
    s.lead = 32;
    return s;
  };
  SECTION("offset -4: vertex i reads element i - 1") {
    HarnessInputs in;
    in.fetch_slots = {slot_with_guard()};
    const auto out = draw_points(h, fetch_vs({vf(3, 0, k_32_32_32_32_FLOAT, 4, -4)}), in);
    check_pixel(out[0][0], {0, 0, 0, 0});
    for (int32_t i = 1; i < 4; ++i) check_pixel(out[0][static_cast<size_t>(i)], float4_value(i - 1));
  }
  SECTION("offset -1 is in dwords: the element straddles two vertices") {
    HarnessInputs in;
    in.fetch_slots = {slot_with_guard()};
    const auto out = draw_points(h, fetch_vs({vf(3, 0, k_32_32_32_32_FLOAT, 4, -1)}), in);
    check_pixel(out[0][0], {0, 0, 0, 0});
    for (int32_t i = 1; i < 4; ++i) {
      const Pixel a = float4_value(i - 1), b = float4_value(i);
      check_pixel(out[0][static_cast<size_t>(i)], {a[3], b[0], b[1], b[2]});
    }
  }
  SECTION("the most negative offset") {
    HarnessInputs in;
    in.fetch_slots = {slot_with_guard()};
    const auto out = draw_points(h, fetch_vs({vf(3, 0, k_32_32_32_32_FLOAT, 4, -4194304)}), in);
    for (size_t i = 0; i < 4; ++i) check_pixel(out[0][i], {0, 0, 0, 0});
  }
}

// --- formats ------------------------------------------------------------------------------------------------------

TEST_CASE("k_16_16_FLOAT", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // x in bits 0..15, y in bits 16..31 (after the swap); missing components (0, 0, 0, 1). is_signed and normalized do
  // not change a float format.
  const std::vector<Row> rows = {
      {{0xC0003E00u}, {1.5f, -2.0f, 0, 1}},
      {{0x7BFF3C00u}, {1.0f, 65504.0f, 0, 1}},
      {{0x00013800u}, {0.5f, 5.9604645e-8f, 0, 1}},  // y: the smallest half denormal
      {{0x00000000u}, {0, 0, 0, 1}},
  };
  check_rows(h, conv(k_16_16_FLOAT, false, true), rows);
  check_rows(h, conv(k_16_16_FLOAT, true, false), rows);
  SECTION("k_16_16_16_16_FLOAT") {
    check_rows(h, conv(k_16_16_16_16_FLOAT, false, true),
               {{{0xC0003E00u, 0x3C00B800u}, {1.5f, -2.0f, -0.5f, 1.0f}},
                {{0x00000000u, 0x4900C400u}, {0, 0, -4.0f, 10.0f}}});
  }
}

TEST_CASE("k_8_8_8_8 normalized unsigned and signed", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // x bits 0..7, y 8..15, z 16..23, w 24..31.
  SECTION("unsigned: v / 255") {
    check_rows(h, conv(k_8_8_8_8, false, true),
               {{{pack({{255, 8}, {0, 8}, {128, 8}, {1, 8}})}, {1, 0, 128 / 255.0f, 1 / 255.0f}},
                {{pack({{51, 8}, {102, 8}, {204, 8}, {255, 8}})}, {0.2f, 0.4f, 0.8f, 1}}},
               kTol);
  }
  SECTION("signed, signed_rf_mode 0: v / 127, clamped to -1") {
    check_rows(h, conv(k_8_8_8_8, true, true, false),
               {{{pack({{127, 8}, {-127, 8}, {-128, 8}, {1, 8}})}, {1, -1, -1, 1 / 127.0f}},
                {{pack({{0, 8}, {64, 8}, {-64, 8}, {-1, 8}})}, {0, 64 / 127.0f, -64 / 127.0f, -1 / 127.0f}}},
               kTol);
  }
  SECTION("signed, signed_rf_mode 1 (no zero): (v + 0.5) / 127.5") {
    check_rows(h, conv(k_8_8_8_8, true, true, true),
               {{{pack({{127, 8}, {-128, 8}, {0, 8}, {-1, 8}})}, {1, -1, 0.5f / 127.5f, -0.5f / 127.5f}}}, kTol);
  }
}

TEST_CASE("k_2_10_10_10 signed", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // x bits 0..9, y 10..19, z 20..29, w 30..31 (two bits: -2..1, scale 1).
  SECTION("normalized, clamped to -1") {
    check_rows(h, conv(k_2_10_10_10, true, true),
               {{{pack({{511, 10}, {-511, 10}, {-512, 10}, {1, 2}})}, {1, -1, -1, 1}},
                {{pack({{0, 10}, {256, 10}, {-256, 10}, {-2, 2}})}, {0, 256 / 511.0f, -256 / 511.0f, -1}},
                {{pack({{1, 10}, {-1, 10}, {100, 10}, {-1, 2}})}, {1 / 511.0f, -1 / 511.0f, 100 / 511.0f, -1}}},
               kTol);
  }
  SECTION("integer") {
    check_rows(h, conv(k_2_10_10_10, true, false),
               {{{pack({{511, 10}, {-512, 10}, {-3, 10}, {-2, 2}})}, {511, -512, -3, -2}}});
  }
  SECTION("unsigned normalized") {
    check_rows(h, conv(k_2_10_10_10, false, true),
               {{{pack({{1023, 10}, {0, 10}, {341, 10}, {3, 2}})}, {1, 0, 341 / 1023.0f, 1}},
                {{pack({{1, 10}, {2, 10}, {3, 10}, {1, 2}})}, {1 / 1023.0f, 2 / 1023.0f, 3 / 1023.0f, 1 / 3.0f}}},
               kTol);
  }
}

TEST_CASE("k_10_11_11 and k_11_11_10", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // k_10_11_11: x 11 bits at 0, y 11 at 11, z 10 at 22. k_11_11_10: x 10 bits at 0, y 11 at 10, z 11 at 21.
  // Three components: w is 1.
  SECTION("k_10_11_11 unsigned normalized") {
    check_rows(h, conv(k_10_11_11, false, true),
               {{{pack({{100, 11}, {2000, 11}, {1000, 10}})}, {100 / 2047.0f, 2000 / 2047.0f, 1000 / 1023.0f, 1}},
                {{pack({{2047, 11}, {0, 11}, {1023, 10}})}, {1, 0, 1, 1}}},
               kTol);
  }
  SECTION("k_11_11_10 unsigned normalized") {
    check_rows(h, conv(k_11_11_10, false, true),
               {{{pack({{1000, 10}, {100, 11}, {2000, 11}})}, {1000 / 1023.0f, 100 / 2047.0f, 2000 / 2047.0f, 1}},
                {{pack({{1023, 10}, {2047, 11}, {0, 11}})}, {1, 1, 0, 1}}},
               kTol);
  }
  SECTION("signed normalized and integer") {
    check_rows(h, conv(k_10_11_11, true, true),
               {{{pack({{-1023, 11}, {512, 11}, {-512, 10}})}, {-1, 512 / 1023.0f, -1, 1}}}, kTol);
    check_rows(h, conv(k_11_11_10, true, false),
               {{{pack({{-512, 10}, {1023, 11}, {-1024, 11}})}, {-512, 1023, -1024, 1}}});
  }
}

TEST_CASE("integer non-normalized fetch", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  SECTION("k_8_8_8_8") {
    check_rows(h, conv(k_8_8_8_8, false, false), {{{pack({{255, 8}, {1, 8}, {2, 8}, {128, 8}})}, {255, 1, 2, 128}}});
    check_rows(h, conv(k_8_8_8_8, true, false), {{{pack({{255, 8}, {1, 8}, {127, 8}, {128, 8}})}, {-1, 1, 127, -128}}});
  }
  SECTION("k_16_16 and k_16_16_16_16") {
    check_rows(h, conv(k_16_16, true, false), {{{pack({{-32768, 16}, {32767, 16}})}, {-32768, 32767, 0, 1}}});
    check_rows(h, conv(k_16_16, false, false), {{{pack({{65535, 16}, {7, 16}})}, {65535, 7, 0, 1}}});
    check_rows(h, conv(k_16_16_16_16, true, false),
               {{{pack({{-1, 16}, {2, 16}}), pack({{-3, 16}, {4, 16}})}, {-1, 2, -3, 4}}});
  }
  SECTION("k_32, k_32_32, k_32_32_32_32") {
    check_rows(h, conv(k_32, false, false), {{{0xFFFFFFFFu}, {4294967296.0f, 0, 0, 1}}, {{7u}, {7, 0, 0, 1}}});
    check_rows(h, conv(k_32, true, false), {{{0xFFFFFFFFu}, {-1, 0, 0, 1}}, {{0x80000000u}, {-2147483648.0f, 0, 0, 1}}});
    check_rows(h, conv(k_32_32, true, false), {{{5u, 0xFFFFFFFBu}, {5, -5, 0, 1}}});
    check_rows(h, conv(k_32_32_32_32, false, false), {{{1u, 2u, 3u, 0x7FFFFFFFu}, {1, 2, 3, 2147483648.0f}}});
  }
}

TEST_CASE("16- and 32-bit normalized and float formats", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  SECTION("k_16_16 and k_16_16_16_16 normalized") {
    check_rows(h, conv(k_16_16, false, true), {{{pack({{65535, 16}, {32768, 16}})}, {1, 32768 / 65535.0f, 0, 1}}},
               kTol);
    check_rows(h, conv(k_16_16_16_16, true, true),
               {{{pack({{32767, 16}, {-32768, 16}}), pack({{-32767, 16}, {16384, 16}})},
                 {1, -1, -1, 16384 / 32767.0f}}},
               kTol);
    check_rows(h, conv(k_16_16, true, true, true),
               {{{pack({{32767, 16}, {-32768, 16}})}, {1, -1, 0, 1}}, {{pack({{0, 16}, {0, 16}})},
                                                                       {0.5f / 32767.5f, 0.5f / 32767.5f, 0, 1}}},
               kTol);
  }
  SECTION("k_32 normalized") {
    check_rows(h, conv(k_32, false, true), {{{0xFFFFFFFFu}, {1, 0, 0, 1}}, {{0x80000000u}, {0.5f, 0, 0, 1}}}, kTol);
    check_rows(h, conv(k_32_32, true, true), {{{0x7FFFFFFFu, 0x80000000u}, {1, -1, 0, 1}}}, kTol);
    check_rows(h, conv(k_32_32_32_32, true, true, true),
               {{{0x7FFFFFFFu, 0x80000000u, 0u, 0xFFFFFFFFu}, {1, -1, 0.5f / 2147483647.5f, -0.5f / 2147483647.5f}}},
               kTol);
  }
  SECTION("32-bit float formats") {
    check_rows(h, conv(k_32_FLOAT, true, false), {{{f2u(-2.5f)}, {-2.5f, 0, 0, 1}}});
    check_rows(h, conv(k_32_32_FLOAT, false, true), {{{f2u(1e30f), f2u(-1e-30f)}, {1e30f, -1e-30f, 0, 1}}});
    check_rows(h, conv(k_32_32_32_FLOAT, false, true), {{{f2u(1), f2u(2), f2u(3)}, {1, 2, 3, 1}}});
    check_rows(h, conv(k_32_32_32_32_FLOAT, false, true), {{{f2u(4), f2u(3), f2u(2), f2u(1)}, {4, 3, 2, 1}}});
  }
}

TEST_CASE("exp_adjust scales the result", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // The converted components are multiplied by 2^exp_adjust (signed 6-bit); the missing w stays 1.
  check_rows(h, conv(k_8_8_8_8, false, false, false, -1), {{{pack({{255, 8}, {2, 8}, {0, 8}, {1, 8}})}, {127.5f, 1, 0, 0.5f}}});
  check_rows(h, conv(k_8_8_8_8, false, true, false, 4), {{{pack({{255, 8}, {0, 8}, {0, 8}, {0, 8}})}, {16, 0, 0, 0}}},
             kTol);
  check_rows(h, conv(k_32_FLOAT, false, true, false, 2), {{{f2u(3)}, {12, 0, 0, 1}}});
  check_rows(h, conv(k_32_32_32_32_FLOAT, false, true, false, -32), {{{f2u(4294967296.0f), f2u(1), 0, 0}, {1, 0x1p-32f, 0, 0}}});
  check_rows(h, conv(k_16_16, true, false, false, 31), {{{pack({{1, 16}, {-1, 16}})}, {0x1p31f, -0x1p31f, 0, 1}}});
}

// --- swizzle, mini, predicate, kill -------------------------------------------------------------------------------

TEST_CASE("destination swizzle selects, zeroes, sets one and keeps", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // r3 = c2 = (9, 9, 9, 9), then vfetch r3 with x <- w, y <- 0, z <- 1, w <- keep.
  HarnessInputs in;
  in.vs_constants = {{}, {}, {9, 9, 9, 9}};
  in.fetch_slots = {fetch_slot(0, float4_data(4))};
  xe::VFetch f = vf(3, 0, k_32_32_32_32_FLOAT, 4);
  f.dst_swizzle = 3 | (sh::kFetchSwizzleZero << 3) | (sh::kFetchSwizzleOne << 6) | (sh::kFetchSwizzleKeep << 9);
  const auto out = draw_points(h, fetch_vs({f}, 1, {mov(3, c(2))}), in);
  for (int32_t i = 0; i < 4; ++i) check_pixel(out[0][static_cast<size_t>(i)], {float4_value(i)[3], 0, 1, 9});
  {
    INFO("a missing component through the swizzle: k_32_32_FLOAT .wzyx = (1, 0, y, x)");
    HarnessInputs in2;
    in2.width = 1;
    in2.fetch_slots = {fetch_slot(0, {f2u(5), f2u(6)})};
    xe::VFetch g = vf(3, 0, k_32_32_FLOAT, 2);
    g.dst_swizzle = 3 | (2 << 3) | (1 << 6) | (0 << 9);
    check_pixel(draw_points(h, fetch_vs({g}), in2)[0][0], {1, 0, 6, 5});
  }
}

TEST_CASE("mini fetch reuses the index, slot and stride with its own offset and format", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // Slot 5, stride 6: element i = float4 (words 0..3), a 16_16_FLOAT pair (word 4: i + 1, 0.5) and an 8_8_8_8 word
  // (word 5: i, 2, 3, 4). The full fetch reads words 0..3; the first mini fetch word 4, the second word 5. Between
  // the full fetch and the mini fetches r0 is overwritten with 1000: a mini fetch reuses the index the full fetch
  // computed, it does not read the register again.
  const uint32_t halves[] = {0x3C00, 0x4000, 0x4200, 0x4400};  // 1, 2, 3, 4
  std::vector<uint32_t> data;
  for (uint32_t i = 0; i < 4; ++i) {
    const auto e = float4_data(i + 1);
    data.insert(data.end(), e.end() - 4, e.end());
    data.push_back(halves[i] | (0x3800u << 16));
    data.push_back(pack({{i, 8}, {2, 8}, {3, 8}, {4, 8}}));
  }
  HarnessInputs in;
  in.vs_constants = {{}, {}, {1000, 1000, 1000, 1000}};
  in.fetch_slots = {fetch_slot(5, data)};
  xe::VFetch bytes = mini(5, k_8_8_8_8, 5);
  bytes.normalized = false;
  const std::vector<Cf> vs = {exec(position()), exec({vf(3, 5, k_32_32_32_32_FLOAT, 6)}), exec({mov(0, c(2))}),
                              exec({mini(4, k_16_16_FLOAT, 4), bytes}), exec_end(exports(3))};
  const auto out = draw_points(h, vs, in, 3);
  for (uint32_t i = 0; i < 4; ++i) {
    INFO("vertex " << i);
    check_pixel(out[0][i], float4_value(static_cast<int32_t>(i)));
    check_pixel(out[1][i], {static_cast<float>(i + 1), 0.5f, 0, 1});
    check_pixel(out[2][i], {static_cast<float>(i), 2, 3, 4});
  }
}

TEST_CASE("predicated fetch keeps the old value when the predicate is false", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // r3 = r4 = c2 = 7; p0 = (c3.w == 0); vfetch r3 (pred true) and vfetch r4 (pred false) from slot 0.
  xe::VFetch when_true = vf(3, 0, k_32_32_32_32_FLOAT, 4);
  when_true.predicated = true;
  when_true.pred_condition = true;
  xe::VFetch when_false = vf(4, 0, k_32_32_32_32_FLOAT, 4);
  when_false.predicated = true;
  when_false.pred_condition = false;
  const auto vs =
      fetch_vs({when_true, when_false}, 2, {mov(3, c(2)), mov(4, c(2)), pred_set(xe::kScalarPredSetEq, c(3))});
  for (const bool p0 : {true, false}) {
    INFO("p0 " << p0);
    HarnessInputs in;
    in.vs_constants = {{}, {}, {7, 7, 7, 7}, {0, 0, 0, p0 ? 0.0f : 1.0f}};
    in.fetch_slots = {fetch_slot(0, float4_data(4))};
    const auto out = draw_points(h, vs, in, 2);
    for (int32_t i = 0; i < 4; ++i) {
      INFO("vertex " << i);
      check_pixel(out[0][static_cast<size_t>(i)], p0 ? float4_value(i) : Pixel{7, 7, 7, 7});
      check_pixel(out[1][static_cast<size_t>(i)], p0 ? Pixel{7, 7, 7, 7} : float4_value(i));
    }
  }
  // Evaluated unconditionally and merged with OpSelect: the predicated shader has the same selection constructs as
  // the unpredicated one (only the bounds checks) and two more OpSelects per predicated fetch (the register merge and
  // the mini-fetch base).
  if (lr::testing::spirv_tools_available()) {
    const auto unpredicated = fetch_vs({vf(3, 0, k_32_32_32_32_FLOAT, 4), vf(4, 0, k_32_32_32_32_FLOAT, 4)}, 2,
                                       {mov(3, c(2)), mov(4, c(2)), pred_set(xe::kScalarPredSetEq, c(3))});
    const std::string with = lr::testing::spirv_dis(translate_ok(ShaderStage::Vertex, assemble(vs)).spirv);
    const std::string without = lr::testing::spirv_dis(translate_ok(ShaderStage::Vertex, assemble(unpredicated)).spirv);
    const auto count = [](const std::string& s, const std::string& what) {
      size_t n = 0;
      for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) ++n;
      return n;
    };
    INFO(with);
    CHECK(count(with, "OpSelectionMerge") == count(without, "OpSelectionMerge"));
    CHECK(count(with, "= OpSelect ") == count(without, "= OpSelect ") + 4);
  }
}

TEST_CASE("kill ops discard the pixel: a fetch after the kill still runs", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  // The fetch case of Task 15.6's kill test, in a pixel shader: KILLE(c1, c2) with a zero write mask, then r2 = c3
  // (index 1), vfetch r1 from slot 0 at r2.x, oC0 = r1. A surviving pixel shows element 1, a killed one the clear 0.
  xe::Alu kill = vop(xe::kVectorKillEq, 1, c(1), c(2));
  kill.vector_mask = 0;
  const auto ps = translate_ok(
      ShaderStage::Pixel, assemble({exec({kill, mov(2, c(3))}), exec({vf(1, 0, k_32_32_32_32_FLOAT, 4, 0, 2)}),
                                    exec_end({mov(0, r(1), true)})}));
  const auto vs = translate_ok(ShaderStage::Vertex, assemble({exec_end(position())}));
  for (const bool kills : {false, true}) {
    INFO((kills ? "killed" : "kept"));
    HarnessInputs in;
    in.vertex_count = in.width;
    in.topology = lr::testing::HarnessTopology::PointList;
    in.vs_constants = point_constants(in.width);
    in.ps_constants = {{}, {1, 2, 3, 4}, {kills ? 1.0f : 5.0f, 6, 7, 8}, {1, 1, 1, 1}};
    in.fetch_slots = {fetch_slot(0, float4_data(4))};
    auto res = h.draw(vs.spirv, ps.spirv, in);
    if (!res) FAIL("draw: " << res.error().message);
    for (const Pixel& p : res.value().color[0]) check_pixel(p, kills ? Pixel{0, 0, 0, 0} : float4_value(1));
  }
  if (lr::testing::spirv_tools_available()) {
    const std::string dis = lr::testing::spirv_dis(ps.spirv);
    INFO(dis);
    size_t kills = 0;
    for (size_t at = dis.find("OpKill"); at != std::string::npos; at = dis.find("OpKill", at + 1)) ++kills;
    CHECK(kills == 1);
  }
}

// --- errors -------------------------------------------------------------------------------------------------------

TEST_CASE("unknown vertex format fails naming it", "[unit][gpu]") {
  for (const uint32_t format : {0u, 5u, 39u, 63u}) {
    INFO("format " << format);
    const auto t = try_translate(ShaderStage::Vertex, assemble(fetch_vs({vf(3, 0, format, 4)})));
    REQUIRE_FALSE(t);
    CHECK(t.error().message == "instruction 4 (fetch): vertex format " + std::to_string(format) + " is not supported");
  }
  // A known format translates.
  CHECK(static_cast<bool>(try_translate(ShaderStage::Vertex, assemble(fetch_vs({vf(3, 0, k_32_32_32_FLOAT, 3)})))));
}
