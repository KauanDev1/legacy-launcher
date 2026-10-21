// SPDX-License-Identifier: MIT
// Memory imports: virtual memory (Nt*VirtualMemory) over the 4 KiB / 64 KiB page heaps, physical memory (Mm*) over
// the physical heap, and the kernel pool (ExAllocatePool*, XamAlloc*). Argument order, flag handling and status
// codes follow Xenia's xboxkrnl_memory.cc and xam_info.cc (semantic reference only, no code copied); every
// deviation is commented where it happens. MmMapIoSpace (M3) returns the address it is given.
#include <cstdint>
#include <cstring>
#include <optional>

#include "runtime/kernel/guest_structs.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/status.h"
#include "runtime/memory/page_heap.h"
#include "runtime/memory/physical_heap.h"
#include "runtime/memory_map.h"

namespace lr::kernel {
namespace {

using runtime::PageHeap;
using runtime::RegionInfo;
using U32Ptr = GuestPtr<be<uint32_t>>;

// X_MEM_* (Xenia src/xenia/xbox.h).
constexpr uint32_t MEM_COMMIT = 0x00001000;
constexpr uint32_t MEM_RESERVE = 0x00002000;
constexpr uint32_t MEM_DECOMMIT = 0x00004000;
constexpr uint32_t MEM_FREE = 0x00010000;
constexpr uint32_t MEM_PRIVATE = 0x00020000;
constexpr uint32_t MEM_RESET = 0x00080000;
constexpr uint32_t MEM_TOP_DOWN = 0x00100000;
constexpr uint32_t MEM_NOZERO = 0x00800000;
constexpr uint32_t MEM_LARGE_PAGES = 0x20000000;

constexpr uint32_t kExecuteBits = 0x10 | 0x20 | 0x40 | 0x80;  // PAGE_EXECUTE, _READ, _READWRITE, _WRITECOPY
constexpr uint32_t kPoolTagNone = 0x656E6F4E;                 // 'None', ExAllocatePool's tag (as in Xenia)
constexpr uint32_t E_OUTOFMEMORY = 0x8007000E;

// Xenia stores protection as read/write/nocache/writecombine and converts back on query, so execute and guard
// bits are dropped and unknown values degrade to NOACCESS instead of failing. We store the same normalized
// value, which keeps queries identical to Xenia's.
uint32_t normalize_protect(uint32_t p) {
  using namespace runtime;
  const bool read = (p & (kPageReadOnly | kPageReadWrite | kPageExecuteRead | kPageExecuteReadWrite)) != 0;
  const bool write = (p & (kPageReadWrite | kPageExecuteReadWrite)) != 0;
  const uint32_t access = !read ? kPageNoAccess : (write ? kPageReadWrite : kPageReadOnly);
  return access | (p & (kPageNoCache | kPageWriteCombine));
}

uint64_t round_up(uint64_t v, uint32_t page) { return (v + page - 1) / page * page; }

template <class T>
T* deref(const GuestCall& c, GuestPtr<T> p) {
  return p.valid_range(sizeof(T)) ? p.get(c.base) : nullptr;
}

// PageHeap error -> NTSTATUS for NtAllocateVirtualMemory. Xenia returns STATUS_NO_MEMORY for every failure; we
// return STATUS_CONFLICTING_ADDRESSES for a requested base that cannot be used (the Windows/Xbox kernel status).
uint32_t alloc_status(const Error& e, bool fixed) {
  if (e.code == ErrorCode::InvalidFormat) return STATUS_INVALID_PARAMETER;
  return fixed ? STATUS_CONFLICTING_ADDRESSES : STATUS_NO_MEMORY;
}

}  // namespace

namespace exports {

// NtAllocateVirtualMemory(base*, size*, allocation_type, protect, debug_memory). *base == 0 places the range
// (bottom-up, MEM_TOP_DOWN top-down) in the 4 KiB heap, or the 64 KiB heap with MEM_LARGE_PAGES; otherwise the
// heap containing *base decides the page size. MEM_COMMIT without MEM_RESERVE on free pages reserves too (Xenia's
// AllocFixed). Writes back the page-aligned base and size.
uint32_t NtAllocateVirtualMemory(GuestCall& c, U32Ptr base_ptr, U32Ptr size_ptr, uint32_t type, uint32_t protect,
                                 uint32_t /*debug_memory*/) {
  be<uint32_t>* const bp = deref(c, base_ptr);
  be<uint32_t>* const sp = deref(c, size_ptr);
  if (bp == nullptr || sp == nullptr) return STATUS_INVALID_PARAMETER;
  uint32_t size = *sp;
  if (static_cast<int32_t>(size) < 0) size = 0u - size;  // Xenia negates a negative size
  if (size == 0) return STATUS_INVALID_PARAMETER;
  if (type & MEM_RESET) {
    // Xenia rejects MEM_RESET combined with anything and leaves it alone unimplemented; alone it only says the
    // contents may be discarded, so keeping them is a valid implementation.
    return type == MEM_RESET ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
  }
  if ((type & (MEM_COMMIT | MEM_RESERVE)) == 0) return STATUS_INVALID_PARAMETER;

  Kernel& k = kernel();
  const uint32_t requested = *bp;
  const bool fixed = requested != 0;
  PageHeap* heap = nullptr;
  if (fixed) {
    heap = k.virtual_heap_for(requested);
    if (heap == nullptr) return STATUS_INVALID_PARAMETER;  // as Xenia: only the virtual heaps
  } else {
    heap = (type & MEM_LARGE_PAGES) ? &k.heap64k() : &k.heap4k();
  }
  const uint32_t page = heap->page_size();
  const uint64_t start = fixed ? requested / page * page : 0;
  const uint64_t end = round_up(static_cast<uint64_t>(fixed ? requested : 0) + size, page);
  const uint64_t span = end - start;
  if (span > static_cast<uint64_t>(heap->end()) - heap->base()) return fixed ? STATUS_CONFLICTING_ADDRESSES : STATUS_NO_MEMORY;
  const uint32_t prot = normalize_protect(protect);

  uint32_t base = static_cast<uint32_t>(start);
  bool reserved_here = false;
  if (!fixed) {
    auto r = heap->reserve(static_cast<uint32_t>(span), 0, (type & MEM_TOP_DOWN) != 0, std::nullopt, prot);
    if (!r) return alloc_status(r.error(), false);
    base = r.value();
    reserved_here = true;
  } else {
    bool reserve = (type & MEM_RESERVE) != 0;
    if (!reserve) {
      auto q = heap->query(base);
      reserve = q && q.value().state == RegionInfo::Free;
    }
    if (reserve) {
      auto r = heap->reserve(static_cast<uint32_t>(span), 0, false, base, prot);
      if (!r) return alloc_status(r.error(), true);
      reserved_here = true;
    }
  }
  if (type & MEM_COMMIT) {
    // Zeroing happens in the kernel's commit hook, for newly committed pages only (as Xenia).
    if (auto r = heap->commit(base, static_cast<uint32_t>(span), prot, (type & MEM_NOZERO) == 0); !r) {
      if (reserved_here) (void)heap->release(base);
      return alloc_status(r.error(), fixed);
    }
  }
  *bp = base;
  *sp = static_cast<uint32_t>(span);
  return STATUS_SUCCESS;
}

// NtFreeVirtualMemory(base*, size*, free_type, debug_memory). free_type == MEM_DECOMMIT decommits; anything else
// releases the whole allocation at *base (Xenia). Writes back the base and the decommitted/released size.
uint32_t NtFreeVirtualMemory(GuestCall& c, U32Ptr base_ptr, U32Ptr size_ptr, uint32_t free_type,
                             uint32_t /*debug_memory*/) {
  be<uint32_t>* const bp = deref(c, base_ptr);
  be<uint32_t>* const sp = deref(c, size_ptr);
  if (bp == nullptr || sp == nullptr) return STATUS_INVALID_PARAMETER;  // Xenia does not check
  const uint32_t addr = *bp;
  if (addr == 0) return STATUS_MEMORY_NOT_ALLOCATED;
  PageHeap* const heap = kernel().virtual_heap_for(addr);
  if (heap == nullptr) return STATUS_INVALID_PARAMETER;
  const uint32_t page = heap->page_size();
  const uint32_t base = addr / page * page;

  uint64_t size = 0;
  if (free_type == MEM_DECOMMIT) {
    if (*sp == 0) {
      // Windows: size 0 decommits up to the end of the allocation (Xenia asserts a non-zero size).
      auto q = heap->query(base);
      auto total = heap->allocation_size(base);
      if (!q || !total) return STATUS_UNSUCCESSFUL;
      size = static_cast<uint64_t>(q.value().allocation_base) + total.value() - base;
    } else {
      size = round_up(static_cast<uint64_t>(addr) + *sp, page) - base;
    }
    if (!heap->decommit(base, static_cast<uint32_t>(size))) return STATUS_UNSUCCESSFUL;
  } else {
    auto total = heap->allocation_size(base);
    if (!total) return STATUS_UNSUCCESSFUL;
    if (!heap->release(base)) return STATUS_UNSUCCESSFUL;  // not an allocation base
    size = total.value();
  }
  *bp = base;
  *sp = static_cast<uint32_t>(size);
  return STATUS_SUCCESS;
}

// NtProtectVirtualMemory(base*, size*, protect, old_protect*, debug_memory).
uint32_t NtProtectVirtualMemory(GuestCall& c, U32Ptr base_ptr, U32Ptr size_ptr, uint32_t protect, U32Ptr old_ptr,
                                uint32_t /*debug_memory*/) {
  be<uint32_t>* const bp = deref(c, base_ptr);
  be<uint32_t>* const sp = deref(c, size_ptr);
  if (bp == nullptr || sp == nullptr || *sp == 0) return STATUS_INVALID_PARAMETER;
  if (protect & kExecuteBits) return STATUS_ACCESS_DENIED;  // Xenia refuses executable protection here
  const uint32_t addr = *bp;
  PageHeap* const heap = kernel().virtual_heap_for(addr);
  if (heap == nullptr) return STATUS_INVALID_PARAMETER;
  const uint32_t page = heap->page_size();
  const uint32_t base = addr / page * page;
  const uint64_t size = round_up(static_cast<uint64_t>(addr) + *sp, page) - base;
  uint32_t old = 0;
  if (!heap->protect(base, static_cast<uint32_t>(size), normalize_protect(protect), &old)) {
    return STATUS_ACCESS_DENIED;  // uncommitted pages or a range spanning allocations, as Xenia
  }
  *bp = base;
  *sp = static_cast<uint32_t>(size);
  // Xenia writes its internal protection format here (a bug); we write the Xbox PAGE_* value.
  if (be<uint32_t>* const op = deref(c, old_ptr)) *op = old;
  return STATUS_SUCCESS;
}

// NtQueryVirtualMemory(address, MEMORY_BASIC_INFORMATION*). Only the virtual heaps are queried in M2; physical
// views and the image region return STATUS_INVALID_PARAMETER.
uint32_t NtQueryVirtualMemory(GuestCall& c, uint32_t addr, GuestPtr<X_MEMORY_BASIC_INFORMATION> out) {
  X_MEMORY_BASIC_INFORMATION* const m = deref(c, out);
  if (m == nullptr) return STATUS_INVALID_PARAMETER;
  PageHeap* const heap = kernel().virtual_heap_for(addr);
  if (heap == nullptr) return STATUS_INVALID_PARAMETER;
  auto q = heap->query(addr);
  if (!q) return STATUS_INVALID_PARAMETER;
  const RegionInfo& r = q.value();
  m->base_address = r.base;
  m->allocation_base = r.allocation_base;  // 0 for free pages (Xenia reports a heap-relative offset there)
  m->allocation_protect = r.allocation_protect;
  m->region_size = r.size;
  m->state = r.state == RegionInfo::Committed ? MEM_COMMIT : r.state == RegionInfo::Reserved ? MEM_RESERVE : MEM_FREE;
  m->protect = r.protect;
  m->type = MEM_PRIVATE;  // always, as Xenia
  return STATUS_SUCCESS;
}

// MmAllocatePhysicalMemoryEx(flags, size, protect, min_physical, max_physical, alignment) -> guest address in the
// view chosen by the page-size bits of `protect`, or 0. The block is zeroed (reused physical pages would
// otherwise keep old data; fresh ones are untouched by the madvise-based zeroing).
uint32_t MmAllocatePhysicalMemoryEx(uint32_t /*flags*/, uint32_t size, uint32_t protect, uint32_t min_addr,
                                    uint32_t max_addr, uint32_t alignment) {
  Kernel& k = kernel();
  auto r = k.physical().allocate(size, protect, min_addr, max_addr, alignment);
  if (!r) return 0;
  auto total = k.physical().allocation_size(r.value());
  if (total) (void)k.memory().zero(r.value(), total.value());
  return r.value();
}

// MmFreePhysicalMemory(type, address): `address` must be what MmAllocatePhysicalMemoryEx returned; other values
// are ignored, as Xenia ignores the release result.
void MmFreePhysicalMemory(uint32_t /*type*/, uint32_t addr) { (void)kernel().physical().free(addr); }

// MmGetPhysicalAddress(address): physical views only (E0 adds 0x1000, as Xenia); 0 for anything else (Xenia
// returns 0 after an assert for non-physical heaps).
uint32_t MmGetPhysicalAddress(uint32_t addr) {
  if (addr < runtime::kPhysicalViewA0) return 0;
  return runtime::PhysicalHeap::physical_address(addr);
}

// MmQueryAddressProtect(address): the PAGE_* of a committed page, 0 when not committed. Physical allocations and
// the kernel data/image region (0x80000000-0x9FFFFFFF, never handed out by a heap) report PAGE_READWRITE: M2
// does not track their protection.
uint32_t MmQueryAddressProtect(uint32_t addr) {
  Kernel& k = kernel();
  if (PageHeap* heap = k.virtual_heap_for(addr)) {
    auto q = heap->query(addr);
    return q && q.value().state == RegionInfo::Committed ? q.value().protect : 0;
  }
  if (addr >= runtime::kPhysicalViewA0) return k.physical().allocation_size(addr) ? runtime::kPageReadWrite : 0;
  if (addr >= runtime::kKernelDataBase) return runtime::kPageReadWrite;
  return 0;
}

// MmSetAddressProtect(address, size, protect): as Xenia, protect 0 is ignored and failures are silent.
void MmSetAddressProtect(uint32_t addr, uint32_t size, uint32_t protect) {
  if (protect == 0) return;
  if (PageHeap* heap = kernel().virtual_heap_for(addr)) (void)heap->protect(addr, size, normalize_protect(protect), nullptr);
}

// MmQueryAllocationSize(address): size of the allocation containing address, 0 if none.
uint32_t MmQueryAllocationSize(uint32_t addr) {
  Kernel& k = kernel();
  if (PageHeap* heap = k.virtual_heap_for(addr)) {
    auto r = heap->allocation_size(addr);
    return r ? r.value() : 0;
  }
  if (addr >= runtime::kPhysicalViewA0) {
    auto r = k.physical().allocation_size(addr);
    return r ? r.value() : 0;
  }
  return 0;
}

// MmQueryStatistics(result*): the caller sets result->size = 104. Constants are Xenia's; used pages come from the
// physical heap.
uint32_t MmQueryStatistics(GuestCall& c, GuestPtr<X_MM_QUERY_STATISTICS_RESULT> out) {
  X_MM_QUERY_STATISTICS_RESULT* const s = deref(c, out);
  if (s == nullptr) return STATUS_INVALID_PARAMETER;
  if (s->size != sizeof(X_MM_QUERY_STATISTICS_RESULT)) return STATUS_BUFFER_TOO_SMALL;
  constexpr uint32_t kTotalPages = 0x00020000;
  std::memset(static_cast<void*>(s), 0, sizeof *s);
  s->size = sizeof(X_MM_QUERY_STATISTICS_RESULT);
  s->total_physical_pages = kTotalPages;
  s->kernel_pages = 0x300;
  const uint32_t used = kernel().physical().used_pages();
  s->title.available_pages = used < kTotalPages ? kTotalPages - used : 0;
  s->title.total_virtual_memory_bytes = 0x2FFF0000;
  s->title.reserved_virtual_memory_bytes = 0x00160000;
  s->title.physical_pages = 0x1000;
  s->title.pool_pages = 0x10;
  s->title.stack_pages = 0x100;
  s->title.image_pages = 0x100;
  s->title.heap_pages = 0x100;
  s->title.virtual_pages = 0x100;
  s->title.page_table_pages = 0x100;
  s->title.cache_pages = 0x100;
  s->highest_physical_page = 0x0001FFFF;
  return STATUS_SUCCESS;
}

// Pool: Xenia allocates from its 4 KiB virtual heap (SystemHeapAlloc); so do we, page-granular and zeroed.
uint32_t ExAllocatePoolTypeWithTag(uint32_t size, uint32_t tag, uint32_t /*zero*/) {
  return kernel().pool_alloc(size, tag);
}
uint32_t ExAllocatePool(uint32_t size) { return kernel().pool_alloc(size, kPoolTagNone); }
void ExFreePool(uint32_t addr) { kernel().pool_free(addr); }

// XamAlloc(flags, size, out*) -> HRESULT, writing the block to *out (Xenia: SystemHeapAlloc, always S_OK; we
// report E_OUTOFMEMORY when the heap is exhausted).
uint32_t XamAlloc(GuestCall& c, uint32_t /*flags*/, uint32_t size, U32Ptr out_ptr) {
  be<uint32_t>* const out = deref(c, out_ptr);
  const uint32_t a = kernel().pool_alloc(size, 0);
  if (out != nullptr) *out = a;
  return a != 0 ? 0 : E_OUTOFMEMORY;
}
// XamAllocEx(physical_flags, flags, size, out*): not in Xenia; assumed to be XamAlloc with an extra leading
// argument (XamAlloc forwards to it with 0). The physical flags are ignored: the block comes from the pool.
uint32_t XamAllocEx(GuestCall& c, uint32_t /*physical_flags*/, uint32_t flags, uint32_t size, U32Ptr out_ptr) {
  return XamAlloc(c, flags, size, out_ptr);
}
uint32_t XamFree(uint32_t addr) {
  kernel().pool_free(addr);
  return 0;
}

// MmMapIoSpace(unk0, address, size, flags): the address itself, as Xenia (which has only seen it map XMA contexts).
// Our I/O ranges already live at their guest addresses (the GPU registers in the MMIO window, runtime/gpu/mmio.h,
// committed read/write), so the identity mapping is also right for the GPU register range.
uint32_t MmMapIoSpace(uint32_t /*unk0*/, uint32_t address, uint32_t /*size*/, uint32_t /*flags*/) { return address; }

}  // namespace exports
}  // namespace lr::kernel

LR_KERNEL(NtAllocateVirtualMemory);
LR_KERNEL(NtFreeVirtualMemory);
LR_KERNEL(NtProtectVirtualMemory);
LR_KERNEL(NtQueryVirtualMemory);
LR_KERNEL(MmAllocatePhysicalMemoryEx);
LR_KERNEL(MmFreePhysicalMemory);
LR_KERNEL(MmGetPhysicalAddress);
LR_KERNEL(MmMapIoSpace);
LR_KERNEL(MmQueryAddressProtect);
LR_KERNEL(MmSetAddressProtect);
LR_KERNEL(MmQueryAllocationSize);
LR_KERNEL(MmQueryStatistics);
LR_KERNEL(ExAllocatePool);
LR_KERNEL(ExAllocatePoolTypeWithTag);
LR_KERNEL(ExFreePool);
LR_KERNEL(XamAlloc);
LR_KERNEL(XamAllocEx);
LR_KERNEL(XamFree);
