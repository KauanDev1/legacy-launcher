// SPDX-License-Identifier: MIT
// Test helper: hand-assembled Xenos microcode (the dwords a game loads with IM_LOAD / IM_LOAD_IMMEDIATE). Never game
// data: every word here is written by the tests.
//
// Microcode encodings follow the public Xenos ucode layout (control flow: 48-bit instructions packed two per three
// dwords; ALU and fetch: three dwords each). Written from the field tables, no code copied. The decoder that reads
// them back is src/runtime/gpu/shader/ (ucode.h has the field positions).
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
  kCfCondCall = 9,
  kCfReturn = 10,
  kCfCondJmp = 11,
  kCfAlloc = 12,
  kCfCondExecPredClean = 13,
  kCfCondExecPredCleanEnd = 14,
  kCfMarkVsFetchDone = 15,
};

inline uint64_t cf_opcode(uint64_t opcode) { return opcode << 44; }

// The address/count/sequence part shared by every exec form.
inline uint64_t cf_exec_body(uint32_t address, uint32_t count, uint32_t sequence) {
  return (address & 0xFFFull) | (uint64_t(count & 7) << 12) | (uint64_t(sequence & 0xFFF) << 16);
}

// exec / exec_end: run `count` instructions from slot `address`; `sequence` has 2 bits per instruction (bit 0 =
// fetch, bit 1 = serialize).
inline uint64_t cf_exec(bool end, uint32_t address, uint32_t count, uint32_t sequence) {
  return cf_exec_body(address, count, sequence) | cf_opcode(end ? kCfExecEnd : kCfExec);
}

// cond_exec: like exec, but only when bool constant `bool_address` equals `condition`.
inline uint64_t cf_cond_exec(bool end, uint32_t address, uint32_t count, uint32_t sequence, uint32_t bool_address,
                             bool condition) {
  return cf_exec_body(address, count, sequence) | (uint64_t(bool_address & 0xFF) << 34) |
         (uint64_t(condition ? 1 : 0) << 42) | cf_opcode(end ? kCfCondExecEnd : kCfCondExec);
}

// cond_exec_pred: like exec, but only when the predicate equals `condition`.
inline uint64_t cf_cond_exec_pred(bool end, uint32_t address, uint32_t count, uint32_t sequence, bool condition) {
  return cf_exec_body(address, count, sequence) | (uint64_t(condition ? 1 : 0) << 42) |
         cf_opcode(end ? kCfCondExecPredEnd : kCfCondExecPred);
}

// cond_exec_pred_clean: the cond_exec layout (bool constant `bool_address` == `condition`).
inline uint64_t cf_cond_exec_pred_clean(bool end, uint32_t address, uint32_t count, uint32_t sequence,
                                        uint32_t bool_address, bool condition) {
  return cf_exec_body(address, count, sequence) | (uint64_t(bool_address & 0xFF) << 34) |
         (uint64_t(condition ? 1 : 0) << 42) | cf_opcode(end ? kCfCondExecPredCleanEnd : kCfCondExecPredClean);
}

// loop_start: loop constant `loop_id`; `address` = CF index to skip to when the count is 0; `repeat` keeps the
// current aL instead of loading the start value.
inline uint64_t cf_loop_start(uint32_t loop_id, uint32_t address, bool repeat = false) {
  return (address & 0x1FFFull) | (uint64_t(repeat ? 1 : 0) << 13) | (uint64_t(loop_id & 31) << 16) |
         cf_opcode(kCfLoopStart);
}

// loop_end: loop constant `loop_id`; `address` = CF index of the first body instruction. With `predicated_break`
// the loop is left early when the predicate equals `break_condition`.
inline uint64_t cf_loop_end(uint32_t loop_id, uint32_t address, bool predicated_break = false,
                            bool break_condition = false) {
  return (address & 0x1FFFull) | (uint64_t(loop_id & 31) << 16) | (uint64_t(predicated_break ? 1 : 0) << 21) |
         (uint64_t(break_condition ? 1 : 0) << 42) | cf_opcode(kCfLoopEnd);
}

