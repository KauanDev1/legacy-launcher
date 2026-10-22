// SPDX-License-Identifier: MIT
#include "runtime/func_table.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace lr::runtime {

Result<void> install_function_table(const GuestMemory& mem, uint64_t table_offset, uint32_t code_base, uint32_t code_size,
                                    std::span<const FuncMapping> mappings) {
  const uint64_t code_end = static_cast<uint64_t>(code_base) + code_size;
  for (const FuncMapping& m : mappings) {
    char addr[16];
    std::snprintf(addr, sizeof addr, "0x%08X", m.guest);
    const std::string where = std::string("function at guest ") + addr;
    if (m.guest < code_base || m.guest >= code_end) {
      return Error{ErrorCode::InvalidFormat, where + " is outside the code range", ""};
    }
    if (m.guest % 4 != 0) return Error{ErrorCode::InvalidFormat, where + " is not 4-byte aligned", ""};
    // Bound before adding so a huge table_offset cannot wrap uint64.
    const uint64_t delta = static_cast<uint64_t>(m.guest - code_base) * 2;
    if (table_offset > mem.size() || delta > mem.size() - table_offset ||
        mem.size() - table_offset - delta < sizeof(void*)) {
      return Error{ErrorCode::InvalidFormat, where + " maps to a table slot outside guest memory", ""};
    }
    void* slot = nullptr;
    std::memcpy(&slot, mem.base() + table_offset + delta, sizeof(void*));
    if (slot == nullptr) std::memcpy(mem.base() + table_offset + delta, &m.host, sizeof(void*));
  }
  return {};
}

}  // namespace lr::runtime
