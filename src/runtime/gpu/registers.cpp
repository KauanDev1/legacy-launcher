// SPDX-License-Identifier: MIT
#include "runtime/gpu/registers.h"

#include <algorithm>
#include <bit>

namespace lr::gpu {
namespace {

struct Entry {
  uint32_t index;
  const char* name;
};

constexpr auto make_table() {
  std::array<Entry, 0
#define LR_GPU_REG_COUNT(name, index) +1
                        LR_GPU_REGISTERS(LR_GPU_REG_COUNT)
#undef LR_GPU_REG_COUNT
                    >
      table{
#define LR_GPU_REG_ENTRY(name, index) Entry{index, #name},
          LR_GPU_REGISTERS(LR_GPU_REG_ENTRY)
#undef LR_GPU_REG_ENTRY
      };
  std::ranges::sort(table, {}, &Entry::index);
  return table;
}

constexpr auto kTable = make_table();

constexpr bool unique_indices() {
  for (size_t i = 1; i < kTable.size(); ++i) {
    if (kTable[i - 1].index == kTable[i].index) return false;
  }
  return true;
}
static_assert(unique_indices(), "two registers share an index");
static_assert(kTable.back().index < kRegisterCount);

}  // namespace

float RegisterFile::read_float(uint32_t index) const { return std::bit_cast<float>(read(index)); }

const char* RegisterFile::name(uint32_t index) {
  const auto it = std::ranges::lower_bound(kTable, index, {}, &Entry::index);
  return it != kTable.end() && it->index == index ? it->name : nullptr;
}

}  // namespace lr::gpu
