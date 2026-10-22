// SPDX-License-Identifier: MIT
#include "runtime/gpu/shader_cache.h"

#include <cstddef>

// Header-only xxHash (XenonRecomp's bundled copy): nothing extra to link.
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace lr::gpu {

uint64_t microcode_hash(const uint8_t* guest_microcode, uint32_t dwords) {
  return XXH3_64bits(guest_microcode, size_t{dwords} * 4);
}

}  // namespace lr::gpu
