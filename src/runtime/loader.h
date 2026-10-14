// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "common/result.h"
#include "runtime/guest_memory.h"

namespace lr::runtime {

struct LoadedImage {
  uint32_t base;   // guest address of the image start (e.g. 0x82000000)
  uint32_t size;   // image extent in bytes
  uint32_t entry;  // guest address of the entry point
};

// Parses the (patched, decrypted) XEX2 via XenonUtils and copies every section to its guest address.
// XenonUtils reports section bases as absolute guest addresses (image base + RVA).
// InvalidFormat: unparsable image / entry outside the image; OutOfRange: a section does not fit in `mem`.
Result<LoadedImage> load_image(GuestMemory& mem, std::span<const std::byte> xex);

}  // namespace lr::runtime
