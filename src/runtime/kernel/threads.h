// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "common/result.h"
#include "formats/xex2.h"
#include "runtime/kernel/objects.h"

// Guest threads (spec section 7). Every guest thread, the main one included, is a host thread (16 MiB stack, its
// own signal altstack) with, in guest memory: a KPCR (r13), a KTHREAD (DISPATCHER_HEADER type 6; it is the
// thread's waitable object and its KObject guest header), a TLS block and a guest stack with a PROT_NONE guard page
// below it. Layouts follow Xenia's X_KPCR / X_KTHREAD (guest_structs.h) and xthread.cc (semantic reference only):
//
//   pool block:  KTHREAD (0xAB0) | KPCR (0x2D8) | TLS: XEX TLS template (data_size, rounded up to 4) | slots
//   KPCR +0x00 tls_ptr and KTHREAD +0x68 tls_address -> start of the TLS block (the template copy), as Xenia's
//   tls_static_address; the KeTlsAlloc slots (be<uint32_t> each) follow the template, as tls_dynamic_address.
//   stack:       [guard page PROT_NONE, reserved only][stack_size bytes committed]; KPCR +0x70 / KTHREAD +0x5C =
//                top (high address), KPCR +0x74 / KTHREAD +0x60 = bottom; the thread starts with r1 = top - 0x80.
//
// A thread ends when its start routine returns (exit code = its r3, or 0 with an XAPI startup routine, as Xenia)
// or calls ExTerminateThread (GuestThreadExit). Then its KTHREAD signal_state becomes 1 and its waiters are woken;
// queued APCs are discarded. The guest memory (pool block and stack) is freed when the last reference (handles,
// kernel references, the running host thread) goes away. At the top of every guest thread: UnimplementedImport ->
// report with a "Thread: <id>" line, _exit(3) for the whole process; any other exception -> _exit(4).

namespace lr::kernel {

class Kernel;

struct ThreadParams {
  uint32_t stack_size = 0;  // 0 = the image's default stack size (Threads::default_stack_size)
  uint32_t start = 0;
  uint32_t context = 0;
  uint32_t xapi_start = 0;  // 0 = none; else called as xapi_start(start, context)
  bool suspended = false;
  uint8_t cpu_mask = 0;  // creation_flags >> 24: one bit = that CPU (0..5); 0 = round robin
};

// Thrown by ExTerminateThread and caught at the top of the guest thread, which ends with `code`. The second (and
// last) exception allowed to cross runtime internals besides UnimplementedImport (AGENTS.md, ADR 0004).
struct GuestThreadExit {
  uint32_t code;
};

// An APC queued by NtQueueApcThread: routine(context, arg1, arg2) runs on the target thread in its next alertable
// wait (or KeDelayExecutionThread).
struct GuestApc {
  uint32_t routine, context, arg1, arg2;
};

class GuestThread final : public KObject {
 public:
  struct Layout {
    uint32_t block, kthread, pcr, tls, tls_slots, tls_slot_count, stack_alloc, stack_bottom, stack_top;
  };
  GuestThread(Kernel& k, uint32_t id, const ThreadParams& p, const Layout& l, uint8_t cpu);
  ~GuestThread() override;

  uint32_t id() const { return id_; }
  uint32_t pcr() const { return layout_.pcr; }
  uint32_t kthread() const { return layout_.kthread; }
  uint32_t handle() const { return handle_; }
  uint32_t stack_top() const { return layout_.stack_top; }
  uint32_t exit_code() const { return exit_code_.load(std::memory_order_acquire); }
  bool finished() const { return finished_.load(std::memory_order_acquire); }

  // Suspend count. Both return the previous count; resume() at 0 does nothing. A suspended thread blocks before
  // its start routine runs, or (cooperatively) at its next import entry when another thread suspends it.
  uint32_t request_suspend();
  uint32_t resume();
  // Blocks the calling thread (which must be this one) while the suspend count is > 0.
  void wait_while_suspended();
  bool suspend_pending() const { return suspend_count_.load(std::memory_order_relaxed) > 0; }

  // Queues an APC and wakes the thread's alertable wait (Dispatcher::wake_for_apc).
  void queue_apc(const GuestApc& apc);
  // Runs the queued APCs on the calling thread (which must be this one) through call_guest, unless APCs are
  // disabled by KeEnterCriticalRegion.
  void deliver_apcs();
  void enter_critical_region();
  void leave_critical_region();

  int32_t priority() const { return priority_.load(); }
  int32_t set_priority(int32_t p) { return priority_.exchange(p); }
  uint32_t affinity() const { return affinity_.load(); }
  uint32_t set_affinity(uint32_t a) { return affinity_.exchange(a); }

  // Guest address of TLS slot `index`, 0 when out of range.
  uint32_t tls_slot_address(uint32_t index) const;

  // Internal to threads.cpp.
  void set_handle(uint32_t h) { handle_ = h; }
  void run(const ThreadParams& p);
  // Runs `body` as this (internal) thread on the calling host thread: see create_internal_guest_thread.
  void run_internal(void (*body)(void*), void* arg);

 private:
  void finish(uint32_t code);

  Kernel& kernel_;
  const uint32_t id_;
  const Layout layout_;
  uint32_t handle_ = 0;
  std::atomic<uint32_t> exit_code_{0};
  std::atomic<bool> finished_{false};
  std::atomic<int32_t> priority_{0};
  std::atomic<uint32_t> affinity_;
  std::atomic<int32_t> apc_disable_{0};

