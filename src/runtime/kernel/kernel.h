// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "common/result.h"
#include "runtime/guest_memory.h"

// Process-wide kernel state. Every kernel subsystem (allocators, objects and handles, threads, files, ...) hangs
// off the single Kernel instance created by init_kernel; later tasks add them as members.

namespace lr::kernel {

struct KernelConfig {
  std::filesystem::path game_dir, user_dir;
  std::string language;
  bool strict_memory = false;
  std::optional<std::filesystem::path> trace_file;
};

class Kernel {
 public:
  Kernel(runtime::GuestMemory& mem, KernelConfig cfg) : mem_(mem), cfg_(std::move(cfg)) {}
  Kernel(const Kernel&) = delete;
  Kernel& operator=(const Kernel&) = delete;

  runtime::GuestMemory& memory() { return mem_; }
  const KernelConfig& config() const { return cfg_; }

 private:
  runtime::GuestMemory& mem_;
  KernelConfig cfg_;
};

// Creates the process-wide Kernel over `mem`, which must outlive all guest code. Once per process: a second call
// returns Error{Internal}. The instance is intentionally never destroyed (leaked), so guest threads still running
// while the process exits never touch a destroyed kernel.
Result<void> init_kernel(runtime::GuestMemory& mem, KernelConfig cfg);

// The instance created by init_kernel. Calling it before init_kernel is a programming error (guest_fatal).
Kernel& kernel();

// TEST ONLY (tests/support/kernel_fixture.h): destroys the current instance, if any, so the next init_kernel
// succeeds again. Never call it while guest code may be running.
void reset_kernel_for_tests();

// Ends the process for a condition the guest cannot recover from (KeBugCheck, an unhandled guest exception, a
// guest call to an address without a recompiled function, ...): writes "Guest fatal: <what>\n" followed by the
// calling thread's import trail (format_trail, the same line as the exit-3 report) to stderr, flushes stdio and
// calls _exit(kExitGuestFatal), i.e. exit code 5.
[[noreturn]] void guest_fatal(std::string_view what);

}  // namespace lr::kernel
