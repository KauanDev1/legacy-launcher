// SPDX-License-Identifier: MIT
// The GPU MMIO window in guest memory. XenonRecomp does not emulate MMIO, so the game reads and writes GPU
// registers as ordinary memory in the reserved range 0x7F000000-0x7FFFFFFF; the window below is committed
// read/write and holds idle values, and the GPU thread reads CP_RB_WPTR from it as the write pointer.
// Register index i lives at kMmioWindowBase + 4 * i, big-endian (guest order).
#pragma once

#include <cstdint>

#include "common/result.h"
#include "runtime/guest_memory.h"

namespace lr::gpu {

inline constexpr uint32_t kMmioWindowBase = 0x7FC80000;
inline constexpr uint32_t kMmioWindowEnd = 0x7FD00000;  // exclusive

// RBBM_STATUS (status the D3D runtime may poll). UNCONFIRMED GUESS: the index and the idle value come from the
// AMD R5xx map as remembered, not from a checked source (Xenia's register_table.inc has neither RBBM_STATUS nor
// CP_RB_WPTR, and Xenia returns 0 for unhandled register reads). Open question: whether 0x05D0 is a dword index
// or a byte offset (byte offset / 4 = 0x174). To be checked when the real game runs (Task 12).
inline constexpr uint32_t kRegRbbmStatus = 0x05D0;
// Idle guess: "GUI active" (bit 31) and every busy bit clear, CMDFIFO_AVAIL (bits 0-6) at 64 entries.
inline constexpr uint32_t kRbbmStatusIdle = 0x00000040;

// Static values Xenia's GraphicsSystem::ReadRegister hard-codes for MMIO reads (hardware facts, reference only,
// no code copied): memory timing, EDRAM bus control, display vertical counter, interrupt status, and the 1280x720
// viewport size.
inline constexpr uint32_t kRegRbEdramTiming = 0x0F00;
inline constexpr uint32_t kRbEdramTimingValue = 0x08100748;
inline constexpr uint32_t kRegRbBcControl = 0x0F01;
inline constexpr uint32_t kRbBcControlValue = 0x0000200E;
inline constexpr uint32_t kRegD1ModeVCounter = 0x194C;
inline constexpr uint32_t kD1ModeVCounterValue = 0x000002D0;
inline constexpr uint32_t kRegInterruptStatus = 0x1951;
inline constexpr uint32_t kInterruptStatusValue = 1;
inline constexpr uint32_t kRegD1ModeViewportSize = 0x1961;
inline constexpr uint32_t kD1ModeViewportSizeValue = 0x050002D0;

constexpr uint32_t mmio_register_address(uint32_t index) { return kMmioWindowBase + 4 * index; }

// Commits the window read/write and stores the initial register values into it (everything else reads 0).
// `mem` must span the 4 GiB guest space.
Result<void> init_mmio_window(runtime::GuestMemory& mem);

}  // namespace lr::gpu