  mutable std::mutex mutex_;
  std::condition_variable resumed_;
  std::atomic<uint32_t> suspend_count_{0};
  std::deque<GuestApc> apcs_;
};

// Per-kernel thread state: image TLS template and default stack size, the KeTlsAlloc bitmap, thread ids, the live
// thread list, the process type and title-terminate notifications. Thread-safe.
class Threads {
 public:
  static constexpr uint32_t kMinTlsSlots = 64;
  static constexpr uint32_t kTlsOutOfIndexes = 0xFFFFFFFF;
  // Sanity caps on the XEX TLS header (a valid title is far below both).
  static constexpr uint32_t kMaxTlsSlots = 4096;
  static constexpr uint32_t kMaxTlsDataSize = 4u << 20;

  Threads();

  // From the loaded image [image_base, image_base + image_size): TLS template (XEX key 0x00020104) and default
  // stack size (0 keeps the current one). Slot count = max(64, the header's slot_count). InvalidFormat when the
  // header is unusable: slot_count > kMaxTlsSlots, data_size > kMaxTlsDataSize, raw_data_size > data_size, or raw
  // data outside the image. Internal once a guest thread has been created (the layout of existing TLS blocks is
  // fixed). Nothing is changed on error.
  Result<void> set_image(std::optional<formats::TlsInfo> tls, uint32_t default_stack_size, uint32_t image_base,
                         uint32_t image_size);
  // Called by create_guest_thread: from then on set_image fails.
  void mark_thread_created();
  std::optional<formats::TlsInfo> tls() const;
  uint32_t tls_slot_count() const;
  uint32_t default_stack_size() const;

  // KeTlsAlloc / KeTlsFree: kTlsOutOfIndexes when full; free returns false for an unallocated slot.
  uint32_t tls_alloc();
  bool tls_free(uint32_t index);

  uint32_t next_id() { return next_id_.fetch_add(1); }
  uint8_t next_cpu() { return static_cast<uint8_t>(next_cpu_.fetch_add(1) % 6); }

  void add(const std::shared_ptr<GuestThread>& t);
  void remove(const GuestThread* t);
  std::shared_ptr<GuestThread> by_id(uint32_t id) const;
  std::vector<std::shared_ptr<GuestThread>> live() const;

  uint32_t process_type() const { return process_type_.load(); }
  void set_process_type(uint32_t t) { process_type_.store(t); }

  struct TerminateNotification {
    uint32_t routine, priority;
  };
  void add_terminate_notification(TerminateNotification n);
  void remove_terminate_notification(uint32_t routine);
  std::vector<TerminateNotification> terminate_notifications() const;

 private:
  mutable std::mutex mutex_;
  std::optional<formats::TlsInfo> tls_;
  uint32_t default_stack_size_;
  std::vector<bool> tls_used_;
  std::vector<std::weak_ptr<GuestThread>> live_;
  std::vector<TerminateNotification> notifications_;
  bool threads_created_ = false;
  std::atomic<uint32_t> next_id_{1};
  std::atomic<uint32_t> next_cpu_{0};
  std::atomic<uint32_t> process_type_{1};
};

// Creates a guest thread (memory, KTHREAD/PCR/TLS, handle in Kernel::objects()) and starts its host thread.
// InsufficientResources when guest memory runs out, IoError when the host thread cannot be created.
Result<std::shared_ptr<GuestThread>> create_guest_thread(const ThreadParams& p);

// A kernel-internal guest thread (the timer/DPC worker, runtime/kernel/timers.h): a KTHREAD, KPCR and guest stack
// like any guest thread, so guest routines (DPCs) can run on it, but it is not a guest-visible thread: no handle
// and no ObjectTable entry, not in Threads::live() (so neither KeTlsAlloc, ObLookupThreadByThreadId nor the
// deadlock detector see it), no host thread of its own and not counted by wait_for_guest_threads_for_tests. Its
// TLS block follows the image layout (XEX template copy and slot count) like any thread's, and creating it freezes
// that layout like any thread creation (Threads::set_image fails afterwards: a wrong order is a load error, never a
// worker TLS block without the template). main.cpp therefore sets the image before resolve_variable_imports, which
// may start the worker. The owner runs it with
// run_internal(body, arg) on a host thread it manages (kHostStackSize stack): the calling host thread becomes this
// guest thread (current_thread(), its KTHREAD as dispatcher token, current guest frame at the top of its stack)
// while body runs. UnimplementedImport from body ends the process with exit 3 and the usual report, any other
// exception (ExTerminateThread included) with exit 4. The guest memory is freed with the last reference.
Result<std::shared_ptr<GuestThread>> create_internal_guest_thread(uint32_t stack_size);

// The guest thread running on the calling host thread; nullptr on non-guest host threads.
GuestThread* current_thread();

// Runs APCs queued for the current guest thread (no-op on other threads). Called by alertable waits that returned
// STATUS_USER_APC.
void deliver_current_apcs();

// Creates the main guest thread (start = entry, no context, the given stack size) through the same path as
// ExCreateThread and waits for it. Returns kExitOk when the main thread ends (the caller exits the process, which
// takes the other guest threads down), kExitFault when it cannot be created. Missing imports and faults end the
// process from the guest thread itself (exit 3 / 4).
int run_main_thread(uint32_t entry, uint32_t stack_size);

// Installs the kernel hooks of the thread module (init_kernel): Dispatcher::current_thread_token (current KTHREAD),
// ObjectTable's kCurrentThread, and the import entry hook (cooperative suspend).
void install_thread_hooks(Kernel& k);

// TEST ONLY (reset_kernel_for_tests): waits until every guest host thread has finished its cleanup.
void wait_for_guest_threads_for_tests();

}  // namespace lr::kernel
