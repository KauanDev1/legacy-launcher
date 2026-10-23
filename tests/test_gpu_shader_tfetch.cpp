// SPDX-License-Identifier: MIT
// Texture fetch inside the translated shaders (plan B, Task 15.9; translator_fetch.cpp): the bindless heaps through the
// shared block's index tables, 1D (a height-1 2D texture), 2D, 3D and cube fetches (the cube one after the CUBE op),
// explicit LOD in the vertex shader, LOD bias, the register LOD, register gradients and getGradients, per-instruction
// offsets, unnormalized coordinates, predicated fetches and the variants that fail. Shaders are synthetic microcode
// (support/xenos_shader_builder.h); every module goes through spirv-val, and every value is checked on lavapipe
// through support/vk_shader_harness.h. Expected values follow the Xenos semantics as Xenia's SPIR-V translator
// describes them (reference only, nothing copied) and the Vulkan sampling rules.
//
// The vertex shader reads its vertices with vfetch from slot 0 (position, then the interpolators, float4 each) and
// exports them. Most draws are point lists (vertex i on pixel i, so every pixel gets its own coordinates and the
// implicit derivatives are 0); the LOD tests that need real derivatives draw one triangle that covers the target with
// u = (x_ndc + 1) / 2, so du/dx is 1 / width per pixel.
//
// Textures hold texel_value(x, y, layer, mip) = (x, 10 + y, 20 + layer, 30 + mip), where the layer is the cube face or
// the 3D slice, and are sampled with nearest filtering (min, mag and mip). Each test binds a decoy texture first, so
// the texture under test sits at heap index 1 and only the index table can find it.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
using lr::testing::HarnessInputs;
using lr::testing::HarnessSampler;
using lr::testing::HarnessTexture;
using lr::testing::HarnessTextureDim;
using lr::testing::ShaderHarness;
using Pixel = std::array<float, 4>;

namespace {

constexpr uint32_t kXXXX = 0x6C;  // ALU source swizzle .xxxx
constexpr uint32_t kZZXY = 0xA6;  // CUBE's src1
constexpr uint32_t kYXZZ = 0xCD;  // CUBE's src2
constexpr uint32_t kWReadsZ = 0xC0;  // scalar operand a (the w slot) reads z
constexpr uint32_t kFloat4 = xe::kFormat32_32_32_32Float;

// tfetch source swizzles: 2 bits per coordinate, absolute.
constexpr uint32_t src_swizzle(uint32_t x, uint32_t y, uint32_t z) { return x | (y << 2) | (z << 4); }
// tfetch destination swizzles: 3 bits per component (0..3 = x..w, 4 = 0, 5 = 1, 7 = keep).
constexpr uint32_t dst_swizzle(uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
  return x | (y << 3) | (z << 6) | (w << 9);
}

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
// A scalar op on src3 (operand a = the swizzled w) into r<dest>.x, with no vector result.
xe::Alu sop(uint32_t op, uint32_t dest, xe::AluSrc s) {
  xe::Alu alu;
  alu.vector_mask = 0;
  alu.scalar_op = op;
  alu.scalar_dest = dest;
  alu.scalar_mask = 1;
  alu.src3 = s;
  return alu;
}
// A scalar predicate set on s.w (no register written).
xe::Alu pred_set(uint32_t op, xe::AluSrc s) {
  xe::Alu alu = sop(op, 0, s);
  alu.scalar_mask = 0;
  return alu;
}

// tfetch r<dst> from r<src> with texture fetch constant `const_index`; every filter from the fetch constant.
xe::TFetch tf(uint32_t dst, uint32_t src, uint32_t const_index, uint32_t dimension) {
  xe::TFetch f;
  f.dst = dst;
  f.src = src;
  f.const_index = const_index;
  f.dimension = dimension;
  return f;
}
// One of the non-sampling texture opcodes on r<src>.
xe::TFetch tex_op(uint32_t opcode, uint32_t src, uint32_t dst = 0) {
  xe::TFetch f = tf(dst, src, 0, xe::kTextureDim2D);
  f.opcode = opcode;
  return f;
}

using Instr = std::variant<xe::Alu, xe::VFetch, xe::TFetch>;
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
      if (!std::holds_alternative<xe::Alu>(instrs[k.first + i])) sequence |= 1u << (2 * i);
    words.push_back(
        xe::cf_exec(k.end, cf_slots + static_cast<uint32_t>(k.first), static_cast<uint32_t>(k.count), sequence));
  }
  if (words.size() % 2) words.push_back(xe::cf_nop());
  std::vector<uint32_t> code;
  for (size_t i = 0; i < words.size(); i += 2) xe::emit(code, xe::cf_pair(words[i], words[i + 1]));
  for (const Instr& in : instrs) {
    if (const auto* a = std::get_if<xe::Alu>(&in))
      xe::emit(code, xe::alu(*a));
    else if (const auto* v = std::get_if<xe::VFetch>(&in))
      xe::emit(code, xe::vfetch(*v));
    else
      xe::emit(code, xe::tfetch(std::get<xe::TFetch>(in)));
  }
  return code;
}

