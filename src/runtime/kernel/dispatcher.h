// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include "runtime/be.h"
#include "runtime/kernel/marshal.h"

// The Xbox wait engine (spec section 8). Guest memory is the source of truth: every waitable object is a
// DISPATCHER_HEADER in guest memory (type +0, signal_state +4, see guest_structs.h), whether the guest initialized
// it itself (KeInitializeEvent is inlined in the game) or a Nt*Create* import did. The dispatcher works on header
// addresses only and keeps no host state inside guest structures.
//
// One global mutex serializes every dispatcher operation. signal_state is read and written under it with atomic
// operations on the big-endian word, so they stay consistent with the guest's lwarx/stwcx. (a real CAS) on the
// same word. Waiters are registered per header address in FIFO order, each with its own condition variable, and
// are satisfied by whoever signals (set/pulse/release/signal) while that caller holds the mutex: a pulse or an
// auto-reset event therefore never loses a wake-up. A guest that changes signal_state with plain stores and no
// import call wakes nobody until the next dispatcher operation on that header.

namespace lr::kernel {

using lr::runtime::be;

struct Timeout {
  enum Kind { Infinite, Poll, Relative, Absolute } kind;
  int64_t ticks_100ns;  // Relative: positive duration; Absolute: FILETIME (100 ns since 1601-01-01 UTC)
};

// Guest LARGE_INTEGER* timeout: null -> Infinite, 0 -> Poll, < 0 -> Relative (ticks = -value), > 0 -> Absolute
// against the host realtime clock. Reads through kernel().memory(); a pointer outside the guest range (< 0x10000)
// is treated as null.
Timeout timeout_from_guest(GuestPtr<be<int64_t>> p);

enum class WaitType { All = 0, Any = 1 };  // values of the guest WAIT_TYPE argument

// Header types (KOBJECTS) the dispatcher knows.
enum : uint8_t {
  kNotificationEvent = 0,
  kSynchronizationEvent = 1,
  kMutantObject = 2,
  kSemaphoreObject = 5,
  kThreadObject = 6,
  kNotificationTimer = 8,
  kSynchronizationTimer = 9,
};

constexpr uint32_t STATUS_WAIT_0 = 0;

struct WaitInfo {
  uint32_t thread_token;
  std::vector<uint32_t> headers;
  WaitType type;
  bool infinite;
  bool alertable;
  bool apc_pending;  // wake_for_apc left a wake-up for this token (an alertable wait would return STATUS_USER_APC)
};

class Dispatcher {
 public:
  explicit Dispatcher(uint8_t* guest_base) : base_(guest_base) {}
  Dispatcher(const Dispatcher&) = delete;
  Dispatcher& operator=(const Dispatcher&) = delete;

  // headers: guest addresses of DISPATCHER_HEADERs (4-byte aligned, valid guest memory; the imports check).
  // Returns STATUS_WAIT_0+i, STATUS_ABANDONED_WAIT_0+i, STATUS_TIMEOUT or STATUS_USER_APC (alertable only).
  // Consumes auto-reset events / timers, semaphore counts and mutant ownership. WaitAll acquires all or none.
  uint32_t wait(std::span<const uint32_t> headers, WaitType type, Timeout t, bool alertable);
  // Signals `signal_header` (event: set; semaphore: release 1; mutant: release) and waits on `wait_header` as one
  // atomic step (NtSignalAndWaitForSingleObjectEx). A signal failure (STATUS_SEMAPHORE_LIMIT_EXCEEDED,
  // STATUS_MUTANT_NOT_OWNED, STATUS_OBJECT_TYPE_MISMATCH for other types) is returned without waiting.
  uint32_t signal_and_wait(uint32_t signal_header, uint32_t wait_header, Timeout t, bool alertable);

