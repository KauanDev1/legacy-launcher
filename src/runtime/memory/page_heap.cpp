// SPDX-License-Identifier: MIT
#include "runtime/memory/page_heap.h"

#include <algorithm>

namespace lr::runtime {
namespace {

Error err(ErrorCode c, const char* m) { return Error{c, m, {}}; }

bool valid_protect(uint32_t p) {
  constexpr uint32_t kMods = kPageGuard | kPageNoCache | kPageWriteCombine;
  if (p & ~(0xFFu | kMods)) return false;
  switch (p & 0xFFu) {
    case kPageNoAccess:
    case kPageReadOnly:
    case kPageReadWrite:
    case kPageExecute:
    case kPageExecuteRead:
    case kPageExecuteReadWrite:
      return true;
    default:
      return false;
  }
}

}  // namespace

PageHeap::PageHeap(uint32_t base, uint32_t end, uint32_t page_size) : base_(base), end_(end), page_(page_size) {
  const size_t n = static_cast<size_t>((static_cast<uint64_t>(end) - base) / page_size);
  state_.assign(n, kFree);
  protect_.assign(n, 0);
  alloc_base_.assign(n, 0);
}

void PageHeap::set_commit_hook(CommitHook hook) {
  std::lock_guard<std::mutex> lk(mu_);
  hook_ = std::move(hook);
}

void PageHeap::add_run(Runs& runs, size_t idx) const {
  if (!runs.empty() && static_cast<uint64_t>(runs.back().first) + runs.back().second == addr_of(idx))
    runs.back().second += page_;
  else
    runs.push_back({addr_of(idx), page_});
}

bool PageHeap::run_free(size_t first, size_t count) const {
  for (size_t i = first; i < first + count; ++i)
    if (state_[i] != kFree) return false;
  return true;
}

Result<uint32_t> PageHeap::reserve(uint32_t size, uint32_t alignment, bool top_down,
                                   std::optional<uint32_t> fixed_base) {
  if (size == 0) return err(ErrorCode::InvalidFormat, "reserve: size is 0");
  if (alignment != 0 && (alignment & (alignment - 1)) != 0)
    return err(ErrorCode::InvalidFormat, "reserve: alignment is not a power of two");
  const uint64_t span = static_cast<uint64_t>(end_) - base_;
  const uint64_t rsize = (static_cast<uint64_t>(size) + page_ - 1) / page_ * page_;
  if (rsize > span) return err(ErrorCode::OutOfRange, "reserve: larger than the heap");
  const size_t count = static_cast<size_t>(rsize / page_);
  const uint64_t align = std::max<uint64_t>(alignment, page_);

  std::lock_guard<std::mutex> lk(mu_);
  uint64_t start = 0;
  if (fixed_base) {
    start = static_cast<uint64_t>(*fixed_base) / page_ * page_;
    if (start < base_ || start + rsize > end_) return err(ErrorCode::OutOfRange, "reserve: fixed range outside heap");
    if (!run_free(index(start), count)) return err(ErrorCode::OutOfRange, "reserve: range already reserved");
  } else {
    bool found = false;
    if (!top_down) {
      uint64_t a = (static_cast<uint64_t>(base_) + align - 1) / align * align;
      while (a + rsize <= end_) {
        const size_t i = index(a);
        size_t last_used = count;  // highest used page inside the window
        for (size_t k = count; k-- > 0;) {
          if (state_[i + k] != kFree) {
            last_used = k;
            break;
          }
        }
        if (last_used == count) {
          start = a;
          found = true;
          break;
        }
        const uint64_t next = static_cast<uint64_t>(addr_of(i + last_used)) + page_;
        a = (next + align - 1) / align * align;
      }
    } else {
      uint64_t a = (static_cast<uint64_t>(end_) - rsize) / align * align;
      while (a >= base_) {
        const size_t i = index(a);
        size_t first_used = count;  // lowest used page inside the window
        for (size_t k = 0; k < count; ++k) {
          if (state_[i + k] != kFree) {
            first_used = k;
            break;
          }
        }
        if (first_used == count) {
          start = a;
          found = true;
          break;
        }
        // The window must end at or below the used page.
        const uint64_t limit = addr_of(i + first_used);
        if (limit < static_cast<uint64_t>(base_) + rsize) break;
        a = (limit - rsize) / align * align;
      }
    }
    if (!found) return err(ErrorCode::OutOfRange, "reserve: no free range");
  }
  const size_t first = index(start);
  for (size_t i = first; i < first + count; ++i) {
    state_[i] = kReserved;
    protect_[i] = 0;
    alloc_base_[i] = static_cast<uint32_t>(start);
  }
  allocs_[static_cast<uint32_t>(start)] = Alloc{static_cast<uint32_t>(rsize), kPageReadWrite};
  return static_cast<uint32_t>(start);
}

Result<void> PageHeap::locate(uint32_t addr, uint32_t size, size_t* first, size_t* count) const {
  if (size == 0) return err(ErrorCode::InvalidFormat, "size is 0");
  const uint64_t s = static_cast<uint64_t>(addr) / page_ * page_;
  const uint64_t e = (static_cast<uint64_t>(addr) + size + page_ - 1) / page_ * page_;
  if (s < base_ || e > end_) return err(ErrorCode::OutOfRange, "range outside the heap");
  const size_t f = index(s), n = static_cast<size_t>((e - s) / page_);
  if (state_[f] == kFree) return err(ErrorCode::NotFound, "address not allocated");
  const uint32_t ab = alloc_base_[f];
  for (size_t i = f; i < f + n; ++i) {
    if (state_[i] == kFree) return err(ErrorCode::NotFound, "range includes unallocated pages");
    if (alloc_base_[i] != ab) return err(ErrorCode::OutOfRange, "range spans allocations");
  }
  *first = f;
  *count = n;
  return {};
}

Result<void> PageHeap::commit(uint32_t addr, uint32_t size, uint32_t protect) {
  if (!valid_protect(protect)) return err(ErrorCode::InvalidFormat, "commit: bad protection");
  Runs runs;
  CommitHook hook;
  {
    std::lock_guard<std::mutex> lk(mu_);
    size_t f = 0, n = 0;
    LR_TRY(locate(addr, size, &f, &n));
    for (size_t i = f; i < f + n; ++i) {
      if (state_[i] != kCommitted) {
        add_run(runs, i);
        state_[i] = kCommitted;
      }
      protect_[i] = static_cast<uint16_t>(protect);
    }
    hook = hook_;
  }
  if (hook)
    for (auto& r : runs) hook(r.first, r.second, true);
  return {};
}

Result<void> PageHeap::decommit(uint32_t addr, uint32_t size) {
  Runs runs;
  CommitHook hook;
  {
    std::lock_guard<std::mutex> lk(mu_);
    size_t f = 0, n = 0;
    LR_TRY(locate(addr, size, &f, &n));
    for (size_t i = f; i < f + n; ++i) {
      if (state_[i] != kCommitted) continue;
      add_run(runs, i);
      state_[i] = kReserved;
      protect_[i] = 0;
    }
    hook = hook_;
  }
  if (hook)
    for (auto& r : runs) hook(r.first, r.second, false);
  return {};
}

Result<void> PageHeap::release(uint32_t allocation_base) {
  Runs runs;
  CommitHook hook;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = allocs_.find(allocation_base);
    if (it == allocs_.end()) return err(ErrorCode::NotFound, "release: not an allocation base");
    const size_t f = index(allocation_base), n = it->second.size / page_;
    for (size_t i = f; i < f + n; ++i) {
      if (state_[i] == kCommitted) add_run(runs, i);
      state_[i] = kFree;
      protect_[i] = 0;
      alloc_base_[i] = 0;
    }
    allocs_.erase(it);
    hook = hook_;
  }
  if (hook)
    for (auto& r : runs) hook(r.first, r.second, false);
  return {};
}