// The shared cond_call / cond_jmp layout. `address` is a CF instruction index.
inline uint64_t cf_branch(CfOpcode op, uint32_t address, bool unconditional, bool predicated, uint32_t bool_address,
                          bool condition) {
  return (address & 0x1FFFull) | (uint64_t(unconditional ? 1 : 0) << 13) | (uint64_t(predicated ? 1 : 0) << 14) |
         (uint64_t(bool_address & 0xFF) << 34) | (uint64_t(condition ? 1 : 0) << 42) | cf_opcode(op);
}

// Unconditional jump to CF instruction `address`.
inline uint64_t cf_jmp(uint32_t address) { return cf_branch(kCfCondJmp, address, true, false, 0, false); }

// Jump when bool constant `bool_address` equals `condition` (or, with `predicated`, when the predicate does).
inline uint64_t cf_cond_jmp(uint32_t address, uint32_t bool_address, bool condition, bool predicated = false) {
  return cf_branch(kCfCondJmp, address, false, predicated, bool_address, condition);
}

// Unconditional call of the subroutine at CF instruction `address`.
inline uint64_t cf_call(uint32_t address) { return cf_branch(kCfCondCall, address, true, false, 0, false); }

// Call when bool constant `bool_address` equals `condition` (or, with `predicated`, when the predicate does).
inline uint64_t cf_cond_call(uint32_t address, uint32_t bool_address, bool condition, bool predicated = false) {
  return cf_branch(kCfCondCall, address, false, predicated, bool_address, condition);
}

inline uint64_t cf_ret() { return cf_opcode(kCfReturn); }

// alloc: `type` 0 none, 1 position, 2 interpolators / colors, 3 memory export; `size` is the 3-bit size field.
inline constexpr uint32_t kAllocNone = 0;
inline constexpr uint32_t kAllocPosition = 1;
inline constexpr uint32_t kAllocInterpolators = 2;
inline constexpr uint32_t kAllocMemory = 3;
inline uint64_t cf_alloc(uint32_t type, uint32_t size = 0) {
  return uint64_t(size & 7) | (uint64_t(type & 3) << 41) | cf_opcode(kCfAlloc);
}

inline uint64_t cf_mark_vs_fetch_done() { return cf_opcode(kCfMarkVsFetchDone); }

inline uint64_t cf_nop() { return 0; }

// Two CF instructions packed into one 3-dword slot.
inline std::array<uint32_t, 3> cf_pair(uint64_t a, uint64_t b) {
  return {uint32_t(a), uint32_t((a >> 32) & 0xFFFF) | uint32_t((b & 0xFFFF) << 16), uint32_t(b >> 16)};
}

// --- ALU (3 dwords) -----------------------------------------------------------------------------------------------

// Vector opcodes.
inline constexpr uint32_t kVectorAdd = 0;
inline constexpr uint32_t kVectorMul = 1;
inline constexpr uint32_t kVectorMax = 2;
inline constexpr uint32_t kVectorMin = 3;
inline constexpr uint32_t kVectorSetEq = 4;
inline constexpr uint32_t kVectorSetGt = 5;
inline constexpr uint32_t kVectorSetGe = 6;
inline constexpr uint32_t kVectorSetNe = 7;
inline constexpr uint32_t kVectorFrac = 8;
inline constexpr uint32_t kVectorTrunc = 9;
inline constexpr uint32_t kVectorFloor = 10;
inline constexpr uint32_t kVectorMulAdd = 11;
inline constexpr uint32_t kVectorCndEq = 12;
inline constexpr uint32_t kVectorCndGe = 13;
inline constexpr uint32_t kVectorCndGt = 14;
inline constexpr uint32_t kVectorDot4 = 15;
inline constexpr uint32_t kVectorDot3 = 16;
inline constexpr uint32_t kVectorDot2Add = 17;
inline constexpr uint32_t kVectorCube = 18;
inline constexpr uint32_t kVectorMax4 = 19;
inline constexpr uint32_t kVectorPredSetEqPush = 20;
inline constexpr uint32_t kVectorPredSetNePush = 21;
inline constexpr uint32_t kVectorPredSetGtPush = 22;
inline constexpr uint32_t kVectorPredSetGePush = 23;
inline constexpr uint32_t kVectorKillEq = 24;
inline constexpr uint32_t kVectorKillGt = 25;
inline constexpr uint32_t kVectorKillGe = 26;
inline constexpr uint32_t kVectorKillNe = 27;
inline constexpr uint32_t kVectorDst = 28;
inline constexpr uint32_t kVectorMaxA = 29;

