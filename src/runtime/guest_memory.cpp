// SPDX-License-Identifier: MIT
#include "runtime/guest_memory.h"

#include <sys/mman.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

namespace lr::runtime {

Result<GuestMemory> GuestMemory::reserve(uint64_t bytes) {
  if (bytes == 0) {
    return Error{ErrorCode::OutOfRange, "cannot reserve 0 bytes of guest memory", ""};
  }
  void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) {
    return Error{ErrorCode::InsufficientResources,
                 "mmap of " + std::to_string(bytes) + " bytes failed: " + std::strerror(errno),
                 "check ulimit -v and vm.overcommit_memory"};
  }
  return GuestMemory(static_cast<uint8_t*>(p), bytes);
}

GuestMemory::GuestMemory(GuestMemory&& o) noexcept : base_(std::exchange(o.base_, nullptr)), size_(std::exchange(o.size_, 0)) {}

GuestMemory& GuestMemory::operator=(GuestMemory&& o) noexcept {
  if (this != &o) {
    release();
    base_ = std::exchange(o.base_, nullptr);
    size_ = std::exchange(o.size_, 0);
  }
  return *this;
}

GuestMemory::~GuestMemory() { release(); }

void GuestMemory::release() {
  if (base_ != nullptr) ::munmap(base_, size_);
  base_ = nullptr;
  size_ = 0;
}

}  // namespace lr::runtime
