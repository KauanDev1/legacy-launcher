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

namespace {
constexpr uint64_t kHostPage = 0x1000;  // x86-64 Linux; the guest heaps use 4 KiB or larger pages
}  // namespace

Result<void> GuestMemory::protect(uint32_t addr, uint32_t size, int prot) {
  const uint64_t s = addr / kHostPage * kHostPage;
  const uint64_t e = (static_cast<uint64_t>(addr) + size + kHostPage - 1) / kHostPage * kHostPage;
  if (e > size_) return Error{ErrorCode::OutOfRange, "protect: range runs past guest memory", ""};
  if (e == s) return {};
  if (::mprotect(base_ + s, e - s, prot) != 0)
    return Error{ErrorCode::IoError, std::string("mprotect failed: ") + std::strerror(errno), ""};
  return {};
}

Result<void> GuestMemory::zero(uint32_t addr, uint32_t size) {
  const uint64_t s = addr, e = s + size;
  if (e > size_) return Error{ErrorCode::OutOfRange, "zero: range runs past guest memory", ""};
  const uint64_t ps = (s + kHostPage - 1) / kHostPage * kHostPage;  // first whole page
  const uint64_t pe = e / kHostPage * kHostPage;                     // end of the last whole page
  if (ps >= pe) {
    std::memset(base_ + s, 0, size);
    return {};
  }
  std::memset(base_ + s, 0, ps - s);
  std::memset(base_ + pe, 0, e - pe);
  if (::madvise(base_ + ps, pe - ps, MADV_DONTNEED) != 0)
    return Error{ErrorCode::IoError, std::string("madvise failed: ") + std::strerror(errno), ""};
  return {};
}

void GuestMemory::release() {
  if (base_ != nullptr) ::munmap(base_, size_);
  base_ = nullptr;
  size_ = 0;
}

}  // namespace lr::runtime
