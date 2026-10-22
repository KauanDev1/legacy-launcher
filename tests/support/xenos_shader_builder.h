// SPDX-License-Identifier: MIT
// Test helper: synthetic Xenos shader containers (the D3D-for-Xbox-360 layout that XenosRecomp scans for) built
// from hand-assembled microcode. Never game data: every byte here is written by the tests.
//
// Container layout (all big-endian), as XenosRecomp reads it (thirdparty/XenosRecomp/XenosRecomp/shader.h):
//   0x00 header: flags (0x102A11xx, bit 0 = vertex shader), virtualSize, physicalSize, fieldC,
//        constantTableOffset, definitionTableOffset, shaderOffset, field1C (0), field20 (0)
//   constant table: u32 size, D3DXSHADER_CONSTANTTABLE (7 u32), D3DXSHADER_CONSTANTINFO[] (20 bytes each), names
//   definition table (optional): 5 u32 header, float4 defs, 0, int4 defs, 0
//   shader: physicalOffset, size, field8, fieldC (PS: svPos register in bits 8..15), field10,
//           interpolatorInfo (count in bits 5..9), then VS: field18, vertexElementCount, field20, vertex elements,
//           interpolators; PS: field18, outputs, interpolators
//   physical part (at virtualSize): float definition values, then the microcode at shader.physicalOffset
//
// Microcode encodings follow the public Xenos ucode layout (control flow: 48-bit instructions packed two per three
// dwords; ALU and fetch: three dwords each). Written from the field tables, no code copied.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace lr::testing::xenos {

// --- control flow (48-bit instructions) ---------------------------------------------------------------------------

enum CfOpcode : uint64_t {
  kCfNop = 0,
  kCfExec = 1,
  kCfExecEnd = 2,
  kCfCondExec = 3,
  kCfCondExecEnd = 4,
  kCfCondExecPred = 5,
  kCfCondExecPredEnd = 6,
  kCfLoopStart = 7,
  kCfLoopEnd = 8,
};

inline uint64_t cf_opcode(uint64_t opcode) { return opcode << 44; }

// exec / exec_end: run `count` instructions from slot `address`; `sequence` has 2 bits per instruction (bit 0 =
// fetch, bit 1 = serialize).
inline uint64_t cf_exec(bool end, uint32_t address, uint32_t count, uint32_t sequence) {
  return (address & 0xFFFull) | (uint64_t(count & 7) << 12) | (uint64_t(sequence & 0xFFF) << 16) |
         cf_opcode(end ? kCfExecEnd : kCfExec);
}

// cond_exec: like exec, but only when bool constant `bool_address` equals `condition`.
inline uint64_t cf_cond_exec(bool end, uint32_t address, uint32_t count, uint32_t sequence, uint32_t bool_address,
                             bool condition) {
  return (address & 0xFFFull) | (uint64_t(count & 7) << 12) | (uint64_t(sequence & 0xFFF) << 16) |
         (uint64_t(bool_address & 0xFF) << 34) | (uint64_t(condition ? 1 : 0) << 42) |
         cf_opcode(end ? kCfCondExecEnd : kCfCondExec);
}

// loop_start: loop constant `loop_id`; `address` = CF index to skip to when the count is 0; `repeat` keeps the
// current aL instead of loading the start value.
inline uint64_t cf_loop_start(uint32_t loop_id, uint32_t address, bool repeat = false) {
  return (address & 0x1FFFull) | (uint64_t(repeat ? 1 : 0) << 13) | (uint64_t(loop_id & 31) << 16) |
         cf_opcode(kCfLoopStart);
}

// loop_end: loop constant `loop_id`; `address` = CF index of the first body instruction.
inline uint64_t cf_loop_end(uint32_t loop_id, uint32_t address) {
  return (address & 0x1FFFull) | (uint64_t(loop_id & 31) << 16) | cf_opcode(kCfLoopEnd);
}

inline uint64_t cf_nop() { return 0; }

// Two CF instructions packed into one 3-dword slot.
inline std::array<uint32_t, 3> cf_pair(uint64_t a, uint64_t b) {
  return {uint32_t(a), uint32_t((a >> 32) & 0xFFFF) | uint32_t((b & 0xFFFF) << 16), uint32_t(b >> 16)};
}

// --- ALU (3 dwords) -----------------------------------------------------------------------------------------------

inline constexpr uint32_t kScalarRetainPrev = 50;
inline constexpr uint32_t kVectorAdd = 0;
inline constexpr uint32_t kVectorMul = 1;
inline constexpr uint32_t kVectorMax = 2;

// Export destinations (vector_dest with export set).
inline constexpr uint32_t kExportVsInterpolator0 = 0;
inline constexpr uint32_t kExportVsPosition = 62;
inline constexpr uint32_t kExportPsColor0 = 0;
inline constexpr uint32_t kExportPsColor1 = 1;

