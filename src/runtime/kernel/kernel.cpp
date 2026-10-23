// SPDX-License-Identifier: MIT
#include "runtime/kernel/kernel.h"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>

#include "runtime/guest_thread.h"
#include "runtime/gpu/gpu_system.h"
#include "runtime/gpu/mmio.h"
#include "runtime/gpu/pm4.h"
#include "runtime/kernel/guest_structs.h"
#include "runtime/kernel/glue.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/threads.h"
#include "runtime/kernel/trace.h"
#include "runtime/memory_map.h"

namespace lr::kernel {

namespace {
// Set once by init_kernel and never deleted (only reset_kernel_for_tests deletes it): see init_kernel.
std::atomic<Kernel*> g_kernel{nullptr};

// Flush hook for the GPU trace (set_trace_flush_hook): every exit path that flushes the import trace flushes it too.
void flush_gpu_trace() {
  if (Kernel* k = g_kernel.load(std::memory_order_acquire)) k->gpu().flush_trace();
}
}  // namespace

Kernel::Kernel(runtime::GuestMemory& mem, KernelConfig cfg)
    : mem_(mem),
      cfg_(std::move(cfg)),
      data_next_(runtime::kKernelDataBase),
      heap4k_(runtime::kHeap4kBase, runtime::kHeap4kEnd, 0x1000),
      heap64k_(runtime::kHeap64kBase, runtime::kHeap64kEnd, 0x10000),
      stacks_(runtime::kStacksBase, runtime::kStacksEnd, 0x1000),
      timers_(*this),
      dispatcher_(mem.base()),
      deadlock_(*this) {
  vfs_.mount_defaults(cfg_.game_dir, cfg_.user_dir);
  gpu::GpuHooks hooks;
  hooks.queue_interrupt = [this](uint32_t callback, uint32_t source, uint32_t user_data, uint32_t cpu,
                                 std::function<void()> done) {
    timers_.queue_call([this, callback, source, user_data, cpu, done = std::move(done)] {
      // The callback runs "on" the interrupt's CPU: the worker's KPCR reports it for the call (D3D's callback
      // indexes per-CPU state with it), then gets its own processor back.
      X_KPCR* pcr = nullptr;
      if (const GuestThread* t = current_thread()) pcr = mem_.translate<X_KPCR>(t->pcr());
      const uint8_t saved = pcr ? pcr->current_cpu : 0;
      if (pcr) pcr->current_cpu = static_cast<uint8_t>(cpu);
      const uint64_t args[] = {source, user_data};
      (void)call_guest(callback, args);
      if (pcr) pcr->current_cpu = saved;
      done();
    });
  };
  hooks.alloc_guest = [this](uint32_t bytes) { return pool_alloc(bytes, 0x47505520 /* "GPU " */); };
  hooks.fatal = [](const std::string& what) { guest_fatal(what); };
  if (const uint64_t limit = cfg_.exit_after_swaps; limit != 0) {
    hooks.on_swap = [limit](uint64_t swaps) {
      if (swaps < limit) return;
      std::fprintf(stderr, "[gpu] exit after %llu swaps\n", static_cast<unsigned long long>(swaps));
      std::fflush(nullptr);
      flush_at_exit();  // also the GPU trace (flush hook)
      _exit(runtime::kExitOk);
    };
  }
  gpu_ = std::make_unique<gpu::GpuSystem>(gpu_memory_access(), std::move(hooks));
  gpu_->set_vblank_mode(cfg_.vblank_paced_by_swap ? gpu::VblankMode::PacedBySwap : gpu::VblankMode::Realtime60Hz);
}

Kernel::~Kernel() {
  gpu_->stop();
  deadlock_.stop();
  timers_.shutdown();
}

Result<void> Kernel::setup_memory() {
  if (mem_.size() < (uint64_t{1} << 32))
    return Error{ErrorCode::OutOfRange, "guest memory must span the whole 4 GiB guest space", ""};
  // The null guard catches guest null-pointer accesses (reported by the fault handler as "at guest 0x0000....").
  LR_TRY(mem_.protect(0, runtime::kNullGuardEnd, PROT_NONE));
  if (cfg_.strict_memory) {
    LR_TRY(mem_.protect(runtime::kHeap4kBase, runtime::kHeap4kEnd - runtime::kHeap4kBase, PROT_NONE));
    LR_TRY(mem_.protect(runtime::kHeap64kBase, runtime::kHeap64kEnd - runtime::kHeap64kBase, PROT_NONE));
    LR_TRY(mem_.protect(runtime::kStacksBase, runtime::kStacksEnd - runtime::kStacksBase, PROT_NONE));
  }
  auto hook = [this](uint32_t addr, uint32_t size, bool committed, bool zero) { on_commit(addr, size, committed, zero); };
  heap4k_.set_commit_hook(hook);
  heap64k_.set_commit_hook(hook);
  stacks_.set_commit_hook(hook);
  LR_TRY(gpu::init_mmio_window(mem_));
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

gpu::MemoryAccess Kernel::gpu_memory_access() {
  return gpu::MemoryAccess{mem_.base(), [this](uint32_t phys) { return physical_.guest_from_physical(phys); },
                           mem_.size(),
                           [this](uint32_t phys) { return physical_.physical_allocation_end(phys); }};
}

runtime::PageHeap* Kernel::virtual_heap_for(uint32_t addr) {
  if (addr >= runtime::kHeap4kBase && addr < runtime::kHeap4kEnd) return &heap4k_;
  if (addr >= runtime::kHeap64kBase && addr < runtime::kHeap64kEnd) return &heap64k_;
  if (addr >= runtime::kStacksBase && addr < runtime::kStacksEnd) return &stacks_;
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
  if (g_kernel.load(std::memory_order_acquire) != nullptr)
    return Error{ErrorCode::Internal, "init_kernel called twice", "the kernel is initialized once per process"};
  if (cfg.trace_imports) LR_TRY(enable_import_trace(cfg.trace_file));
  const bool trace_gpu = cfg.trace_gpu;
  const std::optional<std::filesystem::path> gpu_trace_file = cfg.gpu_trace_file;
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
  if (trace_gpu) {
    if (auto r = k->gpu().enable_trace(gpu_trace_file); !r) {
      g_kernel.store(nullptr, std::memory_order_release);
      delete k;
      return r;
    }
    set_trace_flush_hook(&flush_gpu_trace);
  }
  install_thread_hooks(*k);
  // Spec section 10: the TSC rate KeQueryPerformanceFrequency reports is measured at init (once per process).
  k->timers().set_tsc_hz(process_tsc_hz());
  return {};
}


Kernel& kernel() {
  Kernel* k = g_kernel.load(std::memory_order_acquire);
  if (k == nullptr) guest_fatal("kernel used before init_kernel");
  return *k;
}

void reset_kernel_for_tests() {
  wait_for_guest_threads_for_tests();
  // The worker may still be running guest DPC code, which needs kernel(): stop it before the instance goes away.
  if (Kernel* k = g_kernel.load(std::memory_order_acquire)) {
    k->gpu().stop();  // no new interrupts for the worker
    k->timers().shutdown();
  }
  set_trace_flush_hook(nullptr);
  delete g_kernel.exchange(nullptr, std::memory_order_acq_rel);
  disable_import_trace();
}

void guest_fatal(std::string_view what) {
  const std::string msg = "Guest fatal: " + std::string(what) + "\n" + format_trail(current_trail());
  std::fputs(msg.c_str(), stderr);
  std::fflush(nullptr);
  flush_at_exit();
  _exit(runtime::kExitGuestFatal);
}

}  // namespace lr::kernel

namespace lr::kernel {

uint32_t Kernel::system_module(std::string_view name) {
  if (name != "xboxkrnl.exe" && name != "xam.xex") return 0;
  std::lock_guard<std::mutex> lock(modules_mutex_);
  const auto it = system_modules_.find(name);
  if (it != system_modules_.end()) return it->second;
  const uint32_t ldr = kernel_data_alloc(sizeof(X_LDR_DATA_TABLE_ENTRY), 0x10);
  if (ldr == 0) return 0;
  auto* e = mem_.translate<X_LDR_DATA_TABLE_ENTRY>(ldr);
  e->in_load_order_links = {ldr, ldr};
  e->in_memory_order_links = {ldr + 0x08, ldr + 0x08};
  e->in_initialization_order_links = {ldr + 0x10, ldr + 0x10};
  e->load_count = 1;
  system_modules_.emplace(std::string(name), ldr);
  return ldr;
}

std::string Kernel::system_module_name(uint32_t handle) {
  std::lock_guard<std::mutex> lock(modules_mutex_);
  for (const auto& [n, h] : system_modules_)
    if (h == handle) return n;
  return {};
}

}  // namespace lr::kernel
