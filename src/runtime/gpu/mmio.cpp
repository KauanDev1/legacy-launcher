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

Result<void> init_mmio_window(runtime::GuestMemory& mem) {
  uint8_t* const base = mem.base();
  LR_TRY(mem.protect(kMmioWindowBase, kMmioWindowEnd - kMmioWindowBase, PROT_READ | PROT_WRITE));
  std::memset(base + kMmioWindowBase, 0, kMmioWindowEnd - kMmioWindowBase);
  store_be(base, kRegRbbmStatus, kRbbmStatusIdle);
  store_be(base, CP_RB_WPTR, 0);
  store_be(base, COHER_STATUS_HOST, 0);  // no cache sync pending (bit 31 clear)
  store_be(base, kRegRbEdramTiming, kRbEdramTimingValue);
  store_be(base, kRegRbBcControl, kRbBcControlValue);
  store_be(base, kRegD1ModeVCounter, kD1ModeVCounterValue);
  store_be(base, kRegInterruptStatus, kInterruptStatusValue);
  store_be(base, kRegD1ModeViewportSize, kD1ModeViewportSizeValue);
  return {};
}

}  // namespace lr::gpu