  // After the caller changed signal_state itself: wakes the waiters it can satisfy.
  void signal(uint32_t header);
  // Event operations; each returns the previous signal state.
  int32_t set_event(uint32_t header);
  int32_t reset_event(uint32_t header);
  int32_t pulse_event(uint32_t header);
  // STATUS_SUCCESS (previous count in *previous), STATUS_SEMAPHORE_LIMIT_EXCEEDED (state unchanged) or
  // STATUS_INVALID_PARAMETER for count <= 0. Limit read from X_KSEMAPHORE +0x10.
  uint32_t release_semaphore(uint32_t header, int32_t count, int32_t* previous);
  // Writes a fresh mutant (type 2): owned by the calling thread (signal_state 0) or free (1).
  void init_mutant(uint32_t header, bool initial_owner);
  // STATUS_SUCCESS (previous signal state in *previous) or STATUS_MUTANT_NOT_OWNED when the calling thread is not
  // the owner. `abandon` releases completely (signal_state 1, owner 0) and marks it abandoned.
  uint32_t release_mutant(uint32_t header, bool abandon, int32_t* previous);

  // Makes an alertable wait of `thread_token` return STATUS_USER_APC (now, or at its next alertable wait).
  void wake_for_apc(uint32_t thread_token);

  // A guest thread ended: forgets its pending APC wake-up and abandons every mutant it still owns (signal_state 1,
  // owner 0, abandoned: the next acquirer gets STATUS_ABANDONED_WAIT_0), so nothing carries over to a new thread
  // whose KTHREAD (its token) reuses the same guest address.
  void thread_exited(uint32_t thread_token);

  // Threads currently blocked in wait(), for the deadlock detector (runtime/kernel/deadlock.h).
  std::vector<WaitInfo> snapshot() const;
  // Whether a wake_for_apc for `thread_token` is pending (not yet consumed by an alertable wait nor cleared by
  // thread_exited).
  bool apc_pending(uint32_t thread_token) const;
  // Counts wait entries and exits (every call to wait() or signal_and_wait() that reaches the wait logic, and every
  // blocked wait that ends): unchanged between two reads means no thread entered or left a wait meanwhile.
  uint64_t activity() const { return activity_.load(std::memory_order_acquire); }

  // Identity of the calling thread for mutant ownership and wake_for_apc: the hook's value when one is set (the
  // thread task sets the current KTHREAD guest address; 0 from the hook means "not a guest thread"), else a non-zero
  // token unique per host thread.
  uint32_t current_thread_token() const;
  void set_thread_token_hook(uint32_t (*hook)()) { token_hook_.store(hook, std::memory_order_release); }

 private:
  struct Waiter {
    uint32_t token;
    std::vector<uint32_t> headers;
    WaitType type;
    bool infinite;
    bool alertable;
    bool done = false;
    uint32_t result = 0;
    std::condition_variable cv;
  };

  // All private helpers run with mutex_ held.
  uint8_t type_of(uint32_t header) const { return base_[header]; }
  int32_t load_state(uint32_t header) const;
  void store_state(uint32_t header, int32_t v);
  uint32_t load_u32(uint32_t addr) const;
  void store_u32(uint32_t addr, uint32_t v);
  bool satisfiable(uint32_t header, uint32_t token) const;
  // Consumes `header` for `token`; true when it was an abandoned mutant.
  bool consume(uint32_t header, uint32_t token);
  // The wait status when the wait described by (headers, type, token) can be satisfied now (consuming it).
  bool try_satisfy(std::span<const uint32_t> headers, WaitType type, uint32_t token, uint32_t& result);
  void wake_waiters(uint32_t header);
  // Writes a mutant's owner field and updates owned_.
  void set_owner(uint32_t header, uint32_t token);
  int32_t set_event_locked(uint32_t header);
  uint32_t release_semaphore_locked(uint32_t header, int32_t count, int32_t* previous);
  uint32_t release_mutant_locked(uint32_t header, bool abandon, int32_t* previous);
  uint32_t wait_locked(std::unique_lock<std::mutex>& lock, std::span<const uint32_t> headers, WaitType type,
                       Timeout t, bool alertable);

  uint8_t* base_;
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, std::vector<Waiter*>> by_header_;
  std::vector<Waiter*> waiters_;
  std::unordered_map<uint32_t, bool> apc_pending_;
  // thread token -> headers of the mutants it owns (kept in step with the owner field by set_owner).
  std::unordered_map<uint32_t, std::vector<uint32_t>> owned_;
  std::atomic<uint32_t (*)()> token_hook_{nullptr};
  std::atomic<uint64_t> activity_{0};
};

}  // namespace lr::kernel
