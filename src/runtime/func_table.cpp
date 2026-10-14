// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/func_table.h"

#include <cassert>
#include <cstring>

namespace lr::runtime {

void install_function_table(uint8_t* base, uint64_t table_offset, uint32_t code_base, std::span<const FuncMapping> mappings) {
  for (const FuncMapping& m : mappings) {
    assert(m.guest >= code_base);
    const uint64_t slot = table_offset + static_cast<uint64_t>(m.guest - code_base) * 2;
    std::memcpy(base + slot, &m.host, sizeof(void*));
  }
}

}  // namespace lr::runtime
