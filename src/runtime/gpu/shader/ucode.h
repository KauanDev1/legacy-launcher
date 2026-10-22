// SPDX-License-Identifier: MIT
// Xenos shader microcode bit positions: the raw encoding the decoder (ir.cpp) reads. Encoding per Xenia ucode.h
// (reference only): field positions were taken from the public field tables and written here in our own form; no code
// or structure copied.
//
// A shader is a run of 3-dword "slots" in host order (the guest stores them big-endian; the caller swaps):
// - the control-flow (CF) area comes first, two 48-bit CF instructions per slot: instruction 2k is bits 0..47 of
//   (dword 0 | dword 1 low half), instruction 2k+1 is bits 0..47 of (dword 1 high half | dword 2);
// - ALU and fetch instructions follow, one per slot, run by the CF exec clauses.
#pragma once

#include <cstdint>

namespace lr::gpu::shader::ucode {

// A bitfield of a 32-bit dword (ALU, fetch) or of the 48-bit CF word.
struct Field {
  uint8_t shift;
  uint8_t width;
};

constexpr uint64_t get(uint64_t value, Field f) { return (value >> f.shift) & ((uint64_t{1} << f.width) - 1); }
constexpr uint32_t get32(uint32_t value, Field f) { return static_cast<uint32_t>(get(value, f)); }
// Sign-extends a `width`-bit two's complement field.
constexpr int32_t sign_extend(uint32_t raw, uint8_t width) {
  const uint32_t sign = uint32_t{1} << (width - 1);
  return static_cast<int32_t>(raw ^ sign) - static_cast<int32_t>(sign);
}

// The two CF instructions of the slot at `w` (3 dwords).
constexpr uint64_t cf_lo(const uint32_t* w) { return uint64_t{w[0]} | (uint64_t{w[1] & 0xFFFF} << 32); }
constexpr uint64_t cf_hi(const uint32_t* w) { return uint64_t{w[1] >> 16} | (uint64_t{w[2]} << 16); }

inline constexpr uint32_t kSlotDwords = 3;

// --- control flow (48-bit word) -----------------------------------------------------------------------------------
namespace cf {
inline constexpr Field kOpcode{44, 4};
// exec family (exec, cond_exec, cond_exec_pred, cond_exec_pred_clean)
inline constexpr Field kExecAddress{0, 12};
inline constexpr Field kExecCount{12, 3};
inline constexpr Field kExecSequence{16, 12};  // 2 bits per instruction: bit 0 fetch, bit 1 serialize
inline constexpr Field kBoolAddress{34, 8};    // cond_exec, cond_exec_pred_clean, cond_call, cond_jmp
inline constexpr Field kCondition{42, 1};      // every conditional form; loop_end: the predicated break condition
// loop_start / loop_end / cond_call / cond_jmp
inline constexpr Field kTargetAddress{0, 13};
inline constexpr Field kLoopRepeat{13, 1};     // loop_start
inline constexpr Field kLoopId{16, 5};
inline constexpr Field kLoopPredBreak{21, 1};  // loop_end
inline constexpr Field kBranchUnconditional{13, 1};  // cond_call / cond_jmp
inline constexpr Field kBranchPredicated{14, 1};
// alloc
inline constexpr Field kAllocSize{0, 3};
inline constexpr Field kAllocType{41, 2};
}  // namespace cf

// --- ALU (3 dwords) -----------------------------------------------------------------------------------------------
namespace alu {
// dword 0
inline constexpr Field kVectorDest{0, 6};
inline constexpr Field kVectorDestRel{6, 1};
inline constexpr Field kAbsConstants{7, 1};
inline constexpr Field kScalarDest{8, 6};
inline constexpr Field kScalarDestRel{14, 1};
inline constexpr Field kExportData{15, 1};
inline constexpr Field kVectorWriteMask{16, 4};
inline constexpr Field kScalarWriteMask{20, 4};
inline constexpr Field kVectorSaturate{24, 1};
inline constexpr Field kScalarSaturate{25, 1};
inline constexpr Field kScalarOpcode{26, 6};
// dword 1: per-source swizzle/negate, source i (0 = src1) at these positions
inline constexpr Field kSwizzle[3] = {{16, 8}, {8, 8}, {0, 8}};
inline constexpr Field kNegate[3] = {{26, 1}, {25, 1}, {24, 1}};
inline constexpr Field kPredCondition{27, 1};
inline constexpr Field kPredicated{28, 1};
inline constexpr Field kA0Relative{29, 1};
inline constexpr Field kConst1Rel{30, 1};
inline constexpr Field kConst0Rel{31, 1};
// dword 2
inline constexpr Field kSrcReg[3] = {{16, 8}, {8, 8}, {0, 8}};
inline constexpr Field kVectorOpcode{24, 5};
inline constexpr Field kSrcIsTemp[3] = {{31, 1}, {30, 1}, {29, 1}};
// The register byte of a temporary source.
inline constexpr Field kTempIndex{0, 6};
inline constexpr Field kTempRelative{6, 1};
inline constexpr Field kTempAbs{7, 1};
}  // namespace alu

// --- fetch (3 dwords) ---------------------------------------------------------------------------------------------
namespace fetch {
// dword 0, both kinds
inline constexpr Field kOpcode{0, 5};
inline constexpr Field kSrcReg{5, 6};
inline constexpr Field kSrcRel{11, 1};
inline constexpr Field kDstReg{12, 6};
inline constexpr Field kDstRel{18, 1};
inline constexpr Field kConstIndex{20, 5};
// dword 1 / 2, both kinds
inline constexpr Field kDstSwizzle{0, 12};  // dword 1
inline constexpr Field kPredicated{31, 1};  // dword 1
inline constexpr Field kPredCondition{31, 1};  // dword 2

// vertex fetch
inline constexpr Field kVfConstSelect{25, 2};   // dword 0: slot = const_index * 3 + select
inline constexpr Field kVfPrefetchCount{27, 3};  // dword 0
inline constexpr Field kVfSrcSwizzle{30, 2};    // dword 0: the index component
inline constexpr Field kVfSigned{12, 1};        // dword 1
inline constexpr Field kVfNumFormat{13, 1};     // dword 1: 0 = normalized
inline constexpr Field kVfSignedRfMode{14, 1};  // dword 1
inline constexpr Field kVfRounded{15, 1};       // dword 1
inline constexpr Field kVfFormat{16, 6};        // dword 1
inline constexpr Field kVfExpAdjust{24, 6};     // dword 1, signed
inline constexpr Field kVfMini{30, 1};          // dword 1
inline constexpr Field kVfStride{0, 8};         // dword 2
inline constexpr Field kVfOffset{8, 23};        // dword 2, signed

// texture fetch
inline constexpr Field kTfFetchValidOnly{19, 1};  // dword 0
inline constexpr Field kTfUnnormalized{25, 1};    // dword 0
inline constexpr Field kTfSrcSwizzle{26, 6};      // dword 0
inline constexpr Field kTfMagFilter{12, 2};       // dword 1
inline constexpr Field kTfMinFilter{14, 2};
inline constexpr Field kTfMipFilter{16, 2};
inline constexpr Field kTfAnisoFilter{18, 3};
inline constexpr Field kTfVolMagFilter{24, 2};
inline constexpr Field kTfVolMinFilter{26, 2};
inline constexpr Field kTfUseComputedLod{28, 1};
inline constexpr Field kTfUseRegisterLod{29, 1};
inline constexpr Field kTfUseRegisterGradients{0, 1};  // dword 2
inline constexpr Field kTfSampleLocation{1, 1};
inline constexpr Field kTfLodBias{2, 7};   // signed, 1/16 steps
inline constexpr Field kTfDimension{14, 2};
inline constexpr Field kTfOffsetX{16, 5};  // signed, half texels
inline constexpr Field kTfOffsetY{21, 5};
inline constexpr Field kTfOffsetZ{26, 5};
}  // namespace fetch

}  // namespace lr::gpu::shader::ucode
