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

uint64_t key_modifiers(ShaderStage stage, const RegisterFile& regs, const DrawInfo& draw, uint32_t ps_inputs) {
  if (stage != ShaderStage::Pixel) return 0;
  uint32_t inputs = ps_inputs & 0xFFFF;
  const uint32_t misc = regs.read(SQ_CONTEXT_MISC);
  uint64_t mod = kNoParamGen;
  if ((regs.read(SQ_PROGRAM_CNTL) >> 18) & 1) {
    const uint32_t pos = (misc >> 8) & 0xFF;
    if (pos < 16) {
      mod = pos;
      if (draw.prim_type == kPrimPointList) mod |= kModPointBit;
      inputs &= ~(1u << pos);  // the param_gen register is generated, never interpolated
    }
  }
  const uint32_t interp = regs.read(SQ_INTERPOLATOR_CNTL);
  // Sampled at the centre (bit set) or at the centroid (Xenia's GetInterpolatorSamplingPattern semantics).
  const uint32_t msaa_samples = (regs.read(RB_SURFACE_INFO) >> 16) & 3;
  const uint32_t sample_control = (misc >> 2) & 3;
  uint32_t centre = interp >> 16;  // centroids and centres: the per-interpolator pattern
  if (msaa_samples == 0 || sample_control == 1) centre = 0xFFFF;  // 1x MSAA, or centres only
  else if (sample_control == 0) centre = 0;                        // centroids only
  mod |= uint64_t{interp & inputs} << kModFlatShift;
  mod |= uint64_t{~centre & inputs} << kModCentroidShift;
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
