// SPDX-License-Identifier: MIT
#include "runtime/memory/physical_heap.h"

#include <algorithm>

#include "runtime/memory_map.h"

namespace lr::runtime {
namespace {
Error err(ErrorCode c, const char* m) { return Error{c, m, {}}; }
constexpr uint64_t kPage = 0x1000;
constexpr uint64_t kE0Offset = 0x1000;
constexpr uint64_t kPhysPages = kPhysicalSize / kPage;

uint64_t view_base_for(uint64_t page_size) {
  if (page_size == 0x10000) return kPhysicalViewA0;
  if (page_size == 0x1000000) return kPhysicalViewC0;
  return kPhysicalViewE0;
}
}  // namespace

PhysicalHeap::PhysicalHeap() : used_(kPhysPages, 0) {}

Result<uint32_t> PhysicalHeap::allocate(uint32_t size, uint32_t protect, uint32_t min_phys, uint32_t max_phys,
                                        uint32_t alignment) {
  if (size == 0) return err(ErrorCode::InvalidFormat, "physical allocation of size 0");
  if ((protect & 0x6) == 0) return err(ErrorCode::InvalidFormat, "physical allocation needs PAGE_READONLY/READWRITE");
  if (alignment != 0 && (alignment & (alignment - 1)) != 0)
    return err(ErrorCode::InvalidFormat, "physical alignment is not a power of two");

  uint64_t page = 0x1000;
  if (protect & kMemLargePages)
    page = 0x10000;
  else if (protect & kMem16MbPages)
    page = 0x1000000;

  const uint64_t bytes = (static_cast<uint64_t>(size) + page - 1) / page * page;
  const uint64_t align = std::max<uint64_t>(page, alignment);
  const uint64_t base = view_base_for(page);
  const bool e0 = base == kPhysicalViewE0;

  if (min_phys > max_phys) return err(ErrorCode::OutOfRange, "empty physical range");
  if (min_phys >= kPhysicalSize) return err(ErrorCode::OutOfRange, "min_phys outside physical memory");
  const uint64_t hi = std::min<uint64_t>(max_phys, kPhysicalSize - 1);  // inclusive last byte allowed
  // Physical 0..0xFFF is unreachable through E0 (guest 0xE0000000 maps to physical 0x1000).
  const uint64_t lo = e0 ? std::max<uint64_t>(min_phys, kE0Offset) : min_phys;
  if (bytes > hi + 1 || hi + 1 - bytes < lo) return err(ErrorCode::OutOfRange, "physical range too small");

  std::lock_guard<std::mutex> lock(mu_);
  const uint64_t np = bytes / kPage;
  // Highest aligned start such that the allocation ends at or before hi, then walk down.
  int64_t start = static_cast<int64_t>((hi + 1 - bytes) / align * align);
  while (start >= 0 && static_cast<uint64_t>(start) >= lo) {
    const uint64_t first = static_cast<uint64_t>(start) / kPage;
    uint64_t conflict = UINT64_MAX;
    for (uint64_t i = np; i-- > 0;) {  // highest used page in the candidate, so we skip the furthest
      if (used_[first + i]) {
        conflict = first + i;
        break;
      }
    }
    if (conflict == UINT64_MAX) {
      std::fill_n(used_.begin() + static_cast<std::ptrdiff_t>(first), np, uint8_t{1});
      const uint64_t phys = first * kPage;
      const uint32_t guest = static_cast<uint32_t>(base + phys - (e0 ? kE0Offset : 0));
      allocs_[guest] = Alloc{static_cast<uint32_t>(bytes), static_cast<uint32_t>(first)};
      by_phys_[static_cast<uint32_t>(phys)] = guest;
      return guest;
    }
    // Next candidate must end at or before the conflicting page.
    const int64_t limit = static_cast<int64_t>(conflict * kPage) - static_cast<int64_t>(bytes);
    if (limit < 0) break;
    start = limit / static_cast<int64_t>(align) * static_cast<int64_t>(align);
  }
  return err(ErrorCode::OutOfRange, "out of physical memory");
}

Result<void> PhysicalHeap::free(uint32_t guest_address) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = allocs_.find(guest_address);
  if (it == allocs_.end()) return err(ErrorCode::NotFound, "address is not a physical allocation base");
  std::fill_n(used_.begin() + it->second.first_page, it->second.size / kPage, uint8_t{0});
  by_phys_.erase(it->second.first_page * static_cast<uint32_t>(kPage));
  allocs_.erase(it);
  return {};
}

uint32_t PhysicalHeap::physical_address(uint32_t guest_address) {
  uint32_t phys = guest_address & (kPhysicalSize - 1);
  if (guest_address >= kPhysicalViewE0) phys += static_cast<uint32_t>(kE0Offset);
  return phys;
}

uint32_t PhysicalHeap::used_pages() const {
  std::lock_guard<std::mutex> lock(mu_);
  uint64_t n = 0;
  for (const auto& [guest, a] : allocs_) n += a.size / kPage;
  return static_cast<uint32_t>(n);
}

Result<uint32_t> PhysicalHeap::allocation_size(uint32_t guest_address) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = allocs_.upper_bound(guest_address);
  if (it != allocs_.begin()) {
    --it;
    if (static_cast<uint64_t>(guest_address) < static_cast<uint64_t>(it->first) + it->second.size)
      return it->second.size;
  }
  return err(ErrorCode::NotFound, "address is not inside a physical allocation");
}

std::optional<uint32_t> PhysicalHeap::guest_from_physical(uint32_t phys) const {
  if (phys >= kPhysicalSize) return std::nullopt;
  std::lock_guard<std::mutex> lock(mu_);
  auto it = by_phys_.upper_bound(phys);
  if (it == by_phys_.begin()) return std::nullopt;
  --it;
  const uint32_t guest = it->second;
  const uint64_t offset = phys - it->first;
  if (offset >= allocs_.at(guest).size) return std::nullopt;
  return static_cast<uint32_t>(guest + offset);
}

}  // namespace lr::runtime
