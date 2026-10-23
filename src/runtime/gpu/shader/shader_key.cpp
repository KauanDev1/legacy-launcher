// SPDX-License-Identifier: MIT
#include "runtime/gpu/shader/shader_key.h"

#include <cstddef>
#include <cstdio>

#include "runtime/gpu/shader/translator.h"

// Header-only xxHash (XenonRecomp's bundled copy): nothing extra to link.
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace lr::gpu::shader {

uint64_t microcode_hash(const uint8_t* guest_microcode_be, uint32_t dwords) {
  return XXH3_64bits(guest_microcode_be, size_t{dwords} * 4);
}

uint64_t key_modifiers(ShaderStage stage, const RegisterFile& regs, const DrawInfo& draw) {
  if (stage != ShaderStage::Pixel) return 0;
  uint64_t mod = kNoParamGen;
  if ((regs.read(SQ_PROGRAM_CNTL) >> 18) & 1) {
    const uint32_t pos = (regs.read(SQ_CONTEXT_MISC) >> 8) & 0xFF;
    if (pos < 16) {
      mod = pos;
      if (draw.prim_type == kPrimPointList) mod |= kModPointBit;
    }
  }
  const uint32_t interp = regs.read(SQ_INTERPOLATOR_CNTL);
  mod |= uint64_t{interp & 0xFFFF} << kModFlatShift;
  mod |= uint64_t{interp >> 16} << kModCentroidShift;
  return mod;
}

TranslateOptions key_options(const ShaderKey& key) {
  TranslateOptions o;
  if (key.stage != ShaderStage::Pixel) return o;
  o.ps_param_gen = static_cast<uint32_t>(key.modifiers & kModParamGenMask);
  o.ps_point = (key.modifiers & kModPointBit) != 0;
  o.flat_mask = static_cast<uint16_t>(key.modifiers >> kModFlatShift);
  o.centroid_mask = static_cast<uint16_t>(key.modifiers >> kModCentroidShift);
  return o;
}

std::string cache_file_name(const ShaderKey& key) {
  char b[96];
  std::snprintf(b, sizeof b, "v%u-%s-%016llx-%u-%016llx.spv", kTranslatorVersion,
                key.stage == ShaderStage::Vertex ? "vs" : "ps", static_cast<unsigned long long>(key.microcode_hash),
                key.dwords, static_cast<unsigned long long>(key.modifiers));
  return b;
}

}  // namespace lr::gpu::shader
