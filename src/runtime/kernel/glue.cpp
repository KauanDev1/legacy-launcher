// SPDX-License-Identifier: MIT
#include "runtime/kernel/glue.h"

#include <atomic>
#include <cstdio>
#include <string>

#include "runtime/kernel/kernel.h"

namespace lr::kernel {

namespace {
constexpr uint32_t kCalleeFrameGap = 0x100;  // room left below the caller's r1 before the callee's frame

struct Frame {
  uint32_t r1 = 0, r13 = 0;
};
thread_local Frame t_frame;

// Plain function pointers (trivially destructible): safe for threads still running during process exit.
std::atomic<decltype(GuestGlue::call)> g_call{nullptr};
std::atomic<decltype(GuestGlue::has_function)> g_has_function{nullptr};

[[noreturn]] void fatal_for(const char* what, uint32_t fn) {
  char addr[16];
  std::snprintf(addr, sizeof addr, "0x%08X", fn);
  guest_fatal(std::string("call_guest: ") + what + " " + addr);
}
}  // namespace

void set_guest_glue(const GuestGlue& g) {
  g_call.store(g.call, std::memory_order_release);
  g_has_function.store(g.has_function, std::memory_order_release);
}

void set_current_frame(uint32_t r1, uint32_t r13) { t_frame = {r1, r13}; }

uint64_t call_guest(uint32_t fn, std::span<const uint64_t> args) {
  const auto call = g_call.load(std::memory_order_acquire);
  const auto has_function = g_has_function.load(std::memory_order_acquire);
  if (call == nullptr || has_function == nullptr) fatal_for("no guest glue installed, calling guest", fn);
  if (args.size() > 8) fatal_for("more than 8 arguments for guest", fn);
  if (t_frame.r1 == 0) fatal_for("no current guest frame on this thread, calling guest", fn);
  if (!has_function(fn)) fatal_for("no recompiled function at guest", fn);

  const Frame saved = t_frame;
  const uint32_t r1 = (saved.r1 - kCalleeFrameGap) & ~uint32_t{0xF};
  const uint64_t r3 = call(kernel().memory().base(), fn, args.data(), static_cast<int>(args.size()), r1, saved.r13);
  t_frame = saved;
  return r3;
}

}  // namespace lr::kernel