struct AluSrc {
  uint32_t reg = 0;
  bool temp = true;  // true: temporary register r<reg>; false: float constant c<reg>
  uint32_t swizzle = 0;  // component-relative; 0 = xyzw
  bool negate = false;
};

struct Alu {
  uint32_t vector_op = kVectorMax;
  uint32_t scalar_op = kScalarRetainPrev;
  uint32_t vector_dest = 0;
  uint32_t vector_mask = 0xF;
  uint32_t scalar_dest = 0;
  uint32_t scalar_mask = 0;
  bool export_data = false;
  AluSrc src1, src2, src3;
  // Relative constant addressing (index + aL, or + a0 with address_register_relative): const0 applies to the first
  // constant operand (src1, src2, src3 order), const1 to a later one.
  bool const0_relative = false;
  bool const1_relative = false;
  bool address_register_relative = false;
};

inline std::array<uint32_t, 3> alu(const Alu& a) {
  const uint32_t w0 = (a.vector_dest & 0x3F) | ((a.scalar_dest & 0x3F) << 8) | ((a.export_data ? 1u : 0u) << 15) |
                      ((a.vector_mask & 0xF) << 16) | ((a.scalar_mask & 0xF) << 20) | ((a.scalar_op & 0x3F) << 26);
  const uint32_t w1 = (a.src3.swizzle & 0xFF) | ((a.src2.swizzle & 0xFF) << 8) | ((a.src1.swizzle & 0xFF) << 16) |
                      ((a.src3.negate ? 1u : 0u) << 24) | ((a.src2.negate ? 1u : 0u) << 25) |
                      ((a.src1.negate ? 1u : 0u) << 26) | ((a.address_register_relative ? 1u : 0u) << 29) |
                      ((a.const1_relative ? 1u : 0u) << 30) | ((a.const0_relative ? 1u : 0u) << 31);
  const uint32_t w2 = (a.src3.reg & 0xFF) | ((a.src2.reg & 0xFF) << 8) | ((a.src1.reg & 0xFF) << 16) |
                      ((a.vector_op & 0x1F) << 24) | ((a.src3.temp ? 1u : 0u) << 29) |
                      ((a.src2.temp ? 1u : 0u) << 30) | ((a.src1.temp ? 1u : 0u) << 31);
  return {w0, w1, w2};
}

// --- fetch (3 dwords) ---------------------------------------------------------------------------------------------

inline constexpr uint32_t kSwizzleXyzw = 0 | (1 << 3) | (2 << 6) | (3 << 9);  // destination swizzle, 3 bits each
inline constexpr uint32_t kFormat32_32_32_32Float = 38;
inline constexpr uint32_t kTextureDim1D = 0;
inline constexpr uint32_t kTextureDim2D = 1;

// vfetch r<dst> from vertex fetch constant `slot` (0..95 = const_index * 3 + const_index_select).
inline std::array<uint32_t, 3> vfetch(uint32_t dst, uint32_t src, uint32_t slot, uint32_t offset_dwords,
                                      uint32_t stride_dwords, uint32_t format) {
  const uint32_t w0 = 0u /*vertex fetch*/ | ((src & 0x3F) << 5) | ((dst & 0x3F) << 12) | (1u << 19) |
                      (((slot / 3) & 0x1F) << 20) | (((slot % 3) & 3) << 25);
  const uint32_t w1 = kSwizzleXyzw | ((format & 0x3F) << 16);
  const uint32_t w2 = (stride_dwords & 0xFF) | ((offset_dwords & 0x7FFFFF) << 8);
  return {w0, w1, w2};
}

// tfetch r<dst> = texture fetch constant `const_index` sampled at r<src> (source swizzle: 2 bits per component).
inline std::array<uint32_t, 3> tfetch(uint32_t dst, uint32_t src, uint32_t const_index, uint32_t dimension,
                                      uint32_t src_swizzle = 0 | (1 << 2) | (2 << 4)) {
  const uint32_t w0 = 1u /*texture fetch*/ | ((src & 0x3F) << 5) | ((dst & 0x3F) << 12) |
                      ((const_index & 0x1F) << 20) | ((src_swizzle & 0x3F) << 26);
  const uint32_t w1 = kSwizzleXyzw;
  const uint32_t w2 = (dimension & 3) << 14;
  return {w0, w1, w2};
}

// --- container ----------------------------------------------------------------------------------------------------

enum RegisterSet : uint16_t { kBool = 0, kInt4 = 1, kFloat4 = 2, kSampler = 3 };
enum DeclUsage : uint32_t { kPosition = 0, kTexCoord = 5, kColor = 10 };

struct Constant {
  std::string name;
  RegisterSet set = kFloat4;
  uint16_t index = 0;
  uint16_t count = 1;
};

struct VertexElement {
  uint32_t address = 0;  // instruction slot of the vfetch that reads it
  uint32_t usage = kPosition;
  uint32_t usage_index = 0;
};