// The VS reads vertex r0.x of slot 0 (position, then `n` interpolators, float4 each) into r1 (position) and
// r2..r(n+1), runs `extra`, then exports the position and interpolator k = r<exported[k]> (default r(k+2)).
std::vector<Cf> vertex_shader(uint32_t n, std::vector<Instr> extra = {}, std::vector<uint32_t> exported = {}) {
  const uint32_t stride = 4 * (n + 1);
  xe::VFetch full;
  full.dst = 1;
  full.slot = 0;
  full.format = kFloat4;
  full.stride_dwords = stride;
  std::vector<Instr> fetches = {full};
  for (uint32_t k = 0; k < n; ++k) {
    xe::VFetch mini;
    mini.dst = 2 + k;
    mini.format = kFloat4;
    mini.offset_dwords = static_cast<int32_t>(4 * (k + 1));
    mini.mini = true;
    fetches.push_back(mini);
  }
  if (exported.empty())
    for (uint32_t k = 0; k < n; ++k) exported.push_back(2 + k);
  std::vector<Instr> exports = {mov(sh::kExportVsPosition, r(1), true)};
  for (uint32_t k = 0; k < exported.size(); ++k) exports.push_back(mov(k, r(exported[k]), true));
  std::vector<Cf> vs = {exec(std::move(fetches))};
  if (!extra.empty()) vs.push_back(exec(std::move(extra)));
  vs.push_back(exec_end(std::move(exports)));
  return vs;
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

void check_pixel(const Pixel& got, const Pixel& want, float tolerance = 0.0f) {
  INFO("got " << text(got) << ", want " << text(want));
  for (uint32_t k = 0; k < 4; ++k) CHECK(std::fabs(got[k] - want[k]) <= tolerance);
}

size_t count_of(const std::string& haystack, const std::string& needle) {
  size_t n = 0;
  for (size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + 1)) ++n;
  return n;
}

struct Vertex {
  Pixel position;
  std::vector<Pixel> interpolators;
};

// Vertex i of a point list on a `width` x 1 target lands on pixel i.
Vertex point(uint32_t i, uint32_t width, std::vector<Pixel> interpolators) {
  const float x = (2.0f * static_cast<float>(i) + 1.0f) / static_cast<float>(width) - 1.0f;
  return {{x, 0, 0, 1}, std::move(interpolators)};
}
// One triangle that covers the target: interpolator 0 = ((x + 1) / 2, v, (y + 1) / 2, 0) at NDC (x, y).
std::vector<Vertex> covering_triangle(float v) {
  std::vector<Vertex> out;
  for (const auto& [x, y] : {std::pair{-1.0f, -1.0f}, std::pair{3.0f, -1.0f}, std::pair{-1.0f, 3.0f}})
    out.push_back({{x, y, 0, 1}, {Pixel{(x + 1) / 2, v, (y + 1) / 2, 0}}});
  return out;
}

struct Draw {
  std::vector<Vertex> vertices;
  std::vector<Cf> ps;
  std::vector<Cf> vs;  // default: vertex_shader(interpolator count)
  bool points = true;
};

// A point-list draw of `vertices` with the default VS and pixel shader `ps`.
Draw on_points(std::vector<Vertex> vertices, std::vector<Cf> ps) {
  Draw d;
  d.vertices = std::move(vertices);
  d.ps = std::move(ps);
  return d;
}

// Draws and returns colour target 0 (in.width pixels).
std::vector<Pixel> run(ShaderHarness& h, const Draw& d, HarnessInputs in) {
  REQUIRE_FALSE(d.vertices.empty());
  const auto n = static_cast<uint32_t>(d.vertices[0].interpolators.size());
  std::vector<uint8_t> bytes;
  for (const Vertex& v : d.vertices) {
    REQUIRE(v.interpolators.size() == n);
    std::vector<Pixel> row = {v.position};
    row.insert(row.end(), v.interpolators.begin(), v.interpolators.end());
    for (const Pixel& p : row)
      for (float f : p) {
        const auto u = std::bit_cast<uint32_t>(f);
        for (int b = 0; b < 4; ++b) bytes.push_back(static_cast<uint8_t>(u >> (8 * b)));
      }
  }
  lr::testing::HarnessFetchSlot slot;
  slot.slot = 0;
  slot.bytes = std::move(bytes);
  in.fetch_slots = {slot};
  in.vertex_count = static_cast<uint32_t>(d.vertices.size());
  in.topology = d.points ? lr::testing::HarnessTopology::PointList : lr::testing::HarnessTopology::TriangleList;
  const auto vs_t = translate_ok(ShaderStage::Vertex, assemble(d.vs.empty() ? vertex_shader(n) : d.vs));
  const auto ps_t = translate_ok(ShaderStage::Pixel, assemble(d.ps));
  auto res = h.draw(vs_t.spirv, ps_t.spirv, in);
  if (!res) FAIL("draw: " << res.error().message);
  REQUIRE(res.value().color[0].size() == in.width);
  return res.value().color[0];
}

