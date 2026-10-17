// SPDX-License-Identifier: MIT
#include "runtime/kernel/kernel.h"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <optional>
#include <string>

#include "runtime/guest_thread.h"
#include "runtime/kernel/registry.h"
#include "runtime/memory_map.h"

namespace lr::kernel {

namespace {
// Set once by init_kernel and never deleted (only reset_kernel_for_tests deletes it): see init_kernel.
std::atomic<Kernel*> g_kernel{nullptr};
}  // namespace

Kernel::Kernel(runtime::GuestMemory& mem, KernelConfig cfg)
    : mem_(mem),
      cfg_(std::move(cfg)),
      data_next_(runtime::kKernelDataBase),
      heap4k_(runtime::kHeap4kBase, runtime::kHeap4kEnd, 0x1000),
      heap64k_(runtime::kHeap64kBase, runtime::kHeap64kEnd, 0x10000) {}

Result<void> Kernel::setup_memory() {
  if (mem_.size() < (uint64_t{1} << 32))
    return Error{ErrorCode::OutOfRange, "guest memory must span the whole 4 GiB guest space", ""};
  // The null guard catches guest null-pointer accesses (reported by the fault handler as "at guest 0x0000....").
  LR_TRY(mem_.protect(0, runtime::kNullGuardEnd, PROT_NONE));
  if (cfg_.strict_memory) {
    LR_TRY(mem_.protect(runtime::kHeap4kBase, runtime::kHeap4kEnd - runtime::kHeap4kBase, PROT_NONE));
    LR_TRY(mem_.protect(runtime::kHeap64kBase, runtime::kHeap64kEnd - runtime::kHeap64kBase, PROT_NONE));
  }
  auto hook = [this](uint32_t addr, uint32_t size, bool committed, bool zero) { on_commit(addr, size, committed, zero); };
  heap4k_.set_commit_hook(hook);
  heap64k_.set_commit_hook(hook);
  return {};
}

// Runs under the heap's operation mutex (PageHeap::set_commit_hook), so it never races a later operation on the
// same pages. Released/decommitted pages keep their contents unless strict: the next commit zeroes them anyway.
void Kernel::on_commit(uint32_t addr, uint32_t size, bool committed, bool zero) {
  Result<void> r;
  if (committed) {
    if (cfg_.strict_memory) r = mem_.protect(addr, size, PROT_READ | PROT_WRITE);
    if (r && zero) r = mem_.zero(addr, size);
  } else if (cfg_.strict_memory) {
    r = mem_.protect(addr, size, PROT_NONE);
  }
  if (!r) guest_fatal("guest memory commit hook failed: " + r.error().message);
}

runtime::PageHeap* Kernel::virtual_heap_for(uint32_t addr) {
  if (addr >= runtime::kHeap4kBase && addr < runtime::kHeap4kEnd) return &heap4k_;
  if (addr >= runtime::kHeap64kBase && addr < runtime::kHeap64kEnd) return &heap64k_;
  return nullptr;
}

uint32_t Kernel::pool_alloc(uint32_t bytes, uint32_t /*tag*/) {
  auto base = heap4k_.reserve(bytes == 0 ? 1 : bytes, 0, false, std::nullopt, runtime::kPageReadWrite);
  if (!base) return 0;
  if (!heap4k_.commit(base.value(), bytes == 0 ? 1 : bytes, runtime::kPageReadWrite, true)) {
    (void)heap4k_.release(base.value());
    return 0;
  }
  return base.value();
}

void Kernel::pool_free(uint32_t addr) { (void)heap4k_.release(addr); }

uint32_t Kernel::kernel_data_alloc(uint32_t size, uint32_t align) {
  if (align == 0 || (align & (align - 1)) != 0) return 0;
  std::lock_guard<std::mutex> lock(data_mutex_);
  const uint64_t start = (uint64_t{data_next_} + align - 1) & ~uint64_t{align - 1};
  const uint64_t end = start + (size == 0 ? 1 : size);
  if (end > runtime::kKernelDataEnd) return 0;
  data_next_ = static_cast<uint32_t>(end);
  return static_cast<uint32_t>(start);
}

Result<void> init_kernel(runtime::GuestMemory& mem, KernelConfig cfg) {
  auto* k = new Kernel(mem, std::move(cfg));
  Kernel* expected = nullptr;
  if (!g_kernel.compare_exchange_strong(expected, k, std::memory_order_acq_rel)) {
    delete k;
    return Error{ErrorCode::Internal, "init_kernel called twice", "the kernel is initialized once per process"};
  }
  if (auto r = k->setup_memory(); !r) {
    g_kernel.store(nullptr, std::memory_order_release);
    delete k;
    return r;
  }
  return {};
}


Kernel& kernel() {
  Kernel* k = g_kernel.load(std::memory_order_acquire);
  if (k == nullptr) guest_fatal("kernel used before init_kernel");
  return *k;
}

void reset_kernel_for_tests() { delete g_kernel.exchange(nullptr, std::memory_order_acq_rel); }

void guest_fatal(std::string_view what) {
  const std::string msg = "Guest fatal: " + std::string(what) + "\n" + format_trail(current_trail());
  std::fputs(msg.c_str(), stderr);
  std::fflush(nullptr);
  _exit(runtime::kExitGuestFatal);
}

}  // namespace lr::kernel
