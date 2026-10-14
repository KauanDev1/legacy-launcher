// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <span>

#include "common/result.h"
#include "runtime/guest_memory.h"

namespace lr::runtime {

struct FuncMapping {
  uint32_t guest;  // guest address of the function entry
  void* host;      // native function (PPCFunc*)
};

// Fills XenonRecomp's indirect-call table: for each mapping writes `host` at
// mem.base() + table_offset + (guest - code_base) * 2, matching PPC_LOOKUP_FUNC
// (an 8-byte slot per 4-byte instruction). table_offset = PPC_IMAGE_BASE + PPC_IMAGE_SIZE.
//
// Every mapping must satisfy code_base <= guest < code_base + code_size, guest % 4 == 0, and
// its 8-byte slot must lie inside mem.size(); otherwise InvalidFormat naming the guest address.
// Mappings are validated one by one; earlier valid ones are already written on failure.
Result<void> install_function_table(const GuestMemory& mem, uint64_t table_offset, uint32_t code_base, uint32_t code_size,
                                    std::span<const FuncMapping> mappings);

}  // namespace lr::runtime