// --- textures -----------------------------------------------------------------------------------------------------

Pixel texel_value(uint32_t x, uint32_t y, uint32_t layer, uint32_t mip) {
  return {static_cast<float>(x), 10.0f + static_cast<float>(y), 20.0f + static_cast<float>(layer),
          30.0f + static_cast<float>(mip)};
}

uint32_t mip_size(uint32_t size, uint32_t mip) { return std::max<uint32_t>(size >> mip, 1); }

HarnessTexture texture(uint32_t fetch_slot, HarnessTextureDim dim, uint32_t w, uint32_t h, uint32_t d = 1,
                       uint32_t mips = 1) {
  HarnessTexture t;
  t.fetch_slot = fetch_slot;
  t.dim = dim;
  t.w = w;
  t.h = h;
  t.d = d;
  t.mips = mips;
  for (uint32_t m = 0; m < mips; ++m) {
    const uint32_t layers = dim == HarnessTextureDim::kCube ? 6 : dim == HarnessTextureDim::k3D ? mip_size(d, m) : 1;
    for (uint32_t l = 0; l < layers; ++l)
      for (uint32_t y = 0; y < mip_size(h, m); ++y)
        for (uint32_t x = 0; x < mip_size(w, m); ++x) t.texels.push_back(texel_value(x, y, l, m));
  }
  return t;
}

// A 1x1 texture of -1 for an unused fetch constant: bound first, it takes heap index 0.
HarnessTexture decoy(HarnessTextureDim dim) {
  HarnessTexture t;
  t.fetch_slot = 31;
  t.dim = dim;
  t.texels.assign(dim == HarnessTextureDim::kCube ? 6 : 1, Pixel{-1, -1, -1, -1});
  return t;
}

// Inputs with the decoy and `t` in t's heap, and a nearest clamp-to-edge sampler for t's fetch constant.
HarnessInputs inputs(uint32_t width, HarnessTexture t) {
  HarnessInputs in;
  in.width = width;
  const uint32_t slot = t.fetch_slot;
  in.textures = {decoy(t.dim), std::move(t)};
  HarnessSampler s;
  s.fetch_slot = slot;
  in.samplers = {s};
  return in;
}

// The texel nearest filtering picks at normalized (u, v) of mip `mip` of a `size` x `size` 2D texture.
Pixel texel_at(float u, float v, uint32_t size, uint32_t mip) {
  const uint32_t s = mip_size(size, mip);
  const auto pick = [&](float t) {
    return std::min(static_cast<uint32_t>(std::floor(t * static_cast<float>(s))), s - 1);
  };
  return texel_value(pick(u), pick(v), 0, mip);
}

}  // namespace

// --- dimensions ---------------------------------------------------------------------------------------------------

TEST_CASE("2D fetch samples the bound texture", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  constexpr uint32_t kSlot = 5;
  std::vector<Vertex> verts;
  std::vector<Pixel> want;
  for (uint32_t ty = 0; ty < 4; ++ty)
    for (uint32_t tx = 0; tx < 4; ++tx) {
      const float u = (static_cast<float>(tx) + 0.5f) / 4, v = (static_cast<float>(ty) + 0.5f) / 4;
      verts.push_back(point(static_cast<uint32_t>(verts.size()), 16, {Pixel{u, v, 0.9f, 0.1f}}));
      want.push_back(texel_value(tx, ty, 0, 0));
    }

  SECTION("coordinates xy, result xyzw") {
    const Draw d = on_points(verts, {exec_end({tf(1, 0, kSlot, xe::kTextureDim2D), mov(0, r(1), true)})});
    const auto out = run(h, d, inputs(16, texture(kSlot, HarnessTextureDim::k2D, 4, 4)));
    for (size_t i = 0; i < want.size(); ++i) {
      INFO("pixel " << i);
      check_pixel(out[i], want[i]);
    }
  }
  SECTION("source and destination swizzles") {
    // Coordinates from (w, y): u sits in r0.w. Result: x = texel.w, y = 1, z kept (300), w = texel.x.
    for (Vertex& v : verts) v.interpolators[0] = {0.7f, v.interpolators[0][1], 0.2f, v.interpolators[0][0]};
    xe::TFetch f = tf(1, 0, kSlot, xe::kTextureDim2D);
    f.src_swizzle = src_swizzle(3, 1, 0);
    f.dst_swizzle = dst_swizzle(3, sh::kFetchSwizzleOne, sh::kFetchSwizzleKeep, 0);
    const Draw d = on_points(verts, {exec_end({mov(1, c(0)), f, mov(0, r(1), true)})});
    HarnessInputs in = inputs(16, texture(kSlot, HarnessTextureDim::k2D, 4, 4));
    in.ps_constants = {{100, 200, 300, 400}};
    const auto out = run(h, d, in);
    for (size_t i = 0; i < want.size(); ++i) {
      INFO("pixel " << i);
      check_pixel(out[i], {want[i][3], 1, 300, want[i][0]});
    }
  }
}

