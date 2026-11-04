// SPDX-License-Identifier: MIT
#include "runtime/guest_memory.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

#include "runtime/memory_map.h"
#include "runtime/write_watch.h"

namespace lr::runtime {

namespace {

constexpr uint64_t kHostPage = 0x1000;  // x86-64 Linux; the guest heaps use 4 KiB or larger pages
constexpr uint64_t kGuestSpace = 0x1'0000'0000ull;
constexpr uint32_t kE0Offset = 0x1000;  // the 0xE0 view starts at physical 0x1000

// One physical view: guest [base, base + len) shows physical [phys, phys + len).
struct View {
  uint64_t base;
  uint64_t len;
  uint64_t phys;
};
constexpr View kViews[] = {
    {kPhysicalViewA0, kPhysicalSize, 0},
    {kPhysicalViewC0, kPhysicalSize, 0},
    {kPhysicalViewE0, kPhysicalSize - kE0Offset, kE0Offset},
};

Error io_error(const std::string& what) {
  return Error{ErrorCode::IoError, what + ": " + std::strerror(errno), ""};
}

// The shared physical store, mapped over the reservation at every view. Returns the store's fd.
Result<int> map_physical_views(uint8_t* base) {
  const int fd = ::memfd_create("legacyrecomp-physical", MFD_CLOEXEC);
  if (fd < 0) return io_error("memfd_create for the physical memory failed");
  if (::ftruncate(fd, kPhysicalSize) != 0) {
    Error e = io_error("sizing the physical memory failed");
    ::close(fd);
    return e;
  }
  for (const View& v : kViews) {
    void* p = ::mmap(base + v.base, v.len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED | MAP_NORESERVE, fd,
                     static_cast<off_t>(v.phys));
    if (p == MAP_FAILED) {
      Error e = io_error("mapping a physical memory view failed");
      ::close(fd);
      return e;
    }
  }
  return fd;
}

}  // namespace

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
  auto* base = static_cast<uint8_t*>(p);
  int fd = -1;
  if (bytes >= kGuestSpace) {
    auto mapped = map_physical_views(base);
    if (!mapped) {
      ::munmap(p, bytes);
      return mapped.error();
    }
    fd = mapped.value();
  }
  return GuestMemory(base, bytes, fd);
}

GuestMemory::GuestMemory(GuestMemory&& o) noexcept
    : base_(std::exchange(o.base_, nullptr)), size_(std::exchange(o.size_, 0)),
      physical_fd_(std::exchange(o.physical_fd_, -1)) {}

GuestMemory& GuestMemory::operator=(GuestMemory&& o) noexcept {
  if (this != &o) {
    release();
    base_ = std::exchange(o.base_, nullptr);
    size_ = std::exchange(o.size_, 0);
    physical_fd_ = std::exchange(o.physical_fd_, -1);
  }
  return *this;
}

GuestMemory::~GuestMemory() { release(); }

Result<void> GuestMemory::protect(uint32_t addr, uint32_t size, int prot) {
  const uint64_t s = addr / kHostPage * kHostPage;
  const uint64_t e = (static_cast<uint64_t>(addr) + size + kHostPage - 1) / kHostPage * kHostPage;
  if (e > size_) return Error{ErrorCode::OutOfRange, "protect: range runs past guest memory", ""};
  if (e == s) return {};
  if (::mprotect(base_ + s, e - s, prot) != 0)
    return Error{ErrorCode::IoError, std::string("mprotect failed: ") + std::strerror(errno), ""};
  return {};
}

Result<void> GuestMemory::drop_pages(uint64_t start, uint64_t end) {
  while (start < end) {
    // The part of [start, end) inside one physical view, if any: a hole in the physical store (works whatever the
    // view's protection, and every alias sees it). The rest is private memory: madvise.
    const View* in = nullptr;
    uint64_t stop = end;
    if (physical_fd_ >= 0) {
      for (const View& v : kViews) {
        if (start >= v.base && start < v.base + v.len) in = &v;
        else if (start < v.base) stop = std::min(stop, v.base);
      }
    }
    if (in != nullptr) {
      stop = std::min(end, in->base + in->len);
      const auto off = static_cast<off_t>(in->phys + (start - in->base));
      if (::fallocate(physical_fd_, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, off, static_cast<off_t>(stop - start)) !=
          0)
        return io_error("zeroing physical memory failed");
    } else if (::madvise(base_ + start, stop - start, MADV_DONTNEED) != 0) {
      return io_error("madvise failed");
    }
    start = stop;
  }
  return {};
}

Result<void> GuestMemory::zero(uint32_t addr, uint32_t size) {
  const uint64_t s = addr, e = s + size;
  if (e > size_) return Error{ErrorCode::OutOfRange, "zero: range runs past guest memory", ""};
  write_watch_note_host_write(addr, size, "zero");  // dropping pages never faults (runtime/write_watch.h)
  const uint64_t ps = (s + kHostPage - 1) / kHostPage * kHostPage;  // first whole page
  const uint64_t pe = e / kHostPage * kHostPage;                     // end of the last whole page
  if (ps >= pe) {
    std::memset(base_ + s, 0, size);
    return {};
  }
  std::memset(base_ + s, 0, ps - s);
  std::memset(base_ + pe, 0, e - pe);
  return drop_pages(ps, pe);
}

void GuestMemory::release() {
  if (base_ != nullptr) ::munmap(base_, size_);
  if (physical_fd_ >= 0) ::close(physical_fd_);
  base_ = nullptr;
  size_ = 0;
  physical_fd_ = -1;
}

}  // namespace lr::runtime
