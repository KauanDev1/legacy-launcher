// SPDX-License-Identifier: MIT
#include "runtime/kernel/kernel.h"

#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "runtime/guest_thread.h"
#include "runtime/kernel/registry.h"

namespace lr::kernel {

namespace {
// Set once by init_kernel and never deleted (only reset_kernel_for_tests deletes it): see init_kernel.
std::atomic<Kernel*> g_kernel{nullptr};
}  // namespace

Result<void> init_kernel(runtime::GuestMemory& mem, KernelConfig cfg) {
  auto* k = new Kernel(mem, std::move(cfg));
  Kernel* expected = nullptr;
  if (!g_kernel.compare_exchange_strong(expected, k, std::memory_order_acq_rel)) {
    delete k;
    return Error{ErrorCode::Internal, "init_kernel called twice", "the kernel is initialized once per process"};
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