TEST_CASE("1D fetch samples a height-1 texture", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  constexpr uint32_t kSlot = 2;
  // Only x is a coordinate: y (7.3) and z are ignored, the texture is the height-1 Texture2D of set 0.
  std::vector<Vertex> verts;
  for (uint32_t tx = 0; tx < 4; ++tx)
    verts.push_back(point(tx, 4, {Pixel{(static_cast<float>(tx) + 0.5f) / 4, 7.3f, -5.0f, 0}}));
  const Draw d = on_points(verts, {exec_end({tf(1, 0, kSlot, xe::kTextureDim1D), mov(0, r(1), true)})});
  const auto out = run(h, d, inputs(4, texture(kSlot, HarnessTextureDim::k2D, 4, 1)));
  for (uint32_t tx = 0; tx < 4; ++tx) {
    INFO("pixel " << tx);
    check_pixel(out[tx], texel_value(tx, 0, 0, 0));
  }
}

TEST_CASE("3D fetch", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  constexpr uint32_t kSlot = 3;
  std::vector<Vertex> verts;
  std::vector<Pixel> want;
  for (uint32_t tz = 0; tz < 4; ++tz)
    for (uint32_t ty = 0; ty < 4; ++ty)
      for (uint32_t tx = 0; tx < 4; ++tx) {
        const auto at = [](uint32_t t) { return (static_cast<float>(t) + 0.5f) / 4; };
        verts.push_back(point(static_cast<uint32_t>(verts.size()), 64, {Pixel{at(tx), at(ty), at(tz), 0}}));
        want.push_back(texel_value(tx, ty, tz, 0));
      }
  const Draw d = on_points(verts, {exec_end({tf(1, 0, kSlot, xe::kTextureDim3D), mov(0, r(1), true)})});
  const auto out = run(h, d, inputs(64, texture(kSlot, HarnessTextureDim::k3D, 4, 4, 4)));
  for (size_t i = 0; i < want.size(); ++i) {
    INFO("pixel " << i);
    check_pixel(out[i], want[i]);
  }
}

namespace {

// The Vulkan cube rule (major axis, sc, tc, ma) for direction r: the face and the texel of a size x size face.
Pixel cube_reference(const Pixel& r, uint32_t size) {
  const float ax = std::fabs(r[0]), ay = std::fabs(r[1]), az = std::fabs(r[2]);
  uint32_t face = 0;
  float sc = 0, tc = 0, ma = 0;
  if (ax >= ay && ax >= az) {
    face = r[0] >= 0 ? 0 : 1;
    sc = r[0] >= 0 ? -r[2] : r[2];
    tc = -r[1];
    ma = ax;
  } else if (ay >= az) {
    face = r[1] >= 0 ? 2 : 3;
    sc = r[0];
    tc = r[1] >= 0 ? r[2] : -r[2];
    ma = ay;
  } else {
    face = r[2] >= 0 ? 4 : 5;
    sc = r[2] >= 0 ? r[0] : -r[0];
    tc = -r[1];
    ma = az;
  }
  const auto pick = [&](float c) {
    const float t = 0.5f * (c / ma + 1.0f);
    return std::min(static_cast<uint32_t>(std::floor(t * static_cast<float>(size))), size - 1);
  };
  return texel_value(pick(sc), pick(tc), face, 0);
}

}  // namespace