// Scalar opcodes.
inline constexpr uint32_t kScalarAdd = 0;
inline constexpr uint32_t kScalarAddPrev = 1;
inline constexpr uint32_t kScalarMul = 2;
inline constexpr uint32_t kScalarMulPrev = 3;
inline constexpr uint32_t kScalarMulPrev2 = 4;
inline constexpr uint32_t kScalarMax = 5;
inline constexpr uint32_t kScalarMin = 6;
inline constexpr uint32_t kScalarSetEq = 7;
inline constexpr uint32_t kScalarSetGt = 8;
inline constexpr uint32_t kScalarSetGe = 9;
inline constexpr uint32_t kScalarSetNe = 10;
inline constexpr uint32_t kScalarFrac = 11;
inline constexpr uint32_t kScalarTrunc = 12;
inline constexpr uint32_t kScalarFloor = 13;
inline constexpr uint32_t kScalarExp = 14;
inline constexpr uint32_t kScalarLogClamp = 15;
inline constexpr uint32_t kScalarLog = 16;
inline constexpr uint32_t kScalarRecipClamp = 17;
inline constexpr uint32_t kScalarRecipFixedFunc = 18;
inline constexpr uint32_t kScalarRecip = 19;
inline constexpr uint32_t kScalarRecipSqrtClamp = 20;
inline constexpr uint32_t kScalarRecipSqrtFixedFunc = 21;
inline constexpr uint32_t kScalarRecipSqrt = 22;
inline constexpr uint32_t kScalarMovA = 23;
inline constexpr uint32_t kScalarMovAFloor = 24;
inline constexpr uint32_t kScalarSub = 25;
inline constexpr uint32_t kScalarSubPrev = 26;
inline constexpr uint32_t kScalarPredSetEq = 27;
inline constexpr uint32_t kScalarPredSetNe = 28;
inline constexpr uint32_t kScalarPredSetGt = 29;
inline constexpr uint32_t kScalarPredSetGe = 30;
inline constexpr uint32_t kScalarPredSetInv = 31;
inline constexpr uint32_t kScalarPredSetPop = 32;
inline constexpr uint32_t kScalarPredSetClr = 33;
inline constexpr uint32_t kScalarPredSetRestore = 34;
inline constexpr uint32_t kScalarKillEq = 35;
inline constexpr uint32_t kScalarKillGt = 36;
inline constexpr uint32_t kScalarKillGe = 37;
inline constexpr uint32_t kScalarKillNe = 38;
inline constexpr uint32_t kScalarKillOne = 39;
inline constexpr uint32_t kScalarSqrt = 40;
// The *_CONST ops: _0 and _1 are one instruction; the opcode's bit 0 is bit 0 of the temporary's index.
inline constexpr uint32_t kScalarMulConst0 = 42;
inline constexpr uint32_t kScalarMulConst1 = 43;
inline constexpr uint32_t kScalarAddConst0 = 44;
inline constexpr uint32_t kScalarAddConst1 = 45;
inline constexpr uint32_t kScalarSubConst0 = 46;
inline constexpr uint32_t kScalarSubConst1 = 47;
inline constexpr uint32_t kScalarSin = 48;
inline constexpr uint32_t kScalarCos = 49;
inline constexpr uint32_t kScalarRetainPrev = 50;

