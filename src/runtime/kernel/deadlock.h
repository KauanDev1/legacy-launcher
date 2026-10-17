// SPDX-License-Identifier: MIT
#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>
#include <string>
#include <thread>

#include "runtime/kernel/dispatcher.h"

// Deadlock detector (spec section 8). A small host thread (not a guest thread), started with the first guest thread
// when KernelConfig::deadlock_seconds > 0 (main.cpp: 10 s, or the LR_DEADLOCK_SECONDS environment variable), checks
// once per second. The process is deadlocked when all_threads_blocked holds and the dispatcher's activity() counter
// has not moved for at least deadlock_seconds (no thread entered or left a wait in between). It then prints, to
// stderr,
//
//   Deadlock: every guest thread is blocked in an infinite wait (no timer, DPC or APC pending) for N s
//   Thread <id>: waiting on 0x<header> (<kind>)[, 0x<header> (<kind>)... (any|all)]
//   Trail (last K): ...                                   (that thread's import trail)
//
// for every live guest thread (ascending id), flushes stdio and the import trace and calls _exit(kExitDeadlock) = 6.
// Not counted: the kernel worker (internal, never a dispatcher waiter) and host threads (run_main_thread's wait).

namespace lr::kernel {

class Kernel;

// The deadlock rule over one observation: true when `live_tokens` (the dispatcher tokens, i.e. KTHREAD addresses,
// of the live guest threads) is non-empty, nothing is pending (`pending_work`: an armed timer or queued/running
// DPC), and every live token has a wait in `waits` that is infinite and not (alertable with an APC pending). A live
// thread without a wait (running guest code, suspended, starting) means no deadlock. Waits of other tokens are
// ignored.
bool all_threads_blocked(const std::vector<uint32_t>& live_tokens, const std::vector<WaitInfo>& waits, bool pending_work);

class DeadlockDetector {
 public:
  explicit DeadlockDetector(Kernel& k) : kernel_(k) {}
  ~DeadlockDetector() { stop(); }
  DeadlockDetector(const DeadlockDetector&) = delete;
  DeadlockDetector& operator=(const DeadlockDetector&) = delete;

  // Called by create_guest_thread: starts the watcher once, when kernel().config().deadlock_seconds > 0.
  void on_guest_thread_created();
  // Stops and joins the watcher (Kernel's destructor). Idempotent.
  void stop();

 private:
  void loop(uint32_t seconds);
  // The report for the current state (see above).
  std::string report(uint32_t seconds) const;

  Kernel& kernel_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool started_ = false;
  bool stop_ = false;
  std::thread thread_;
};

}  // namespace lr::kernel