TEST_CASE("cube fetch after the cube op", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  constexpr uint32_t kSlot = 4;
  // Every face, every quadrant of a 2x2 face: direction = +-1 on the major axis, +-0.5 on the other two.
  std::vector<Vertex> verts;
  std::vector<Pixel> want;
  for (uint32_t face = 0; face < 6; ++face)
    for (const float a : {-0.5f, 0.5f})
      for (const float b : {-0.5f, 0.5f}) {
        Pixel dir{a, b, a, 0};
        const uint32_t axis = face / 2;
        dir[axis] = face % 2 ? -1.0f : 1.0f;
        dir[(axis + 1) % 3] = a;
        dir[(axis + 2) % 3] = b;
        verts.push_back(point(static_cast<uint32_t>(verts.size()), 24, {dir}));
        want.push_back(cube_reference(dir, 2));
      }
  // What a compiled cube lookup does: cube r1 = (tc, sc, 2 * ma, face) of r0.xyz; r2.x = 1 / |2 * ma|;
  // r1.xy = r1.xy * r2.x + 1.5 (face coordinates in 1..2); tfetchCube r3, r1.yxw (S, T, face).
  xe::Alu rcp = sop(xe::kScalarRecip, 2, r(1, kWReadsZ));
  rcp.src3.abs = true;
  xe::Alu mad = vop(xe::kVectorMulAdd, 1, r(1), r(2, kXXXX));
  mad.src3 = c(0);
  mad.vector_mask = 0x3;
  xe::TFetch fetch = tf(3, 1, kSlot, xe::kTextureDimCube);
  fetch.src_swizzle = src_swizzle(1, 0, 3);
  const Draw d = on_points(verts,
               {exec_end({vop(xe::kVectorCube, 1, r(0, kZZXY), r(0, kYXZZ)), rcp, mad, fetch, mov(0, r(3), true)})});
  HarnessInputs in = inputs(24, texture(kSlot, HarnessTextureDim::kCube, 2, 2));
  in.ps_constants = {{1.5f, 1.5f, 1.5f, 1.5f}};
  const auto out = run(h, d, in);
  for (size_t i = 0; i < want.size(); ++i) {
    INFO("pixel " << i << ", direction " << text(verts[i].interpolators[0]));
    check_pixel(out[i], want[i]);
  }
}

// --- LOD ----------------------------------------------------------------------------------------------------------

namespace {

constexpr uint32_t kMipSize = 8;  // 8x8, 4 mips (8, 4, 2, 1)
constexpr uint32_t kMips = 4;
constexpr float kU = 0.3f, kV = 0.6f;

HarnessInputs mip_inputs(uint32_t width, uint32_t slot) {
  return inputs(width, texture(slot, HarnessTextureDim::k2D, kMipSize, kMipSize, 1, kMips));
}

}  // namespace

TEST_CASE("vertex shader texture fetch uses explicit lod", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  constexpr uint32_t kSlot = 1;
  // The fork's DXC failure: a VS tfetch with use_computed_lod set (the usual encoding) must still sample with an
  // explicit LOD, 0 + the instruction's bias (implicit LOD does not exist in a vertex shader).
  struct Case {
    int32_t bias_field;  // 1/16 steps
    uint32_t mip;
  };
  for (const Case& k : {Case{0, 0}, Case{16, 1}, Case{32, 2}, Case{-16, 0}, Case{12, 1}}) {
    INFO("lod_bias field " << k.bias_field);
    xe::TFetch f = tf(10, 2, kSlot, xe::kTextureDim2D);
    REQUIRE(f.use_computed_lod);
    f.lod_bias = k.bias_field;
    Draw d;
    for (uint32_t i = 0; i < 2; ++i) d.vertices.push_back(point(i, 2, {Pixel{kU, kV, 0, 0}}));
    d.vs = vertex_shader(1, {f}, {10});
    d.ps = {exec_end({mov(0, r(0), true)})};
    const auto vs = translate_ok(ShaderStage::Vertex, assemble(d.vs));
    if (lr::testing::spirv_tools_available()) {
      const std::string dis = lr::testing::spirv_dis(vs.spirv);
      CHECK(dis.find("OpImageSampleExplicitLod") != std::string::npos);
      CHECK(dis.find("ImplicitLod") == std::string::npos);
    }
    const auto out = run(h, d, mip_inputs(2, kSlot));
    for (uint32_t i = 0; i < 2; ++i) check_pixel(out[i], texel_at(kU, kV, kMipSize, k.mip));
  }
}