// Export destinations (vector_dest with export set).
inline constexpr uint32_t kExportVsInterpolator0 = 0;
inline constexpr uint32_t kExportVsPosition = 62;
inline constexpr uint32_t kExportVsPointSize = 63;
inline constexpr uint32_t kExportPsColor0 = 0;
inline constexpr uint32_t kExportPsColor1 = 1;
inline constexpr uint32_t kExportPsDepth = 61;
inline constexpr uint32_t kExportAddress = 32;  // eA
inline constexpr uint32_t kExportData0 = 33;    // eM0 (eM0..eM4 = 33..37)

struct AluSrc {
  uint32_t reg = 0;
  bool temp = true;  // true: temporary register r<reg>; false: float constant c<reg>
  uint32_t swizzle = 0;  // component-relative; 0 = xyzw
  bool negate = false;
  bool relative = false;  // temporaries only: r[aL + reg] (register byte bit 6)
  bool abs = false;       // temporaries only: absolute value (register byte bit 7); constants use Alu::abs_constants
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
  bool vector_dest_relative = false;  // r[aL + vector_dest]
  bool scalar_dest_relative = false;  // r[aL + scalar_dest]
  bool abs_constants = false;
  bool vector_saturate = false;
  bool scalar_saturate = false;
  bool predicated = false;
  bool pred_condition = false;
};

inline uint32_t alu_reg_byte(const AluSrc& s) {
  if (!s.temp) return s.reg & 0xFF;
  return (s.reg & 0x3F) | ((s.relative ? 1u : 0u) << 6) | ((s.abs ? 1u : 0u) << 7);
}

inline std::array<uint32_t, 3> alu(const Alu& a) {
  const uint32_t w0 = (a.vector_dest & 0x3F) | ((a.vector_dest_relative ? 1u : 0u) << 6) |
                      ((a.abs_constants ? 1u : 0u) << 7) | ((a.scalar_dest & 0x3F) << 8) |
                      ((a.scalar_dest_relative ? 1u : 0u) << 14) | ((a.export_data ? 1u : 0u) << 15) |
                      ((a.vector_mask & 0xF) << 16) | ((a.scalar_mask & 0xF) << 20) |
                      ((a.vector_saturate ? 1u : 0u) << 24) | ((a.scalar_saturate ? 1u : 0u) << 25) |
                      ((a.scalar_op & 0x3F) << 26);
  const uint32_t w1 = (a.src3.swizzle & 0xFF) | ((a.src2.swizzle & 0xFF) << 8) | ((a.src1.swizzle & 0xFF) << 16) |
                      ((a.src3.negate ? 1u : 0u) << 24) | ((a.src2.negate ? 1u : 0u) << 25) |
                      ((a.src1.negate ? 1u : 0u) << 26) | ((a.pred_condition ? 1u : 0u) << 27) |
                      ((a.predicated ? 1u : 0u) << 28) | ((a.address_register_relative ? 1u : 0u) << 29) |
                      ((a.const1_relative ? 1u : 0u) << 30) | ((a.const0_relative ? 1u : 0u) << 31);
  const uint32_t w2 = alu_reg_byte(a.src3) | (alu_reg_byte(a.src2) << 8) | (alu_reg_byte(a.src1) << 16) |
                      ((a.vector_op & 0x1F) << 24) | ((a.src3.temp ? 1u : 0u) << 29) |
                      ((a.src2.temp ? 1u : 0u) << 30) | ((a.src1.temp ? 1u : 0u) << 31);
  return {w0, w1, w2};
}

// --- fetch (3 dwords) ---------------------------------------------------------------------------------------------

inline constexpr uint32_t kSwizzleXyzw = 0 | (1 << 3) | (2 << 6) | (3 << 9);  // destination swizzle, 3 bits each
inline constexpr uint32_t kFormat32_32_32_32Float = 38;
inline constexpr uint32_t kTextureDim1D = 0;
inline constexpr uint32_t kTextureDim2D = 1;
inline constexpr uint32_t kTextureDim3D = 2;
inline constexpr uint32_t kTextureDimCube = 3;

