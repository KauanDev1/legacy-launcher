// SPDX-License-Identifier: GPL-3.0-or-later
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