TEST_CASE("lod bias and register lod", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  constexpr uint32_t kSlot = 6;

  SECTION("explicit lod = register lod + bias") {
    // setTexLOD r1.x, then tfetch with use_computed_lod clear: LOD = (use_register_lod ? r1.x : 0) + bias.
    struct Case {
      bool use_register_lod;
      int32_t bias_field;
      std::vector<std::pair<float, uint32_t>> lod_to_mip;  // register LOD -> mip (nearest: round, clamped 0..3)
    };
    const std::vector<Case> cases = {
        {true, 0, {{0, 0}, {1, 1}, {2, 2}, {3, 3}, {0.75f, 1}, {5, 3}, {-2, 0}}},
        {true, 16, {{0, 1}, {1, 2}, {2, 3}}},
        {true, -8, {{1.25f, 1}, {2.25f, 2}}},
        {false, 16, {{2, 1}, {0, 1}}},  // the register LOD is set but not used
    };
    for (const Case& k : cases) {
      INFO("use_register_lod " << k.use_register_lod << ", lod_bias field " << k.bias_field);
      const auto n = static_cast<uint32_t>(k.lod_to_mip.size());
      std::vector<Vertex> verts;
      for (uint32_t i = 0; i < n; ++i)
        verts.push_back(point(i, n, {Pixel{kU, kV, 0, 0}, Pixel{k.lod_to_mip[i].first, 0, 0, 0}}));
      xe::TFetch f = tf(2, 0, kSlot, xe::kTextureDim2D);
      f.use_computed_lod = false;
      f.use_register_lod = k.use_register_lod;
      f.lod_bias = k.bias_field;
      const Draw d = on_points(verts, {exec_end({tex_op(xe::kFetchSetLod, 1), f, mov(0, r(2), true)})});
      const auto out = run(h, d, mip_inputs(n, kSlot));
      for (uint32_t i = 0; i < n; ++i) {
        INFO("register lod " << k.lod_to_mip[i].first);
        check_pixel(out[i], texel_at(kU, kV, kMipSize, k.lod_to_mip[i].second));
      }
    }
  }

  SECTION("implicit lod with bias") {
    // An 8-pixel triangle over the 8x8 texture: du/dx = 1/8, so the computed LOD is 0; the bias (and the register
    // LOD) add to it.
    struct Case {
      bool use_register_lod;
      float register_lod;
      int32_t bias_field;
      uint32_t mip;
    };
    for (const Case& k : {Case{false, 0, 0, 0}, Case{false, 0, 16, 1}, Case{true, 1, 16, 2}, Case{true, 3, -16, 2}}) {
      INFO("use_register_lod " << k.use_register_lod << ", lod_bias field " << k.bias_field);
      constexpr float kVConst = 0.0625f;
      Draw d;
      d.points = false;
      d.vertices = covering_triangle(kVConst);
      for (Vertex& v : d.vertices) v.interpolators.push_back({k.register_lod, 0, 0, 0});
      xe::TFetch f = tf(2, 0, kSlot, xe::kTextureDim2D);
      f.use_register_lod = k.use_register_lod;
      f.lod_bias = k.bias_field;
      d.ps = {exec_end({tex_op(xe::kFetchSetLod, 1), f, mov(0, r(2), true)})};
      const auto out = run(h, d, mip_inputs(kMipSize, kSlot));
      for (uint32_t i = 0; i < kMipSize; ++i) {
        INFO("pixel " << i);
        check_pixel(out[i], texel_at((static_cast<float>(i) + 0.5f) / kMipSize, kVConst, kMipSize, k.mip));
      }
    }
  }
}

TEST_CASE("gradients", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  constexpr uint32_t kSlot = 7;

  SECTION("register gradients select the mip") {
    // setGradientsH r1, setGradientsV r2, tfetch with use_register_gradients: LOD = log2(max |gradient| * 8), with
    // the bias scaling the gradients by 2^bias.
    struct Row {
      Pixel h, v;
      uint32_t mip;
    };
    struct Case {
      int32_t bias_field;
      std::vector<Row> rows;
    };
    const float e = 1.0f / kMipSize;
    const std::vector<Case> cases = {
        {0,
         {{{e, 0, 0, 0}, {0, 0, 0, 0}, 0},
          {{2 * e, 0, 0, 0}, {0, 0, 0, 0}, 1},
          {{0, 0, 0, 0}, {0, 4 * e, 0, 0}, 2},
          {{8 * e, 0, 0, 0}, {0, 0, 0, 0}, 3},
          {{e, 0, 0, 0}, {0, 2 * e, 0, 0}, 1}}},
        {16, {{{e, 0, 0, 0}, {0, 0, 0, 0}, 1}, {{2 * e, 0, 0, 0}, {0, 0, 0, 0}, 2}}},
    };
    for (const Case& k : cases) {
      INFO("lod_bias field " << k.bias_field);
      const auto n = static_cast<uint32_t>(k.rows.size());
      std::vector<Vertex> verts;
      for (uint32_t i = 0; i < n; ++i) verts.push_back(point(i, n, {Pixel{kU, kV, 0, 0}, k.rows[i].h, k.rows[i].v}));
      xe::TFetch f = tf(3, 0, kSlot, xe::kTextureDim2D);
      f.use_register_gradients = true;
      f.lod_bias = k.bias_field;
      const Draw d = on_points(verts,
                   {exec_end({tex_op(xe::kFetchSetGradientsH, 1), tex_op(xe::kFetchSetGradientsV, 2), f,
                              mov(0, r(3), true)})});
      const auto out = run(h, d, mip_inputs(n, kSlot));
      for (uint32_t i = 0; i < n; ++i) {
        INFO("row " << i);
        check_pixel(out[i], texel_at(kU, kV, kMipSize, k.rows[i].mip));
      }
    }
  }

  SECTION("getGradients") {
    // (dPdx(x), dPdy(x), dPdx(y), dPdy(y)) of the source: u = (x_ndc + 1) / 2 moves 1/8 per pixel in x; the
    // source y is (y_ndc + 1) / 2 (interpolator z, swizzled into y), which moves 1 per pixel in y on a 1-pixel-high
    // target.
    Draw d;
    d.points = false;
    d.vertices = covering_triangle(0.25f);
    xe::TFetch g = tex_op(xe::kFetchGetGradients, 0, 1);
    g.src_swizzle = src_swizzle(0, 2, 1);
    d.ps = {exec_end({g, mov(0, r(1), true)})};
    const auto out = run(h, d, mip_inputs(kMipSize, kSlot));
    for (uint32_t i = 0; i < kMipSize; ++i) {
      INFO("pixel " << i);
      check_pixel(out[i], {0.125f, 0, 0, 1}, 1e-5f);
    }
  }
}