Result<RegionInfo> PageHeap::query(uint32_t addr) const {
  std::lock_guard<std::mutex> lk(mu_);
  if (addr < base_ || addr >= end_) return err(ErrorCode::OutOfRange, "query: outside the heap");
  const size_t at = index(addr);
  auto same = [&](size_t i) {
    if (state_[i] != state_[at]) return false;
    if (state_[at] == kFree) return true;
    return alloc_base_[i] == alloc_base_[at] && protect_[i] == protect_[at];
  };
  size_t lo = at, hi = at;
  while (lo > 0 && same(lo - 1)) --lo;
  while (hi + 1 < state_.size() && same(hi + 1)) ++hi;
  RegionInfo r;
  r.base = addr_of(lo);
  r.size = static_cast<uint32_t>((hi - lo + 1) * static_cast<uint64_t>(page_));
  if (state_[at] == kFree) return r;
  r.state = state_[at] == kCommitted ? RegionInfo::Committed : RegionInfo::Reserved;
  r.allocation_base = alloc_base_[at];
  r.protect = protect_[at];
  r.allocation_protect = allocs_.at(r.allocation_base).protect;
  return r;
}

Result<uint32_t> PageHeap::allocation_size(uint32_t allocation_base) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = allocs_.find(allocation_base);
  if (it == allocs_.end()) return err(ErrorCode::NotFound, "not an allocation base");
  return it->second.size;
}

Result<void> PageHeap::protect(uint32_t addr, uint32_t size, uint32_t protect, uint32_t* old_protect) {
  if (!valid_protect(protect)) return err(ErrorCode::InvalidFormat, "protect: bad protection");
  std::lock_guard<std::mutex> lk(mu_);
  size_t f = 0, n = 0;
  LR_TRY(locate(addr, size, &f, &n));
  for (size_t i = f; i < f + n; ++i)
    if (state_[i] != kCommitted) return err(ErrorCode::OutOfRange, "protect: range not committed");
  if (old_protect) *old_protect = protect_[f];
  for (size_t i = f; i < f + n; ++i) protect_[i] = static_cast<uint16_t>(protect);
  return {};
}

}  // namespace lr::runtime
