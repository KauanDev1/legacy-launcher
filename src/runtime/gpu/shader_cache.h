// SPDX-License-Identifier: MIT
// The hash that identifies a shader microcode the game loads (IM_LOAD / IM_LOAD_IMMEDIATE). It keys the per-hash load
// statistics (GpuSystem::shader_stats, the "shader ..." trace line) and, from plan B (ADR 0005 M8), the runtime
// translator's cache of translated modules.
#pragma once

#include <cstdint>

namespace lr::gpu {

// XXH3-64 (seed 0) of the `dwords` microcode dwords at `guest_microcode`, read as the bytes they are in guest memory
// (big-endian, no swap).
uint64_t microcode_hash(const uint8_t* guest_microcode, uint32_t dwords);

}  // namespace lr::gpu