// --- offsets and unnormalized coordinates -------------------------------------------------------------------------

TEST_CASE("fetch offsets", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  constexpr uint32_t kSlot = 8;
  const auto at = [](uint32_t t, uint32_t size) { return (static_cast<float>(t) + 0.5f) / static_cast<float>(size); };

  SECTION("2D: +1 texel in x, -1 in y") {
    std::vector<Vertex> verts;
    std::vector<Pixel> want;
    for (uint32_t ty = 1; ty < 4; ++ty)
      for (uint32_t tx = 0; tx < 3; ++tx) {
        verts.push_back(point(static_cast<uint32_t>(verts.size()), 9, {Pixel{at(tx, 4), at(ty, 4), 0, 0}}));
        want.push_back(texel_value(tx + 1, ty - 1, 0, 0));
      }
    xe::TFetch f = tf(1, 0, kSlot, xe::kTextureDim2D);
    f.offset_x = 2;  // half texels
    f.offset_y = -2;
    const Draw d = on_points(verts, {exec_end({f, mov(0, r(1), true)})});
    const auto out = run(h, d, inputs(9, texture(kSlot, HarnessTextureDim::k2D, 4, 4)));
    for (size_t i = 0; i < want.size(); ++i) {
      INFO("pixel " << i);
      check_pixel(out[i], want[i]);
    }
  }
  SECTION("3D: +1 texel in z") {
    std::vector<Vertex> verts;
    for (uint32_t tz = 0; tz < 3; ++tz) verts.push_back(point(tz, 3, {Pixel{at(1, 4), at(2, 4), at(tz, 4), 0}}));
    xe::TFetch f = tf(1, 0, kSlot, xe::kTextureDim3D);
    f.offset_z = 2;
    const Draw d = on_points(verts, {exec_end({f, mov(0, r(1), true)})});
    const auto out = run(h, d, inputs(3, texture(kSlot, HarnessTextureDim::k3D, 4, 4, 4)));
    for (uint32_t tz = 0; tz < 3; ++tz) {
      INFO("pixel " << tz);
      check_pixel(out[tz], texel_value(1, 2, tz + 1, 0));
    }
  }
  SECTION("1D: -1 texel in x") {
    std::vector<Vertex> verts;
    for (uint32_t tx = 1; tx < 4; ++tx) verts.push_back(point(tx - 1, 3, {Pixel{at(tx, 4), 0, 0, 0}}));
    xe::TFetch f = tf(1, 0, kSlot, xe::kTextureDim1D);
    f.offset_x = -2;
    f.offset_y = 6;  // ignored: a 1D fetch has no y
    const Draw d = on_points(verts, {exec_end({f, mov(0, r(1), true)})});
    const auto out = run(h, d, inputs(3, texture(kSlot, HarnessTextureDim::k2D, 4, 1)));
    for (uint32_t tx = 1; tx < 4; ++tx) {
      INFO("pixel " << tx - 1);
      check_pixel(out[tx - 1], texel_value(tx - 1, 0, 0, 0));
    }
  }
}

