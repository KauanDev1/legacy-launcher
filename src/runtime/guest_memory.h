// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include "common/result.h"

namespace lr::runtime {

// One lazily-committed anonymous mapping hosting the guest address space.
// Guest address G lives at base() + G. Move-only.
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
  // to whole 4 KiB host pages. OutOfRange when it runs past size(), IoError when mprotect fails.
  Result<void> protect(uint32_t addr, uint32_t size, int prot);

  // Zero-fills guest [addr, addr + size). Whole host pages are dropped with madvise(MADV_DONTNEED) (they read
  // back as zeros without touching memory, whatever their protection); partial pages at the edges are memset,
  // so those must be writable. OutOfRange when the range runs past size().
  Result<void> zero(uint32_t addr, uint32_t size);

  // No bounds check: the caller guarantees guest + sizeof(T) <= size().
  template <class T>
  T* translate(uint32_t guest) const {
    return reinterpret_cast<T*>(base_ + guest);
  }

 private:
  GuestMemory(uint8_t* base, uint64_t size) : base_(base), size_(size) {}
  void release();

  uint8_t* base_ = nullptr;
  uint64_t size_ = 0;
};

}  // namespace lr::runtime
