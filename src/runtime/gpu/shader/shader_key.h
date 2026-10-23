// SPDX-License-Identifier: MIT
// What identifies one translated shader (plan B, ADR 0005 M8): the stage, the XXH3-64 of the microcode the game
// loaded (IM_LOAD / IM_LOAD_IMMEDIATE), its size and the register state the translation depends on (the modifiers).
// The runtime translates each distinct ShaderKey once (runtime/gpu/shader_cache.h) and keeps the result in memory
// and in the disk cache (disk_cache.h, file name cache_file_name()). CPU-only, no Vulkan types.
#pragma once

#include <compare>
#include <cstdint>
#include <string>

#include "runtime/gpu/backend.h"
#include "runtime/gpu/registers.h"

namespace lr::gpu::shader {

struct TranslateOptions;  // translator.h
struct TranslatedShader;  // translator.h

struct ShaderKey {
  ShaderStage stage = ShaderStage::Vertex;
  uint64_t microcode_hash = 0;
  uint32_t dwords = 0;
  uint64_t modifiers = 0;
  auto operator<=>(const ShaderKey&) const = default;
};

// XXH3-64 (seed 0) of the `dwords` microcode dwords at `guest_microcode_be`, read as the bytes they are in guest
// memory (big-endian, no swap).
uint64_t microcode_hash(const uint8_t* guest_microcode_be, uint32_t dwords);

// Pixel-shader modifier layout (the vertex shader has none in M3: always 0).
inline constexpr uint64_t kModParamGenMask = 0xFF;       // bits 0-7: the param_gen register, or 0xFF for none
inline constexpr uint64_t kModPointBit = 1ull << 8;      // bit 8: point-list draw (only with a param_gen register)
inline constexpr uint32_t kModFlatShift = 16;            // bits 16-31: flat-shade mask (SQ_INTERPOLATOR_CNTL 0-15)
inline constexpr uint32_t kModCentroidShift = 32;        // bits 32-47: centroid mask (see key_modifiers)
inline constexpr uint32_t kPrimPointList = 1;            // VGT_DRAW_INITIATOR.prim_type of a point list

// The modifiers of `stage` for a draw (the vertex shader has none: 0). `ps_inputs`: bit r = the loaded pixel shader
// references r<r> (ShaderCache::referenced_interpolators); the masks keep only those bits, so state for interpolators
// the shader never reads does not split keys. PS fields (semantics per Xenia's pipeline cache, reference only):
//  - param_gen register: SQ_CONTEXT_MISC.param_gen_pos (bits 8-15) when SQ_PROGRAM_CNTL.param_gen (bit 18) is set
//    and the position names an interpolator (< 16), else 0xFF. The point bit (a point-list draw) is set only with a
//    param_gen register: it changes nothing otherwise and would only split keys;
//  - flat: SQ_INTERPOLATOR_CNTL.param_shade (bits 0-15);
//  - centroid: the interpolators NOT sampled at the pixel centre. A sampling-pattern bit set means "centre"; the
//    pattern is all centres with 1x MSAA (RB_SURFACE_INFO.msaa_samples, bits 16-17, = 0) or with
//    SQ_CONTEXT_MISC.sc_sample_cntl (bits 2-3) = 1 (centres only), none with sc_sample_cntl = 0 (centroids only), and
//    SQ_INTERPOLATOR_CNTL.sampling_pattern (bits 16-31) with sc_sample_cntl = 2 (centroids and centres; 3 likewise).
//  The param_gen register itself is never an input, so its flat/centroid bits are dropped too.
uint64_t key_modifiers(ShaderStage stage, const RegisterFile& regs, const DrawInfo& draw, uint32_t ps_inputs = 0xFFFF);
// The modifiers decoded into the translator's options.
TranslateOptions key_options(const ShaderKey& key);
// "v<kTranslatorVersion>-<vs|ps>-<hash:016x>-<dwords>-<mod:016x>.spv"
std::string cache_file_name(const ShaderKey& key);

}  // namespace lr::gpu::shader

namespace lr::gpu {

// The shaders a draw uses, as GpuSystem hands them to Backend::draw. nullptr: no microcode loaded for that stage,
// or its translation failed (the key still names it). The pointers stay valid for the GpuSystem's lifetime.
struct ActiveShaders {
  const shader::TranslatedShader* vs = nullptr;
  const shader::TranslatedShader* ps = nullptr;
  shader::ShaderKey vs_key{ShaderStage::Vertex, 0, 0, 0};
  shader::ShaderKey ps_key{ShaderStage::Pixel, 0, 0, 0};
};

}  // namespace lr::gpu
