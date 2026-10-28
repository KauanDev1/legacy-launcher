// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "common/result.h"
#include "formats/xex2.h"
#include "runtime/guest_memory.h"

namespace lr::runtime {

// Stack size of the main guest thread when the XEX has no default stack size header (0x00020200).
inline constexpr uint32_t kDefaultStackSize = 256 * 1024;

struct LoadedImage {
  uint32_t base = 0;   // guest address of the image start (e.g. 0x82000000)
  uint32_t size = 0;   // image extent in bytes
  uint32_t entry = 0;  // guest address of the entry point
  // XEX2 only (0 / empty for other images): guest copy of the XEX header (at kKernelDataBase) and the
  // executable's X_LDR_DATA_TABLE_ENTRY (its module handle), both in the kernel data area.
  uint32_t header_guest = 0;
  uint32_t ldr_entry = 0;
  std::optional<formats::TlsInfo> tls;
  uint32_t stack_size = kDefaultStackSize;
};

// Parses the (patched, decrypted) image via XenonUtils and maps it: an XEX2 as the whole decompressed image (its
// resources lie outside every PE section), other images (the test ELF) section by section.
// XenonUtils reports section bases as absolute guest addresses (image base + RVA).
// For an XEX2 it also copies the XEX header (pe_data_offset bytes) to the kernel data area and builds the module's
// X_LDR_DATA_TABLE_ENTRY after it: init_kernel must have run (the copies come from Kernel::kernel_data_alloc).
// Record slots of imports are left as ParseImage leaves them; resolve_variable_imports (kernel/variables.h) fixes
// the type-0 ones.
// InvalidFormat: unparsable image / entry outside the image; OutOfRange: a section does not fit in `mem`;
// InsufficientResources: the kernel data area is full.
Result<LoadedImage> load_image(GuestMemory& mem, std::span<const std::byte> xex);

}  // namespace lr::runtime
