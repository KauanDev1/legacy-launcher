// SPDX-License-Identifier: MIT
// The GPU MMIO window in guest memory. XenonRecomp does not emulate MMIO, so the game reads and writes GPU
// registers as ordinary memory in the reserved range 0x7F000000-0x7FFFFFFF; the window below is committed
// read/write and holds idle values, and the GPU thread (Task 10) reads CP_RB_WPTR from it as the write pointer.
// Register index i lives at kMmioWindowBase + 4 * i, big-endian (guest order).
#pragma once

#include <cstdint>

#include "common/result.h"

namespace lr::gpu {

inline constexpr uint32_t kMmioWindowBase = 0x7FC80000;
inline constexpr uint32_t kMmioWindowEnd = 0x7FD00000;  // exclusive

// Register indices that are not in the RegisterFile table (status registers the D3D runtime polls).
// RBBM_STATUS: AMD R5xx/Xenos map; index per Xenia register_table.inc (read for the number only). Not
// cross-checked against a second source, see docs/architecture.md.
inline constexpr uint32_t kRegRbbmStatus = 0x05D0;
// Idle: "GUI active" (bit 31) and every busy bit clear, CMDFIFO_AVAIL (bits 0-6) at the full 64 entries.
inline constexpr uint32_t kRbbmStatusIdle = 0x00000040;

constexpr uint32_t mmio_register_address(uint32_t index) { return kMmioWindowBase + 4 * index; }

// Commits the window read/write and stores the initial register values into it (everything else reads 0).
// `base` is the host address of guest address 0 and must span the 4 GiB guest space.
Result<void> init_mmio_window(uint8_t* base);

}  // namespace lr::gpu