TEST_CASE("unnormalized coordinates on a mipmapped texture", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  constexpr uint32_t kSlot = 9;
  // Texel coordinates divided by the mip-0 size in the shader (OpImageQuerySizeLod), so the sampler stays
  // normalized and mips work: a 4x4 texture with 3 mips, explicit LOD from the bias.
  struct Case {
    const char* name;
    int32_t bias_field;
    int32_t offset_x;  // half texels
  };
  for (const Case& k : {Case{"lod 0", 0, 0}, Case{"lod 1", 16, 0}, Case{"lod 0, +1 texel in x", 0, 2}}) {
    INFO(k.name);
    std::vector<Vertex> verts;
    std::vector<Pixel> want;
    for (uint32_t ty = 0; ty < 4; ++ty)
      for (uint32_t tx = 0; tx < 3; ++tx) {
        verts.push_back(point(static_cast<uint32_t>(verts.size()), 12,
                              {Pixel{static_cast<float>(tx) + 0.5f, static_cast<float>(ty) + 0.5f, 0, 0}}));
        const uint32_t x = tx + static_cast<uint32_t>(k.offset_x / 2);
        const auto mip = static_cast<uint32_t>(k.bias_field / 16);
        want.push_back(texel_value(x >> mip, ty >> mip, 0, mip));
      }
    xe::TFetch f = tf(1, 0, kSlot, xe::kTextureDim2D);
    f.unnormalized = true;
    f.use_computed_lod = false;
    f.lod_bias = k.bias_field;
    f.offset_x = k.offset_x;
    const Draw d = on_points(verts, {exec_end({f, mov(0, r(1), true)})});
    const auto out = run(h, d, inputs(12, texture(kSlot, HarnessTextureDim::k2D, 4, 4, 1, 3)));
    for (size_t i = 0; i < want.size(); ++i) {
      INFO("pixel " << i);
      check_pixel(out[i], want[i]);
    }
  }
}

// --- predication and failures -------------------------------------------------------------------------------------

TEST_CASE("implicit-lod fetch under a predicate uses select", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  constexpr uint32_t kSlot = 10;
  // p0 = (r0.w == 0): true on even pixels. r2 = 7, then a predicated implicit-LOD tfetch into r2.
  const auto program = [&](bool predicated, bool condition) {
    xe::TFetch f = tf(2, 0, kSlot, xe::kTextureDim2D);
    REQUIRE(f.use_computed_lod);
    f.predicated = predicated;
    f.pred_condition = condition;
    return std::vector<Cf>{exec_end({pred_set(xe::kScalarPredSetEq, r(0)), mov(2, c(0)), f, mov(0, r(2), true)})};
  };
  // The fetch is not inside a selection construct: the predicated module has the same structured control flow as
  // the unpredicated one, and merges with OpSelect.
  if (lr::testing::spirv_tools_available()) {
    const std::string plain = lr::testing::spirv_dis(translate_ok(ShaderStage::Pixel, assemble(program(false, true))).spirv);
    const std::string pred = lr::testing::spirv_dis(translate_ok(ShaderStage::Pixel, assemble(program(true, true))).spirv);
    CHECK(count_of(pred, "OpImageSampleImplicitLod") == 1);
    CHECK(count_of(pred, "OpSelectionMerge") == count_of(plain, "OpSelectionMerge"));
    CHECK(count_of(pred, "OpBranchConditional") == count_of(plain, "OpBranchConditional"));
    CHECK(count_of(pred, "OpSelect ") > count_of(plain, "OpSelect "));
  }
  std::vector<Vertex> verts;
  for (uint32_t tx = 0; tx < 4; ++tx)
    verts.push_back(point(tx, 4, {Pixel{(static_cast<float>(tx) + 0.5f) / 4, 0.125f, 0, static_cast<float>(tx % 2)}}));
  for (const bool condition : {true, false}) {
    INFO("pred_condition " << condition);
    HarnessInputs in = inputs(4, texture(kSlot, HarnessTextureDim::k2D, 4, 4));
    in.ps_constants = {{7, 7, 7, 7}};
    const auto out = run(h, on_points(verts, program(true, condition)), in);
    for (uint32_t tx = 0; tx < 4; ++tx) {
      INFO("pixel " << tx);
      const bool fetched = (tx % 2 == 0) == condition;
      check_pixel(out[tx], fetched ? texel_value(tx, 0, 0, 0) : Pixel{7, 7, 7, 7});
    }
  }
}

TEST_CASE("unsupported tfetch variant fails naming it", "[unit]") {
  struct Case {
    ShaderStage stage;
    uint32_t opcode;
    const char* name;
  };
  for (const Case& k : {Case{ShaderStage::Pixel, xe::kFetchGetComputedLod, "get_comp_tex_lod"},
                        Case{ShaderStage::Pixel, xe::kFetchGetWeights, "get_weights"},
                        Case{ShaderStage::Pixel, xe::kFetchGetBorderColorFrac, "get_border_color_frac"},
                        Case{ShaderStage::Vertex, xe::kFetchGetGradients, "get_gradients"}}) {
    INFO(k.name);
    // Slot 1: the instruction right after the CF pair.
    const auto t = try_translate(k.stage, assemble({exec_end({tex_op(k.opcode, 0, 1), mov(0, r(1), true)})}));
    REQUIRE_FALSE(t.ok());
    INFO(t.error().message);
    CHECK(t.error().message.find("instruction 1 (fetch)") != std::string::npos);
    CHECK(t.error().message.find(k.name) != std::string::npos);
    CHECK(t.error().message.find("is not supported") != std::string::npos);
  }
}
