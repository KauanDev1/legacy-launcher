// SPDX-License-Identifier: MIT
// The decoded form of a Xenos shader microcode (plan B translator, ADR 0005 M8): the control-flow program, the
// ALU and fetch instructions its exec clauses run, and a summary of what the shader uses. decode() is the only way
// guest microcode enters the translator, so everything here has been bounds-checked: every address, count and index
// in a Program is in range. CPU-only, no Vulkan types.
#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

#include "common/result.h"
#include "runtime/gpu/backend.h"
#include "runtime/gpu/shader/contract.h"

namespace lr::gpu::shader {

// --- control flow -------------------------------------------------------------------------------------------------

// Numbered as the 4-bit CF opcode.
enum class CfOp : uint8_t {
  Nop,
  Exec,
  ExecEnd,
  CondExec,
  CondExecEnd,
  CondPredExec,
  CondPredExecEnd,
  LoopStart,
  LoopEnd,
  CondCall,
  Return,
  CondJmp,
  Alloc,
  CondExecPredClean,
  CondExecPredCleanEnd,
  MarkVsFetchDone,
};

// One CF instruction. Fields that a given op does not have are 0/false.
struct CfInstr {
  CfOp op = CfOp::Nop;
  uint32_t address = 0;   // exec family: first ALU/fetch slot (3-dword units from the microcode start); others: raw
  uint32_t count = 0;     // exec family: instructions in the clause; alloc: the size field
  uint32_t sequence = 0;  // exec family: 2 bits per instruction, bit 0 = fetch, bit 1 = serialize
  uint32_t bool_index = 0;  // cond_exec / cond_exec_pred_clean / non-predicated cond_call, cond_jmp: bool constant
  uint32_t loop_id = 0;     // loop_start / loop_end: loop constant
  uint32_t target = 0;      // loop_start: CF index to skip to; loop_end: CF index of the body; call/jmp: CF index
  bool condition = false;   // cond_exec*, cond_call, cond_jmp: the value the bool (or predicate) must have
  bool use_predicate = false;      // cond_exec_pred*, predicated cond_call / cond_jmp
  bool is_unconditional = false;   // cond_call / cond_jmp
  bool is_repeat = false;          // loop_start: keep the current aL
  bool is_predicated_break = false;   // loop_end: leave the loop when the predicate == pred_break_condition
  bool pred_break_condition = false;
  uint32_t alloc_type = 0;  // alloc: 0 none, 1 position, 2 interpolators / colors, 3 memory export
};

bool is_exec(CfOp op);      // exec, exec_end, cond_exec*, cond_exec_pred*, cond_exec_pred_clean*
bool is_exec_end(CfOp op);  // the exec forms that end the shader after the clause

// --- ALU ----------------------------------------------------------------------------------------------------------

// Numbered as the 5-bit vector opcode (30 and 31 do not exist).
enum class VectorOp : uint8_t {
  Add,
  Mul,
  Max,
  Min,
  SetEq,
  SetGt,
  SetGe,
  SetNe,
  Frac,
  Trunc,
  Floor,
  MulAdd,
  CndEq,
  CndGe,
  CndGt,
  Dot4,
  Dot3,
  Dot2Add,
  Cube,
  Max4,
  PredSetEqPush,
  PredSetNePush,
  PredSetGtPush,
  PredSetGePush,
  KillEq,
  KillGt,
  KillGe,
  KillNe,
  Dst,
  MaxA,
};

// Numbered as the 6-bit scalar opcode (41 and 51..63 do not exist).
enum class ScalarOp : uint8_t {
  Add,
  AddPrev,
  Mul,
  MulPrev,
  MulPrev2,
  Max,
  Min,
  SetEq,
  SetGt,
  SetGe,
  SetNe,
  Frac,
  Trunc,
  Floor,
  Exp,
  LogClamp,
  Log,
  RecipClamp,
  RecipFixedFunc,
  Recip,
  RecipSqrtClamp,
  RecipSqrtFixedFunc,
  RecipSqrt,
  MovA,
  MovAFloor,
  Sub,
  SubPrev,
  PredSetEq,
  PredSetNe,
  PredSetGt,
  PredSetGe,
  PredSetInv,
  PredSetPop,
  PredSetClr,
  PredSetRestore,
  KillEq,
  KillGt,
  KillGe,
  KillNe,
  KillOne,
  Sqrt,
  Invalid41,  // placeholder that keeps the numbering; never produced by decode()
  MulConst0,
  MulConst1,
  AddConst0,
  AddConst1,
  SubConst0,
  SubConst1,
  Sin,
  Cos,
  RetainPrev,
};

// One ALU source operand (src1..src3 of the instruction word).
struct AluSource {
  uint32_t reg = 0;       // temporary 0..63 or float constant 0..255
  bool is_temp = false;   // the per-source select bit
  uint32_t swizzle = 0;   // raw 8 bits; component i reads ((swizzle >> 2i) + i) & 3, so 0 is xyzw
  bool negate = false;
  bool abs = false;       // temporaries: register byte bit 7; constants: the instruction's abs_constants
  bool const_relative = false;  // constants only: index + aL (or + a0 with a0_rel), per the const_0/const_1 rule
};

// One ALU instruction: a vector op and a scalar op issued together.
//
// Destinations: without export_data, the vector result goes to r<vector_dest> (r[aL + vector_dest] when
// vector_dest_rel) under vector_write_mask, and the scalar result to r<scalar_dest> (likewise) under
// scalar_write_mask. With export_data, everything goes to export register vector_dest (scalar_dest is unused, and
// exports are never relative: decode() rejects vector_dest_rel), and the raw masks combine as resolved in the
// export_*_mask fields (semantics per Xenia's constant-0/1 export masks, reference only), with vm/sm the raw
// vector/scalar write masks:
//   - vm & ~sm: the vector result (export_vector_mask);
//   - sm & ~vm: the scalar result (export_scalar_mask);
//   - vm & sm: the constant 1.0 (export_const1_mask);
//   - ~(vm | sm), only when scalar_dest_rel is set: the constant 0.0 (export_const0_mask); scalar_dest_rel means
//     nothing else on an export.
// The export is written (and counted in the Program summary) when any of the four is nonzero. The four masks are 0
// without export_data.
//
// *_CONST_0/1 scalar ops: the operands are the constant src[2] (component a) and the temporary scalar_const_temp
// (component b, from src[2]'s swizzle); both take src[2].negate and abs_constants (the temporary has no abs bit of
// its own; src[2].abs already holds abs_constants).
//
// Relative constants: src[i].const_relative is already resolved (the first constant operand of the instruction uses
// const0_rel, a later one const1_rel); the raw bits are kept for the disassembler.
struct AluInstr {
  VectorOp vector_op = VectorOp::Add;
  ScalarOp scalar_op = ScalarOp::RetainPrev;
  uint32_t vector_dest = 0;
  uint32_t scalar_dest = 0;
  uint32_t vector_write_mask = 0;  // bit i = component i
  uint32_t scalar_write_mask = 0;
  bool vector_dest_rel = false;  // aL-relative temporary destination
  bool scalar_dest_rel = false;
  bool export_data = false;
  bool vector_saturate = false;
  bool scalar_saturate = false;
  bool abs_constants = false;
  bool predicated = false;
  bool pred_condition = false;
  bool const0_rel = false;
  bool const1_rel = false;
  bool a0_rel = false;  // relative constants add a0 instead of aL
  std::array<AluSource, 3> src{};                   // src1, src2, src3
  std::array<bool, 3> is_src_temp_relative{};       // temporaries: r[aL + reg] (register byte bit 6)
  // *_CONST_0/1 scalar ops: the temporary operand (its index is spread over the scalar opcode bit 0, src3's select
  // bit and src3's swizzle); the constant operand is src[2].
  uint32_t scalar_const_temp = 0;
  // Resolved export component masks (export_data only; see above).
  uint32_t export_vector_mask = 0;
  uint32_t export_scalar_mask = 0;
  uint32_t export_const0_mask = 0;
  uint32_t export_const1_mask = 0;
};

// Sources the vector op reads (1, 2 or 3: src1, src1..2, src1..3).
uint32_t vector_op_source_count(VectorOp op);
// Whether the vector op does something even with a zero write mask (predicate push, kill, MaxA writes a0).
bool vector_op_has_side_effect(VectorOp op);
// Scalar operands read from src3: 0 (RetainPrev, PredSetClr), 1 (component a = w-ish), 2 (a and b). The CONST ops
// count 2 (the constant src3 and scalar_const_temp).
uint32_t scalar_op_operand_count(ScalarOp op);
bool scalar_op_is_const(ScalarOp op);  // MulConst0 .. SubConst1
bool scalar_op_has_side_effect(ScalarOp op);  // predicate sets, kills, MovA*
bool is_kill(VectorOp op);
bool is_kill(ScalarOp op);

// --- fetch --------------------------------------------------------------------------------------------------------

enum class FetchOp : uint8_t {
  VertexFetch,              // 0
  TextureFetch,             // 1
  GetTextureBorderColorFrac,  // 16
  GetTextureComputedLod,    // 17
  GetTextureGradients,      // 18
  GetTextureWeights,        // 19
  SetTextureLod,            // 24
  SetTextureGradientsHorz,  // 25
  SetTextureGradientsVert,  // 26
};

// Destination swizzle selectors (3 bits per component, absolute): 0..3 = x..w, 4 = 0.0, 5 = 1.0, 7 = keep.
inline constexpr uint32_t kFetchSwizzleZero = 4;
inline constexpr uint32_t kFetchSwizzleOne = 5;
inline constexpr uint32_t kFetchSwizzleKeep = 7;

// One fetch instruction. Vertex fields are 0 for texture ops and vice versa.
//
// A mini vertex fetch has only its own destination, format, offset and conversion fields; decode() fills its
// src_reg, src_rel, index_component, slot, stride and rounded from the last full vertex fetch before it (in CF
// order), so a translator treats both alike.
struct FetchInstr {
  FetchOp op = FetchOp::VertexFetch;
  uint32_t src_reg = 0;   // vertex: the index register; texture: the coordinate register
  uint32_t dst_reg = 0;
  bool src_rel = false;   // aL-relative registers
  bool dst_rel = false;
  uint32_t dst_swizzle = 0;  // raw 12 bits, see kFetchSwizzle*
  uint32_t src_swizzle = 0;  // vertex: 2 bits (= index_component); texture: 6 bits, 2 per coordinate, absolute
  bool predicated = false;
  bool pred_condition = false;

