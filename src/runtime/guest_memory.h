// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include "common/result.h"

namespace lr::runtime {

// One lazily-committed anonymous mapping hosting the guest address space.
// Guest address G lives at base() + G. Move-only.
//
// A reservation that spans the whole 4 GiB guest space also gets the console's physical memory: one 512 MiB shared
// store (a memfd) mapped at the three physical views, so they alias like on the hardware. Physical address p is at
// guest 0xA0000000 + p, 0xC0000000 + p and 0xE0000000 + p - 0x1000 (memory_map.h; the 0xE0 view starts at physical
// 0x1000, as Xenia's). The game fills buffers through one view and reads them through another (Task 22, ADR 0005
// M14). The last 4 KiB of the 0xE0 view and everything past 4 GiB stay private anonymous memory.
class GuestMemory {
 public:
  static Result<GuestMemory> reserve(uint64_t bytes);

  GuestMemory(const GuestMemory&) = delete;
  GuestMemory& operator=(const GuestMemory&) = delete;
  GuestMemory(GuestMemory&& o) noexcept;
  GuestMemory& operator=(GuestMemory&& o) noexcept;
  ~GuestMemory();

  uint8_t* base() const { return base_; }
  uint64_t size() const { return size_; }

  // mprotect()s the host pages covering guest [addr, addr + size) to `prot` (PROT_* flags). The range is widened
  // to whole 4 KiB host pages. OutOfRange when it runs past size(), IoError when mprotect fails. In a physical view
  // the protection belongs to that view only, not to its aliases.
  Result<void> protect(uint32_t addr, uint32_t size, int prot);

  // Zero-fills guest [addr, addr + size). Whole host pages are dropped without touching memory, whatever their
  // protection: madvise(MADV_DONTNEED) for private memory, a hole punched in the physical store for the physical
  // views (so every alias reads zeros). Partial pages at the edges are memset, so those must be writable.
  // OutOfRange when the range runs past size().
  Result<void> zero(uint32_t addr, uint32_t size);

  // No bounds check: the caller guarantees guest + sizeof(T) <= size().
  template <class T>
  T* translate(uint32_t guest) const {
    return reinterpret_cast<T*>(base_ + guest);
  }

 private:
  GuestMemory(uint8_t* base, uint64_t size, int physical_fd) : base_(base), size_(size), physical_fd_(physical_fd) {}
  void release();
  Result<void> drop_pages(uint64_t start, uint64_t end);  // whole host pages, start < end

  uint8_t* base_ = nullptr;
  uint64_t size_ = 0;
  int physical_fd_ = -1;  // the physical store, -1 when the reservation has no physical views
};

}  // namespace lr::runtime
