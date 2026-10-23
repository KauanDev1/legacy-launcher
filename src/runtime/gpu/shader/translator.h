// SPDX-License-Identifier: MIT
// The plan B shader translator (ADR 0005 M8): a decoded Xenos Program -> a SPIR-V 1.5 module for Vulkan 1.2 that
// follows the runtime contract of contract.h (push constants, shared block, descriptor sets, spec constant 0) and
// links VS -> PS interpolators by register index. Clean-room MIT code; Xenia is a reference for semantics only.
//
// Microcode the translator cannot handle is an InvalidFormat error "instruction <n> (<kind>): <what> is not
// supported", with <n> the CF index for control flow and the slot address for ALU/fetch, so the caller can name it
// (fatal with a backend that binds shaders, exit 5). The translator never aborts: its own bugs are errors too.
// CPU-only, no Vulkan types.
#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <vector>

#include "common/result.h"
#include "runtime/gpu/shader/ir.h"

namespace lr::gpu::shader {

// Bumped whenever the translated SPIR-V changes for the same input (the disk cache is keyed by it).
inline constexpr uint32_t kTranslatorVersion = 7;
inline constexpr uint32_t kNoParamGen = 0xFF;

// A shader with jumps, calls or loops runs as a loop around a switch on its pc (translator_cf.cpp). Only taken
// backward branches count (LOOP_END back-edges, backward jmp/call, a return to an earlier instruction, a skip to an
// earlier one): when the count reaches this cap the branch is not followed and the shader ends with its outputs as
// written so far, so guest microcode can never hang the GPU. Three nested loops of 255 iterations fit (255^3 <
// 2^24); four, or three with a call in the innermost body (each return counts), are cut short.
inline constexpr uint32_t kCfBackwardBranchCap = 1u << 24;

// The PS key modifiers, decoded (key_options(const ShaderKey&)). The VS has none in M3.
struct TranslateOptions {
  uint32_t ps_param_gen = kNoParamGen;  // register that receives the param_gen value, or kNoParamGen
  bool ps_point = false;                // the draw is a point list (param_gen's point flag and point coordinate)
  uint16_t flat_mask = 0;               // bit r: interpolator r is flat-shaded (SQ_INTERPOLATOR_CNTL.param_shade)
  uint16_t centroid_mask = 0;           // bit r: interpolator r is sampled at the centroid (sampling_pattern)
};

// What the runtime needs to know about a translated shader.
struct ShaderInfo {
  std::bitset<96> vfetch_slots;          // vertex fetch constants the shader reads
  std::bitset<32> tfetch_slots;          // texture fetch constants the shader samples
  std::array<uint8_t, 32> tfetch_dims{};  // per texture fetch constant: the fetch dimension (0 1D, 1 2D, 2 3D, 3 cube)
  uint32_t interpolators_read = 0;       // PS: bit r = input declared at location r
  uint32_t colors_written = 0;           // PS: bit i = oC<i>
  bool writes_depth = false;             // PS
  bool uses_kill = false;                // PS
  bool drops_point_size = false;         // VS: export 63 (point size / edge flag / kill vertex) was ignored
};

struct TranslatedShader {
  std::vector<uint32_t> spirv;
  ShaderInfo info;
};

Result<TranslatedShader> translate(const Program& program, const TranslateOptions& options);

}  // namespace lr::gpu::shader
