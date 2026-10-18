// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "common/result.h"
#include "runtime/guest_memory.h"
#include "runtime/kernel/deadlock.h"
#include "runtime/kernel/dispatcher.h"
#include "runtime/kernel/objects.h"
#include "runtime/kernel/threads.h"
#include "runtime/kernel/timers.h"
#include "runtime/kernel/vfs.h"
#include "runtime/memory/page_heap.h"
#include "runtime/memory/physical_heap.h"

// Process-wide kernel state. Every kernel subsystem (allocators, objects and handles, threads, files, ...) hangs
// off the single Kernel instance created by init_kernel; later tasks add them as members.

namespace lr::kernel {

struct KernelConfig {
  std::filesystem::path game_dir, user_dir;
  std::string language;
  bool strict_memory = false;
  // --trace-imports: init_kernel enables the import trace (runtime/kernel/trace.h) to trace_file, or stderr without
  // one. trace_file alone enables nothing.
  bool trace_imports = false;
  std::optional<std::filesystem::path> trace_file;
  // Deadlock detector threshold in seconds (runtime/kernel/deadlock.h); 0 = no detector (the default; main.cpp
  // sets 10 or LR_DEADLOCK_SECONDS).
  uint32_t deadlock_seconds = 0;
};

class Kernel {
 public:
  Kernel(runtime::GuestMemory& mem, KernelConfig cfg);
  // Stops the deadlock detector and the timer/DPC worker first (Timers::shutdown), while everything they use still
  // exists.
  ~Kernel();
  Kernel(const Kernel&) = delete;
  Kernel& operator=(const Kernel&) = delete;

  runtime::GuestMemory& memory() { return mem_; }
  const KernelConfig& config() const { return cfg_; }

  // Guest allocators (spec section 6.2, memory map in runtime/memory_map.h). Committing pages in either virtual
  // heap zeroes them unless MEM_NOZERO; with config().strict_memory, pages that are not committed are PROT_NONE.
  runtime::PageHeap& heap4k() { return heap4k_; }
  runtime::PageHeap& heap64k() { return heap64k_; }
  runtime::PhysicalHeap& physical() { return physical_; }
  // Guest thread stacks [kStacksBase, kStacksEnd), 4 KiB pages (runtime/kernel/threads.h).
  runtime::PageHeap& stacks() { return stacks_; }
  // The virtual heap containing addr (the stacks heap included), or nullptr.
  runtime::PageHeap* virtual_heap_for(uint32_t addr);

  // Kernel pool (ExAllocatePool*, XamAlloc): page-granular, committed read/write and zeroed, in the 4 KiB heap.
  // `tag` is informational. Returns 0 when out of memory. pool_free ignores addresses it did not hand out.
  uint32_t pool_alloc(uint32_t bytes, uint32_t tag);
  void pool_free(uint32_t addr);

  // Called once by init_kernel: null guard, strict-memory protections and the heaps' commit hooks.
  Result<void> setup_memory();

  // Bump allocator over the kernel data area [kKernelDataBase, kKernelDataEnd) (guest read/write, never handed out
  // by a heap): the XEX header copy (always the first allocation, so it sits at kKernelDataBase), the LDR entry,
  // kernel variable blocks and missing-variable guard pages. `align` is a power of two. Memory is never freed and
  // starts zeroed (fresh mapping). Returns 0 when the area is exhausted. Thread-safe.
  uint32_t kernel_data_alloc(uint32_t size, uint32_t align);

  // The executable's LDR_DATA_TABLE_ENTRY (its module handle) and XEX header copy; set by resolve_variable_imports.
  uint32_t executable_module() const { return executable_module_; }
  uint32_t executable_header() const { return executable_header_; }
  void set_executable_module(uint32_t ldr_entry, uint32_t header) {
    executable_module_ = ldr_entry;
    executable_header_ = header;
  }

  // Handle table and kernel objects (runtime/kernel/objects.h).
  ObjectTable& objects() { return objects_; }

  // The wait engine (runtime/kernel/dispatcher.h) over this kernel's guest memory.
  Dispatcher& dispatcher() { return dispatcher_; }

  // Guest thread state (runtime/kernel/threads.h).
  Threads& threads() { return threads_; }

  // Clocks, timers, DPCs and the kernel worker thread (runtime/kernel/timers.h).
  Timers& timers() { return timers_; }

  // Guest path layer (runtime/kernel/vfs.h), mounted from config(): game_dir read-only, user_dir writable.
  Vfs& vfs() { return vfs_; }

  // The deadlock detector (runtime/kernel/deadlock.h).
  DeadlockDetector& deadlock() { return deadlock_; }

  // Kernel variable name -> guest address of its block (filled by resolve_variable_imports).
  std::map<std::string, uint32_t, std::less<>>& variable_blocks() { return variable_blocks_; }

 private:
  void on_commit(uint32_t addr, uint32_t size, bool committed, bool zero);

  runtime::GuestMemory& mem_;
  KernelConfig cfg_;
  std::mutex data_mutex_;
  uint32_t data_next_;
  uint32_t executable_module_ = 0;
  uint32_t executable_header_ = 0;
  std::map<std::string, uint32_t, std::less<>> variable_blocks_;
  runtime::PageHeap heap4k_;
  runtime::PageHeap heap64k_;
  runtime::PhysicalHeap physical_;
  runtime::PageHeap stacks_;
  Threads threads_;
  // Destroyed after the objects (a timer object disarms itself here) and before the heaps (the worker's guest
  // memory); the worker itself is stopped by ~Kernel before any member goes away.
  Timers timers_;
  // After the heaps: objects are destroyed first, so objects backed by pool memory can still free it.
  ObjectTable objects_;
  Dispatcher dispatcher_;
  Vfs vfs_;
  // Last: its watcher reads the members above; ~Kernel stops it first anyway.
  DeadlockDetector deadlock_;
};

// Creates the process-wide Kernel over `mem`, which must outlive all guest code and span the whole 4 GiB guest
// space. It makes the null guard [0, kNullGuardEnd) PROT_NONE and, with strict_memory, both virtual heaps too. Once per process: a second call
// returns Error{Internal}. The instance is intentionally never destroyed (leaked), so guest threads still running
// while the process exits never touch a destroyed kernel.
Result<void> init_kernel(runtime::GuestMemory& mem, KernelConfig cfg);

// The instance created by init_kernel. Calling it before init_kernel is a programming error (guest_fatal).
Kernel& kernel();

// TEST ONLY (tests/support/kernel_fixture.h): destroys the current instance, if any, so the next init_kernel
// succeeds again. It first waits for every guest host thread to finish (runtime/kernel/threads.h), so a test must
// let all the guest threads it started end. Never call it while guest code may be running.
void reset_kernel_for_tests();

// Ends the process for a condition the guest cannot recover from (KeBugCheck, an unhandled guest exception, a
// guest call to an address without a recompiled function, ...): writes "Guest fatal: <what>\n" followed by the
// calling thread's import trail (format_trail, the same line as the exit-3 report) to stderr, flushes stdio and
// calls _exit(kExitGuestFatal), i.e. exit code 5. The import trace is flushed first.
// guest_fatal: not async-signal-safe, never call from signal handlers.
[[noreturn]] void guest_fatal(std::string_view what);

}  // namespace lr::kernel
