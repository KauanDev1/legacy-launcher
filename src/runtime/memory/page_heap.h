// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "common/result.h"

namespace lr::runtime {

// Xbox 360 PAGE_* protection values.
enum PageProtect : uint32_t {
  kPageNoAccess = 0x01,
  kPageReadOnly = 0x02,
  kPageReadWrite = 0x04,
  kPageExecute = 0x10,
  kPageExecuteRead = 0x20,
  kPageExecuteReadWrite = 0x40,
  kPageGuard = 0x100,
  kPageNoCache = 0x200,
  kPageWriteCombine = 0x400,
};

struct RegionInfo {
  enum State { Free, Reserved, Committed };
  uint32_t base = 0;             // first page of the run of pages sharing state/protect/allocation
  uint32_t allocation_base = 0;  // 0 when Free
  uint32_t size = 0;
  uint32_t protect = 0;  // 0 unless Committed
  uint32_t allocation_protect = 0;
  State state = Free;
};

// Address-space bookkeeping for one guest heap (semantics follow Xenia's BaseHeap; no code copied). It manages
// addresses only: backing host memory is the caller's business, signalled through the commit hook.
//
// Error contract (Task 13 maps these to NTSTATUS):
//   ErrorCode::InvalidFormat  bad argument (size 0, bad protect, bad alignment)  -> STATUS_INVALID_PARAMETER
//   ErrorCode::OutOfRange     no space / conflicting or already reserved range / range outside the heap or
//                             spanning allocations / protect on uncommitted pages
//                             -> STATUS_NO_MEMORY or STATUS_CONFLICTING_ADDRESSES
//   ErrorCode::NotFound       address not allocated / release with a non-base address
//                             -> STATUS_MEMORY_NOT_ALLOCATED
//
// Sizes round up and addresses round down to the page size. Thread-safe. The commit hook is called OUTSIDE the
// state mutex but while holding the operation mutex: mutators (reserve/commit/decommit/release/protect) and
// their hooks are fully serialized, so a hook never sees a later operation overtake it. The hook may call
// query()/allocation_size() (state mutex only) but MUST NOT call mutators (it would deadlock).
class PageHeap {
 public:
  using CommitHook = std::function<void(uint32_t addr, uint32_t size, bool committed, bool zero)>;

  PageHeap(uint32_t base, uint32_t end, uint32_t page_size);

  // Reserves address space (not committed). alignment 0 means page size; otherwise a power of two. fixed_base
  // (rounded down to a page) places the range exactly; else first fit bottom-up, or highest fit if top_down.
  // `protect` is recorded as the allocation protect (MEMORY_BASIC_INFORMATION.AllocationProtect).
  Result<uint32_t> reserve(uint32_t size, uint32_t alignment, bool top_down, std::optional<uint32_t> fixed_base,
                              uint32_t protect = kPageReadWrite);
  // Commits pages of one allocation and sets their protection; the hook gets each newly committed run, with `zero`
  // telling it whether the pages must be zeroed (false for MEM_NOZERO).
  Result<void> commit(uint32_t addr, uint32_t size, uint32_t protect, bool zero = true);
  // Returns committed pages to Reserved; the hook gets each decommitted run (committed=false).
  Result<void> decommit(uint32_t addr, uint32_t size);
  // Frees a whole allocation; allocation_base must be its exact base. Committed pages are reported to the hook.
  Result<void> release(uint32_t allocation_base);
  // Windows semantics: base = addr rounded down to a page; size runs forward to the end of the run of pages
  // sharing state/protect/allocation.
  Result<RegionInfo> query(uint32_t addr) const;
  // Size of the whole allocation containing addr (any address inside it). NotFound for free pages.
  Result<uint32_t> allocation_size(uint32_t addr) const;
  // Changes protection of committed pages in one allocation; old_protect (optional) is the first page's.
  Result<void> protect(uint32_t addr, uint32_t size, uint32_t protect, uint32_t* old_protect);
  void set_commit_hook(CommitHook hook);

  uint32_t base() const { return base_; }
  uint32_t end() const { return end_; }
  uint32_t page_size() const { return page_; }

 private:
  struct Alloc {
    uint32_t size;
    uint32_t protect;
  };
  enum : uint8_t { kFree = 0, kReserved = 1, kCommitted = 2 };
  using Runs = std::vector<std::pair<uint32_t, uint32_t>>;

  size_t index(uint64_t addr) const { return static_cast<size_t>((addr - base_) / page_); }
  uint32_t addr_of(size_t idx) const { return static_cast<uint32_t>(base_ + static_cast<uint64_t>(idx) * page_); }
  void add_run(Runs& runs, size_t idx) const;
  bool run_free(size_t first, size_t count) const;
  // Validates [addr,size) lies in one live allocation; fills first/count page indices.
  Result<void> locate(uint32_t addr, uint32_t size, size_t* first, size_t* count) const;

  uint32_t base_, end_, page_;
  std::mutex op_mu_;  // serializes mutators across state change AND hook; always taken before mu_
  mutable std::mutex mu_;
  std::vector<uint8_t> state_;
  std::vector<uint16_t> protect_;
  std::vector<uint32_t> alloc_base_;
  std::map<uint32_t, Alloc> allocs_;
  CommitHook hook_;
};

}  // namespace lr::runtime
