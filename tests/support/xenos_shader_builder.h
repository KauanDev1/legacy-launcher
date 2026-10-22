// SPDX-License-Identifier: MIT
// Test helper: hand-assembled Xenos microcode (the dwords a game loads with IM_LOAD / IM_LOAD_IMMEDIATE). Never game
// data: every word here is written by the tests.
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

// The microcode as the big-endian bytes it has in guest memory: what the microcode hash covers.
inline std::vector<uint8_t> microcode_bytes(const std::vector<uint32_t>& microcode) {
  std::vector<uint8_t> out;
  for (uint32_t w : microcode)
    for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(w >> s));
  return out;
}

// Appends instruction triples to a microcode vector.
inline void emit(std::vector<uint32_t>& code, const std::array<uint32_t, 3>& words) {
  code.insert(code.end(), words.begin(), words.end());
}

}  // namespace lr::testing::xenos