  // vertex
  uint32_t slot = 0;          // vertex fetch constant 0..95 (const_index * 3 + select)
  int32_t offset = 0;         // dwords, signed 23-bit: -4194304..4194303
  uint32_t stride = 0;        // dwords
  uint32_t format = 0;        // 6-bit vertex data format
  bool is_signed = false;
  bool normalized = false;    // num_format_all == 0
  bool signed_rf_mode = false;
  int32_t exp_adjust = 0;     // signed 6-bit
  bool mini = false;
  uint32_t index_component = 0;  // 0..3 = x..w of src_reg
  bool rounded = false;       // index rounded instead of truncated
  uint32_t prefetch_count = 0;  // the field is "count - 1"

  // texture
  uint32_t const_index = 0;   // texture fetch constant 0..31
  uint32_t dimension = 0;     // 0 1D, 1 2D, 2 3D / stacked, 3 cube
  bool fetch_valid_only = false;
  bool unnormalized = false;  // tx_coord_denorm
  uint32_t mag_filter = 0;    // 2 bits, 3 = from the fetch constant
  uint32_t min_filter = 0;
  uint32_t mip_filter = 0;
  uint32_t aniso_filter = 0;  // 3 bits, 7 = from the fetch constant
  uint32_t arbitrary_filter = 0;  // 3 bits, 7 = from the fetch constant
  uint32_t vol_mag_filter = 0;
  uint32_t vol_min_filter = 0;
  bool use_computed_lod = false;
  bool use_register_lod = false;
  bool use_register_gradients = false;
  bool sample_location_center = false;
  float lod_bias = 0.0f;  // signed 7-bit field / 16
  float offset_x = 0.0f;  // signed 5-bit field / 2 (texels)
  float offset_y = 0.0f;
  float offset_z = 0.0f;
};

// Whether the fetch writes dst_reg (any component not kept; the Set* texture ops write nothing).
bool fetch_writes_dest(const FetchInstr& f);

// --- program ------------------------------------------------------------------------------------------------------

// Export register numbers (ALU vector_dest with export_data): kExport* in contract.h.

inline constexpr uint32_t kMaxTemps = 64;  // the register array size when any temporary is aL-relative

struct Program {
  ShaderStage stage = ShaderStage::Vertex;
  std::vector<CfInstr> cf;  // two per 3-dword pair, from the start of the microcode up to the first exec's slots
  // Indexed by slot address (3-dword units from the microcode start): every slot some exec runs is decoded here, as
  // that exec's sequence bits say. Slots no exec runs (the CF area included) are a default AluInstr and unused.
  std::vector<std::variant<AluInstr, FetchInstr>> slots;
  // Temporaries: highest absolute register used + 1; kMaxTemps when any register is aL-relative.
  uint32_t temp_count = 0;
  std::bitset<256> bools_used;
  std::bitset<32> loops_used;
  std::bitset<96> vfetch_slots;
  std::bitset<32> tfetch_slots;
  uint32_t interpolators_written = 0;  // VS: bit i = export i (0..15)
  uint32_t colors_written = 0;         // PS: bit i = color i (0..3)
  uint32_t memexport_written = 0;      // bit 0 = eA, bits 1..5 = eM0..eM4
  bool writes_position = false;
  bool writes_point_size = false;
  bool writes_depth = false;
  bool uses_memexport = false;
  bool uses_kill = false;
  bool uses_relative_temps = false;
  bool has_jumps_calls_or_loops = false;
};

// Decodes `microcode` (host-order dwords: the caller byte-swaps from guest big-endian). Every malformed or
// out-of-range input is an InvalidFormat error "instruction <n> (<kind>): <what>", where <n> is the CF index for
// control flow and the slot address for ALU/fetch instructions.
Result<Program> decode(ShaderStage stage, std::span<const uint32_t> microcode);

}  // namespace lr::gpu::shader
