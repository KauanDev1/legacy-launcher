// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <span>

namespace lr::runtime {

struct FuncMapping {
  uint32_t guest;  // guest address of the function entry
  void* host;      // native function (PPCFunc*)
};

// Fills XenonRecomp's indirect-call table: for each mapping writes `host` at
// base + table_offset + (guest - code_base) * 2, matching PPC_LOOKUP_FUNC
// (an 8-byte slot per 4-byte instruction). table_offset = PPC_IMAGE_BASE + PPC_IMAGE_SIZE.
//
// Precondition: guest >= code_base and every computed slot lies inside the memory behind
// `base` (the table region must be mapped; GuestMemory's reservation covers it). Violations
// are checked with assert in debug builds only.
void install_function_table(uint8_t* base, uint64_t table_offset, uint32_t code_base, std::span<const FuncMapping> mappings);

}  // namespace lr::runtime