struct Interpolator {
  uint32_t usage = kTexCoord;
  uint32_t usage_index = 0;
  uint32_t reg = 0;
};

struct ShaderDesc {
  bool vertex = true;
  std::vector<Constant> constants;
  std::vector<VertexElement> vertex_elements;  // VS only
  std::vector<Interpolator> interpolators;
  uint32_t ps_outputs = 1;                     // PS only: bit 0 = COLOR0 ... bit 4 = depth
  std::vector<uint32_t> microcode;             // host-order dwords
  uint32_t shader_offset_override = 0;         // != 0: a broken container whose shader header is out of range
};

namespace detail {
inline void put32(std::vector<uint8_t>& out, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s));
}
inline void put16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v));
}
inline void set32(std::vector<uint8_t>& out, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) out[at + i] = uint8_t(v >> (24 - 8 * i));
}
inline void align4(std::vector<uint8_t>& out) {
  while (out.size() % 4) out.push_back(0);
}
}  // namespace detail

// The microcode exactly as it sits in the container (big-endian bytes): what the microcode hash covers.
inline std::vector<uint8_t> microcode_bytes(const ShaderDesc& d) {
  std::vector<uint8_t> out;
  for (uint32_t w : d.microcode) detail::put32(out, w);
  return out;
}

inline std::vector<uint8_t> build_container(const ShaderDesc& d) {
  using namespace detail;
  std::vector<uint8_t> out(36, 0);
  set32(out, 0, 0x102A1100u | (d.vertex ? 1u : 0u));

  // Constant table: u32 size, then D3DXSHADER_CONSTANTTABLE; offsets inside it are relative to its start.
  const size_t ct_container = out.size();
  set32(out, 16, uint32_t(ct_container));
  put32(out, 0);  // container size, patched below
  const size_t ct = out.size();
  const uint32_t info_offset = 28;
  put32(out, 28);                                   // size
  put32(out, 0);                                    // creator
  put32(out, 0xFFFE0300u);                          // version
  put32(out, uint32_t(d.constants.size()));         // constants
  put32(out, info_offset);                          // constantInfo
  put32(out, 0);                                    // flags
  put32(out, 0);                                    // target
  uint32_t name_at = info_offset + uint32_t(d.constants.size()) * 20;
  for (const auto& c : d.constants) {
    put32(out, name_at);
    put16(out, c.set);
    put16(out, c.index);
    put16(out, c.count);
    put16(out, 0);
    put32(out, 0);  // typeInfo
    put32(out, 0);  // defaultValue
    name_at += uint32_t(c.name.size()) + 1;
  }
  for (const auto& c : d.constants) {
    out.insert(out.end(), c.name.begin(), c.name.end());
    out.push_back(0);
  }
  align4(out);
  set32(out, ct_container, uint32_t(out.size() - ct));

  // No definition table (offset 0).
  // Shader header.
  const size_t shader = out.size();
  set32(out, 24, d.shader_offset_override ? d.shader_offset_override : uint32_t(shader));
  const uint32_t microcode_size = uint32_t(d.microcode.size() * 4);
  put32(out, 0);                                                   // physicalOffset
  put32(out, microcode_size);                                      // size
  put32(out, 0);                                                   // field8
  put32(out, d.vertex ? 0u : 0xFF00u);                             // fieldC: PS svPos register 0xFF (none)
  put32(out, 0);                                                   // field10
  put32(out, uint32_t(d.interpolators.size()) << 5);               // interpolatorInfo
  auto interp_word = [](const Interpolator& i) {
    return (i.usage_index & 0xF) | ((i.usage & 0xF) << 4) | ((i.reg & 0xF) << 8);
  };
  if (d.vertex) {
    put32(out, 0);                                                 // field18: words before the vertex elements
    put32(out, uint32_t(d.vertex_elements.size()));
    put32(out, 0);                                                 // field20
    for (const auto& e : d.vertex_elements)
      put32(out, (e.address & 0xFFF) | ((e.usage & 0xF) << 12) | ((e.usage_index & 0xF) << 16));
    for (const auto& i : d.interpolators) put32(out, interp_word(i));
  } else {
    put32(out, 0);                                                 // field18
    put32(out, d.ps_outputs);
    for (const auto& i : d.interpolators) put32(out, interp_word(i));
  }
  align4(out);

  const uint32_t virtual_size = uint32_t(out.size());
  set32(out, 4, virtual_size);
  set32(out, 8, microcode_size);
  for (uint32_t w : d.microcode) put32(out, w);
  return out;
}

// Appends instruction triples to a microcode vector.
inline void emit(std::vector<uint32_t>& code, const std::array<uint32_t, 3>& words) {
  code.insert(code.end(), words.begin(), words.end());
}

}  // namespace lr::testing::xenos