// Fetch opcodes.
inline constexpr uint32_t kFetchVertex = 0;
inline constexpr uint32_t kFetchTexture = 1;
inline constexpr uint32_t kFetchGetBorderColorFrac = 16;
inline constexpr uint32_t kFetchGetComputedLod = 17;
inline constexpr uint32_t kFetchGetGradients = 18;
inline constexpr uint32_t kFetchGetWeights = 19;
inline constexpr uint32_t kFetchSetLod = 24;
inline constexpr uint32_t kFetchSetGradientsH = 25;
inline constexpr uint32_t kFetchSetGradientsV = 26;

// Every vertex fetch field. A mini fetch (`mini`) carries only its own destination, format, offset and conversion
// fields; it takes the source, slot and stride from the last full fetch before it.
struct VFetch {
  uint32_t dst = 0;
  uint32_t src = 0;           // the index register
  uint32_t src_component = 0;  // 0..3 = x..w
  uint32_t slot = 0;          // 0..95 = const_index * 3 + const_index_select
  int32_t offset_dwords = 0;  // signed 23-bit
  uint32_t stride_dwords = 0;
  uint32_t format = kFormat32_32_32_32Float;
  uint32_t dst_swizzle = kSwizzleXyzw;
  bool is_signed = false;
  bool normalized = true;
  bool signed_rf_mode = false;
  bool rounded = false;
  int32_t exp_adjust = 0;  // signed 6-bit
  bool mini = false;
  uint32_t prefetch_count = 0;
  bool src_relative = false;
  bool dst_relative = false;
  bool predicated = false;
  bool pred_condition = false;
};

inline std::array<uint32_t, 3> vfetch(const VFetch& f) {
  const uint32_t w0 = kFetchVertex | ((f.src & 0x3F) << 5) | ((f.src_relative ? 1u : 0u) << 11) |
                      ((f.dst & 0x3F) << 12) | ((f.dst_relative ? 1u : 0u) << 18) | (1u << 19) |
                      (((f.slot / 3) & 0x1F) << 20) | (((f.slot % 3) & 3) << 25) | ((f.prefetch_count & 7) << 27) |
                      ((f.src_component & 3) << 30);
  const uint32_t w1 = (f.dst_swizzle & 0xFFF) | ((f.is_signed ? 1u : 0u) << 12) | ((f.normalized ? 0u : 1u) << 13) |
                      ((f.signed_rf_mode ? 1u : 0u) << 14) | ((f.rounded ? 1u : 0u) << 15) |
                      ((f.format & 0x3F) << 16) | ((uint32_t(f.exp_adjust) & 0x3F) << 24) |
                      ((f.mini ? 1u : 0u) << 30) | ((f.predicated ? 1u : 0u) << 31);
  const uint32_t w2 = (f.stride_dwords & 0xFF) | ((uint32_t(f.offset_dwords) & 0x7FFFFF) << 8) |
                      ((f.pred_condition ? 1u : 0u) << 31);
  return {w0, w1, w2};
}

// vfetch r<dst> from vertex fetch constant `slot` (0..95 = const_index * 3 + const_index_select).
inline std::array<uint32_t, 3> vfetch(uint32_t dst, uint32_t src, uint32_t slot, uint32_t offset_dwords,
                                      uint32_t stride_dwords, uint32_t format) {
  VFetch f;
  f.dst = dst;
  f.src = src;
  f.slot = slot;
  f.offset_dwords = int32_t(offset_dwords);
  f.stride_dwords = stride_dwords;
  f.format = format;
  return vfetch(f);
}

// Mini vfetch r<dst>: inherits source, slot and stride from the previous full vfetch.
inline std::array<uint32_t, 3> vfetch_mini(uint32_t dst, int32_t offset_dwords, uint32_t format) {
  VFetch f;
  f.dst = dst;
  f.offset_dwords = offset_dwords;
  f.format = format;
  f.mini = true;
  return vfetch(f);
}

