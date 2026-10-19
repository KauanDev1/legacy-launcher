// SPDX-License-Identifier: MIT
#include "runtime/gpu/mmio.h"

#include <sys/mman.h>

#include <cstring>

#include "runtime/gpu/registers.h"

namespace lr::gpu {
namespace {
void store_be(uint8_t* base, uint32_t index, uint32_t value) {
  const uint32_t be = __builtin_bswap32(value);
  std::memcpy(base + mmio_register_address(index), &be, 4);
}
}  // namespace

Result<void> init_mmio_window(uint8_t* base) {
  if (mprotect(base + kMmioWindowBase, kMmioWindowEnd - kMmioWindowBase, PROT_READ | PROT_WRITE) != 0)
    return Error{ErrorCode::IoError, "cannot commit the GPU MMIO window", ""};
  std::memset(base + kMmioWindowBase, 0, kMmioWindowEnd - kMmioWindowBase);
  store_be(base, kRegRbbmStatus, kRbbmStatusIdle);
  store_be(base, CP_RB_WPTR, 0);
  store_be(base, COHER_STATUS_HOST, 0);  // no cache sync pending (bit 31 clear)
  return {};
}

}  // namespace lr::gpu
