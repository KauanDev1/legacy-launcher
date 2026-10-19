// SPDX-License-Identifier: MIT
// Xenos GPU register file: the 0x6000 dword registers the guest programs through PM4 packets. The command
// processor writes them, the draw, EDRAM and texture paths read them by name. Pure GPU core: no Vulkan types.
#pragma once

#include <array>
#include <cstdint>

namespace lr::gpu {

inline constexpr uint32_t kRegisterCount = 0x6000;

// Shader constant ranges (dword register indices). Later tasks index them by number.
inline constexpr uint32_t kShaderConstantFloatBase = 0x4000;  // vec4 i at base + 4 * i (X, Y, Z, W)
inline constexpr uint32_t kShaderConstantFloatVec4Count = 512;
inline constexpr uint32_t kShaderConstantFloatCount = kShaderConstantFloatVec4Count * 4;
inline constexpr uint32_t kShaderConstantFetchBase = 0x4800;  // fetch constant i at base + 6 * i
inline constexpr uint32_t kShaderConstantFetchRegsPerConstant = 6;
inline constexpr uint32_t kShaderConstantFetchCount = 32;
inline constexpr uint32_t kShaderConstantBoolBase = 0x4900;  // 8 registers, 32 bools each (256 bools)
inline constexpr uint32_t kShaderConstantBoolRegCount = 8;
inline constexpr uint32_t kShaderConstantLoopBase = 0x4908;  // 32 loop constants
inline constexpr uint32_t kShaderConstantLoopCount = 32;

// Named registers: X(name, index). Sources per group:
//  - CP ring buffer: AMD R5xx/Xenos register map; second source: the Linux kernel's radeon_reg.h byte offsets / 4
//    (RADEON_CP_RB_BASE 0x0700 / 4 = 0x1C0, CP_RB_CNTL 0x0704 / 4 = 0x1C1, CP_RB_RPTR_ADDR 0x070C / 4 = 0x1C3,
//    CP_RB_WPTR 0x0714 / 4 = 0x1C5). Not present in Xenia's register_table.inc.
//  - CP scratch registers: AMD R5xx/Xenos map (radeon_reg.h SCRATCH_REG0 0x15E0 / 4 = 0x578, SCRATCH_UMSK
//    0x0770 / 4, SCRATCH_ADDR 0x0774 / 4); cross-checked against Xenia register_table.inc, which names
//    SCRATCH_REG4/5 (0x57C/0x57D) CALLBACK_ADDRESS/CALLBACK_CONTEXT after how the Xbox 360 kernel uses them.
//  - RB, PA, SQ, VGT context registers, COHER_STATUS_HOST, the viz-query status pair and the shader constant
//    ranges: AMD R6xx/Xenos register map; cross-checked against Xenia register_table.inc (indices only; they are
//    hardware facts).
// The lookup table in registers.cpp sorts them at compile time, so the order here is free.
#define LR_GPU_REGISTERS(X)                                                                                  \
  /* Command processor */                                                                                    \
  X(CP_RB_BASE, 0x01C0) X(CP_RB_CNTL, 0x01C1) X(CP_RB_RPTR_ADDR, 0x01C3) X(CP_RB_WPTR, 0x01C5)               \
  X(SCRATCH_UMSK, 0x01DC) X(SCRATCH_ADDR, 0x01DD)                                                            \
  X(SCRATCH_REG0, 0x0578) X(SCRATCH_REG1, 0x0579) X(SCRATCH_REG2, 0x057A) X(SCRATCH_REG3, 0x057B)            \
  X(SCRATCH_REG4, 0x057C) X(SCRATCH_REG5, 0x057D) X(SCRATCH_REG6, 0x057E) X(SCRATCH_REG7, 0x057F)            \
  X(COHER_STATUS_HOST, 0x0A31)                                                                               \
  X(PA_SC_VIZ_QUERY_STATUS_0, 0x0C44) X(PA_SC_VIZ_QUERY_STATUS_1, 0x0C45)                                    \
  /* Render backend (EDRAM surfaces, copy, blend and depth state) */                                         \
  X(RB_SURFACE_INFO, 0x2000) X(RB_COLOR_INFO, 0x2001) X(RB_DEPTH_INFO, 0x2002) X(RB_COLOR1_INFO, 0x2003)      \
  X(RB_COLOR2_INFO, 0x2004) X(RB_COLOR3_INFO, 0x2005)                                                        \
  X(RB_COPY_CONTROL, 0x2318) X(RB_COPY_DEST_BASE, 0x2319) X(RB_COPY_DEST_PITCH, 0x231A)                      \
  X(RB_COPY_DEST_INFO, 0x231B)                                                                               \
  X(RB_DEPTHCONTROL, 0x2200) X(RB_BLENDCONTROL0, 0x2201) X(RB_COLORCONTROL, 0x2202)                          \
  X(RB_BLENDCONTROL1, 0x2209) X(RB_BLENDCONTROL2, 0x220A) X(RB_BLENDCONTROL3, 0x220B)                        \
  X(RB_MODECONTROL, 0x2208)                                                                                  \
  /* Primitive assembly: window, scissor, viewport, rasteriser mode */                                       \
  X(PA_SC_WINDOW_OFFSET, 0x2080) X(PA_SC_WINDOW_SCISSOR_TL, 0x2081) X(PA_SC_WINDOW_SCISSOR_BR, 0x2082)       \
  X(PA_CL_VPORT_XSCALE, 0x210F) X(PA_CL_VPORT_XOFFSET, 0x2110) X(PA_CL_VPORT_YSCALE, 0x2111)                 \
  X(PA_CL_VPORT_YOFFSET, 0x2112) X(PA_CL_VPORT_ZSCALE, 0x2113) X(PA_CL_VPORT_ZOFFSET, 0x2114)                \
  X(PA_SU_SC_MODE_CNTL, 0x2205) X(PA_SC_VIZ_QUERY, 0x2293)                                                   \
  /* Sequencer and vertex grouper */                                                                         \
  X(SQ_PROGRAM_CNTL, 0x2180) X(VGT_EVENT_INITIATOR, 0x21F9) X(VGT_DMA_BASE, 0x21FA) X(VGT_DMA_SIZE, 0x21FB)    \
  X(VGT_DRAW_INITIATOR, 0x21FC)                                                                              \
  /* Shader constants (first and last register of each range) */                                             \
  X(SHADER_CONSTANT_000_X, 0x4000) X(SHADER_CONSTANT_511_W, 0x47FF)                                          \
  X(SHADER_CONSTANT_FETCH_00_0, 0x4800) X(SHADER_CONSTANT_FETCH_31_5, 0x48BF)                                \
  X(SHADER_CONSTANT_BOOL_000_031, 0x4900) X(SHADER_CONSTANT_BOOL_224_255, 0x4907)                            \
  X(SHADER_CONSTANT_LOOP_00, 0x4908) X(SHADER_CONSTANT_LOOP_31, 0x4927)

enum Reg : uint32_t {
#define LR_GPU_REG_ENUM(name, index) name = index,
  LR_GPU_REGISTERS(LR_GPU_REG_ENUM)
#undef LR_GPU_REG_ENUM
};

// Thread-safety: owned by the GPU thread only.
class RegisterFile {
 public:
  // Out-of-range reads return 0.
  uint32_t read(uint32_t index) const { return index < kRegisterCount ? regs_[index] : 0; }
  // Out-of-range writes are ignored.
  void write(uint32_t index, uint32_t value) {
    if (index < kRegisterCount) regs_[index] = value;
  }
  // The stored dword reinterpreted as an IEEE float (shader constants, viewport scale/offset).
  float read_float(uint32_t index) const;
  // Name of a registered index (exact match only), nullptr if unnamed. No allocation.
  static const char* name(uint32_t index);

 private:
  std::array<uint32_t, kRegisterCount> regs_{};
};

}  // namespace lr::gpu
