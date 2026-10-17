// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

#include "common/result.h"

namespace lr::runtime {

// Allocator for the Xbox 360's 512 MiB of physical memory, handed out as guest addresses in one of three views
// (0xA0000000 = 64 KiB pages, 0xC0000000 = 16 MiB pages, 0xE0000000 = 4 KiB pages). M2 does no aliasing: each
// physical page belongs to exactly one view, chosen by the page size of the allocation. Address bookkeeping only;
// the views' backing memory is the caller's business.
//
// Semantics confirmed against Xenia (MmAllocatePhysicalMemoryEx, PhysicalHeap; no code copied):
//  - Page size comes from the *protect* argument: MEM_LARGE_PAGES (0x20000000) -> 64 KiB, else MEM_16MB_PAGES
//    (0x80000000) -> 16 MiB, else 4 KiB. Xenia additionally requires PAGE_READONLY or PAGE_READWRITE in the low
//    protect bits and fails otherwise; we do the same (InvalidFormat).
//  - Size and alignment round up to the page size; alignment 0 stays "page size". Search is always top-down.
//  - min/max are physical addresses; Xenia has no special case for 0 / 0xFFFFFFFF, they simply clamp to the heap
//    (so 0..0xFFFFFFFF means "everywhere"). max is inclusive (the last byte of the allocation must be <= max).
//  - Xenia's PhysicalHeap::GetPhysicalAddress adds 0x1000 in the E0 view (guest 0xE0000000 <-> physical 0x1000):
//    physical = (addr & 0x1FFFFFFF) + 0x1000 there, so an E0 guest address is 0xE0000000 + phys - 0x1000.
//    Physical 0..0xFFF is therefore never handed out through E0; the guest end stays below 2^32 (64-bit math).
//
// Error contract: InvalidFormat bad argument (size 0, bad protect/alignment); OutOfRange no space or bad range;
// NotFound free/allocation_size of an address that is not (inside) an allocation. Thread-safe.
class PhysicalHeap {
 public:
  static constexpr uint32_t kMemLargePages = 0x20000000;
  static constexpr uint32_t kMem16MbPages = 0x80000000;

  PhysicalHeap();

  Result<uint32_t> allocate(uint32_t size, uint32_t protect, uint32_t min_phys, uint32_t max_phys,
                            uint32_t alignment);
  // guest_address must be exactly what allocate returned.
  Result<void> free(uint32_t guest_address);
  static uint32_t physical_address(uint32_t guest_address);
  // Size of the whole allocation containing guest_address (any address inside it).
  Result<uint32_t> allocation_size(uint32_t guest_address) const;

 private:
  struct Alloc {
    uint32_t size;
    uint32_t first_page;  // 4 KiB page index in physical space
  };

  mutable std::mutex mu_;
  std::vector<uint8_t> used_;         // one entry per 4 KiB physical page
  std::map<uint32_t, Alloc> allocs_;  // keyed by guest address
};

}  // namespace lr::runtime
