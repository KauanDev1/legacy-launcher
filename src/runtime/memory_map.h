// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

// Guest address space layout (spec section 6.1). The whole 4 GiB guest space is one host mapping; these are the
// guest regions the kernel HLE hands out from. All values are guest virtual addresses.
namespace lr::runtime {

inline constexpr uint32_t kNullGuardEnd = 0x00010000;  // [0, 64 KiB) is never mapped

inline constexpr uint32_t kHeap4kBase = 0x00010000;  // 4 KiB pages
inline constexpr uint32_t kHeap4kEnd = 0x40000000;
inline constexpr uint32_t kHeap64kBase = 0x40000000;  // 64 KiB pages
inline constexpr uint32_t kHeap64kEnd = 0x70000000;

inline constexpr uint32_t kStacksBase = 0x70000000;
inline constexpr uint32_t kStacksEnd = 0x7F000000;
inline constexpr uint32_t kGpuReservedBase = 0x7F000000;

inline constexpr uint32_t kKernelDataBase = 0x80000000;
inline constexpr uint32_t kKernelDataEnd = 0x82000000;

// Physical memory views over the same 512 MiB (kPhysicalSize) of backing store.
inline constexpr uint32_t kPhysicalViewA0 = 0xA0000000;  // 64 KiB pages
inline constexpr uint32_t kPhysicalViewC0 = 0xC0000000;  // 16 MiB pages
inline constexpr uint32_t kPhysicalViewE0 = 0xE0000000;  // 4 KiB pages
inline constexpr uint32_t kPhysicalSize = 0x20000000;

}  // namespace lr::runtime
