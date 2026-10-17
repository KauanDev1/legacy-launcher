// SPDX-License-Identifier: MIT
#pragma once

#include <pthread.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>

// Time, timers, DPCs and IRQL (spec sections 8 and 10).
//
// Clocks. XenonRecomp translates the PowerPC timebase read (mftb) to __rdtsc(), so the guest's performance counter
// is the host TSC: KeQueryPerformanceFrequency reports the TSC rate measured against CLOCK_MONOTONIC
// (calibrate_tsc_hz), not the console's 50 MHz. System time is the host realtime clock as a FILETIME (100 ns ticks
// since 1601-01-01 UTC).
//
// The kernel worker. One host thread per kernel runs timer expirations, DPC routines and the KeTimeStampBundle
// updates. DPC routines are guest code, so the worker is a kernel-internal guest thread
// (create_internal_guest_thread: own KTHREAD/KPCR/guest stack, no handle, not in Threads::live(), so the deadlock
// detector never counts it as a guest thread; it blocks on its own condition variable, never in the dispatcher). It
// starts lazily on the first timer set, DPC queued or KeTimeStampBundle import, runs at DISPATCH_LEVEL, and is
// stopped and joined by Kernel's destructor (only reset_kernel_for_tests destroys a kernel).
//
// Timers live in guest memory (a DISPATCHER_HEADER of type 8 notification / 9 synchronization); due time, period and
// APC are kept here, keyed by the header address. On expiry the worker sets signal_state to 1 and wakes the
// dispatcher's waiters, queues the timer's APC (routine(context, filetime low, filetime high), Xenia's XTimer) to the
// guest thread that set the timer, and re-arms a periodic timer. Locking: the timers mutex is taken before the
// dispatcher's and a guest thread's mutex, never after.

namespace lr::kernel {

class Kernel;
class GuestThread;

// Measures the TSC rate against CLOCK_MONOTONIC within a 50 ms budget: three samples of ~16 ms, each clock read
// bracketed between two __rdtsc() reads; the sample with the tightest brackets wins. Not cached.
uint64_t calibrate_tsc_hz();

// calibrate_tsc_hz() measured once per process (the invariant TSC rate does not change; every kernel, including the
// many test kernels, reuses the first measurement). Called by init_kernel, so the measurement happens at init.
uint64_t process_tsc_hz();

// Host realtime clock as a FILETIME.
int64_t host_filetime_now();

// Calendar fields of a FILETIME (proleptic Gregorian, UTC; weekday 0 = Sunday) and back. fields_to_filetime
// ignores weekday and returns nullopt for out-of-range fields (year < 1601 or > 30827, month 1..12, day within the
// month, hour < 24, minute < 60, second < 60, milliseconds < 1000).
struct TimeFields {
  int32_t year, month, day, hour, minute, second, milliseconds, weekday;
};
TimeFields filetime_to_fields(int64_t filetime);
std::optional<int64_t> fields_to_filetime(const TimeFields& f);

// IRQL of the calling thread (0 = PASSIVE_LEVEL, 2 = DISPATCH_LEVEL), per host thread and so per guest thread.
uint8_t current_irql();
void set_current_irql(uint8_t irql);

class Timers {
 public:
  explicit Timers(Kernel& k);
  ~Timers();
  Timers(const Timers&) = delete;
  Timers& operator=(const Timers&) = delete;

  // The TSC rate measured at init_kernel (set_tsc_hz, from process_tsc_hz()).
  uint64_t tsc_hz() const { return tsc_hz_.load(std::memory_order_acquire); }
  void set_tsc_hz(uint64_t hz) { tsc_hz_.store(hz, std::memory_order_release); }

  // Arms the timer at `header` (replacing a previous arming): signal_state becomes 0, it fires at `due` and then
  // every period_ms (0 = once). apc_routine != 0 queues routine(apc_context, filetime low, high) to `apc_thread`
  // (when it still runs) at each expiry. Returns the previous signal state.
  int32_t set_timer(uint32_t header, std::chrono::steady_clock::time_point due, uint32_t period_ms,
                    uint32_t apc_routine, uint32_t apc_context, std::weak_ptr<GuestThread> apc_thread);
  // Disarms the timer (the signal state is left as is). Returns whether it was armed. Once it returns, the worker
  // no longer touches `header` (safe to free the timer body).
  bool cancel_timer(uint32_t header);

  // Queues the DPC object at `dpc` (X_KDPC, arguments written to it) for the worker, which calls
  // routine(dpc, context, arg1, arg2). False when it is already queued.
  bool queue_dpc(uint32_t dpc, uint32_t arg1, uint32_t arg2);

  // The game imports KeTimeStampBundle: writes the block at `addr` now and every 1 ms from the worker.
  void enable_time_stamp_bundle(uint32_t addr);
  // Fills an X_TIME_STAMP_BUNDLE at `host` with the current times.
  void fill_time_stamp_bundle(uint8_t* host) const;

  // An armed timer or a queued/running DPC (for the deadlock detector).
  bool has_pending_work() const;

  // Stops and joins the worker (Kernel's destructor, before the dispatcher and objects go away). Idempotent.
  void shutdown();

 private:
  using Clock = std::chrono::steady_clock;
  struct Armed {
    Clock::time_point due;
    uint32_t period_ms;
    uint32_t apc_routine, apc_context;
    std::weak_ptr<GuestThread> apc_thread;
  };
  struct Dpc {
    uint32_t addr;
  };

  void ensure_worker_locked();
  static void* worker_main(void* self);
  static void worker_body(void* self);
  void loop();
  void fire_locked(uint32_t header, Armed& a, Clock::time_point now);

  Kernel& kernel_;
  const Clock::time_point start_;
  std::atomic<uint64_t> tsc_hz_{0};

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::map<uint32_t, Armed> timers_;
  std::deque<uint32_t> dpcs_;
  std::set<uint32_t> queued_dpcs_;
  bool dpc_running_ = false;
  uint32_t bundle_ = 0;
  bool stop_ = false;
  bool started_ = false;
  pthread_t worker_{};
  std::shared_ptr<GuestThread> worker_thread_;
};

}  // namespace lr::kernel