// Every texture fetch field (tfetch and the other texture opcodes). Filters: 2 bits each (3 = use the fetch
// constant); aniso and arbitrary: 3 bits (7 = use the fetch constant). lod_bias is in 1/16 steps (signed 7-bit),
// the offsets in half texels (signed 5-bit).
struct TFetch {
  uint32_t opcode = kFetchTexture;
  uint32_t dst = 0;
  uint32_t src = 0;
  uint32_t const_index = 0;
  uint32_t dimension = kTextureDim2D;
  uint32_t src_swizzle = 0 | (1 << 2) | (2 << 4);  // 2 bits per component, absolute
  uint32_t dst_swizzle = kSwizzleXyzw;
  bool fetch_valid_only = false;
  bool unnormalized = false;
  uint32_t mag_filter = 3;
  uint32_t min_filter = 3;
  uint32_t mip_filter = 3;
  uint32_t aniso_filter = 7;
  uint32_t arbitrary_filter = 7;
  uint32_t vol_mag_filter = 3;
  uint32_t vol_min_filter = 3;
  bool use_computed_lod = true;
  bool use_register_lod = false;
  bool use_register_gradients = false;
  bool sample_location_center = false;
  int32_t lod_bias = 0;
  int32_t offset_x = 0;
  int32_t offset_y = 0;
  int32_t offset_z = 0;
  bool src_relative = false;
  bool dst_relative = false;
  bool predicated = false;
  bool pred_condition = false;
};

inline std::array<uint32_t, 3> tfetch(const TFetch& f) {
  const uint32_t w0 = (f.opcode & 0x1F) | ((f.src & 0x3F) << 5) | ((f.src_relative ? 1u : 0u) << 11) |
                      ((f.dst & 0x3F) << 12) | ((f.dst_relative ? 1u : 0u) << 18) |
                      ((f.fetch_valid_only ? 1u : 0u) << 19) | ((f.const_index & 0x1F) << 20) |
                      ((f.unnormalized ? 1u : 0u) << 25) | ((f.src_swizzle & 0x3F) << 26);
  const uint32_t w1 = (f.dst_swizzle & 0xFFF) | ((f.mag_filter & 3) << 12) | ((f.min_filter & 3) << 14) |
                      ((f.mip_filter & 3) << 16) | ((f.aniso_filter & 7) << 18) |
                      ((f.arbitrary_filter & 7) << 21) | ((f.vol_mag_filter & 3) << 24) |
                      ((f.vol_min_filter & 3) << 26) | ((f.use_computed_lod ? 1u : 0u) << 28) |
                      ((f.use_register_lod ? 1u : 0u) << 29) | ((f.predicated ? 1u : 0u) << 31);
  const uint32_t w2 = (f.use_register_gradients ? 1u : 0u) | ((f.sample_location_center ? 1u : 0u) << 1) |
                      ((uint32_t(f.lod_bias) & 0x7F) << 2) | ((f.dimension & 3) << 14) |
                      ((uint32_t(f.offset_x) & 0x1F) << 16) | ((uint32_t(f.offset_y) & 0x1F) << 21) |
                      ((uint32_t(f.offset_z) & 0x1F) << 26) | ((f.pred_condition ? 1u : 0u) << 31);
  return {w0, w1, w2};
}

// tfetch r<dst> = texture fetch constant `const_index` sampled at r<src> (source swizzle: 2 bits per component).
// Matches the Task 13 encoding: every other field is 0 (filters 0, implicit LOD bit clear).
inline std::array<uint32_t, 3> tfetch(uint32_t dst, uint32_t src, uint32_t const_index, uint32_t dimension,
                                      uint32_t src_swizzle = 0 | (1 << 2) | (2 << 4)) {
  const uint32_t w0 = kFetchTexture | ((src & 0x3F) << 5) | ((dst & 0x3F) << 12) | ((const_index & 0x1F) << 20) |
                      ((src_swizzle & 0x3F) << 26);
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
