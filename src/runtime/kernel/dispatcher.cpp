// SPDX-License-Identifier: MIT
// Semantics follow the NT/Xbox dispatcher as described by Xenia (xboxkrnl_threading.cc, xmutant.cc, xevent.cc;
// semantic reference only, no code copied): mutant signal_state is 1 when free and goes down by one per
// acquisition (recursion by the owner), back up per release, and the owner is cleared when it returns to 1.
#include "runtime/kernel/dispatcher.h"

#include <algorithm>
#include <atomic>
#include <chrono>

#include "runtime/kernel/kernel.h"
#include "runtime/kernel/status.h"

namespace lr::kernel {

namespace {

constexpr uint32_t kMutantOwner = 0x18;      // X_KMUTANT::owner
constexpr uint32_t kMutantAbandoned = 0x1C;  // X_KMUTANT::abandoned
constexpr uint32_t kSemaphoreLimit = 0x10;   // X_KSEMAPHORE::limit
// FILETIME of the Unix epoch (100 ns ticks between 1601-01-01 and 1970-01-01).
constexpr int64_t kUnixEpochFiletime = 116444736000000000;
// Longer waits (~3 years) are treated as infinite, which also keeps the nanosecond arithmetic in range.
constexpr int64_t kMaxTicks = 1'000'000'000'000'000;

std::atomic<uint32_t> g_next_token{1};

uint32_t default_token() {
  thread_local const uint32_t token = [] {
    uint32_t t = g_next_token.fetch_add(1, std::memory_order_relaxed);
    if (t == 0) t = g_next_token.fetch_add(1, std::memory_order_relaxed);
    return t;
  }();
  return token;
}

}  // namespace

Timeout timeout_from_guest(GuestPtr<be<int64_t>> p) {
  if (!p.valid_range(8)) return {Timeout::Infinite, 0};
  const int64_t v = *p.get(kernel().memory().base());
  if (v == 0) return {Timeout::Poll, 0};
  if (v < 0) return {Timeout::Relative, v == INT64_MIN ? INT64_MAX : -v};
  return {Timeout::Absolute, v};
}

uint32_t Dispatcher::current_thread_token() const {
  if (auto* hook = token_hook_.load(std::memory_order_acquire))
    if (const uint32_t t = hook(); t != 0) return t;
  return default_token();
}

int32_t Dispatcher::load_state(uint32_t header) const { return static_cast<int32_t>(load_u32(header + 4)); }
void Dispatcher::store_state(uint32_t header, int32_t v) { store_u32(header + 4, static_cast<uint32_t>(v)); }

uint32_t Dispatcher::load_u32(uint32_t addr) const {
  std::atomic_ref<uint32_t> word(*reinterpret_cast<uint32_t*>(base_ + addr));
  return __builtin_bswap32(word.load(std::memory_order_seq_cst));
}

void Dispatcher::store_u32(uint32_t addr, uint32_t v) {
  std::atomic_ref<uint32_t> word(*reinterpret_cast<uint32_t*>(base_ + addr));
  word.store(__builtin_bswap32(v), std::memory_order_seq_cst);
}

bool Dispatcher::satisfiable(uint32_t header, uint32_t token) const {
  if (type_of(header) == kMutantObject) return load_state(header) > 0 || load_u32(header + kMutantOwner) == token;
  return load_state(header) > 0;
}

bool Dispatcher::consume(uint32_t header, uint32_t token) {
  switch (type_of(header)) {
    case kSynchronizationEvent:
    case kSynchronizationTimer:
      store_state(header, 0);
      return false;
    case kSemaphoreObject:
      store_state(header, load_state(header) - 1);
      return false;
    case kMutantObject: {
      store_state(header, load_state(header) - 1);
      set_owner(header, token);
      if (load_u32(header + kMutantAbandoned) != 0) {
        store_u32(header + kMutantAbandoned, 0);
        return true;
      }
      return false;
    }
    default:  // notification events/timers, threads: stay signaled
      return false;
  }
}

bool Dispatcher::try_satisfy(std::span<const uint32_t> headers, WaitType type, uint32_t token, uint32_t& result) {
  if (type == WaitType::Any) {
    for (size_t i = 0; i < headers.size(); ++i) {
      if (!satisfiable(headers[i], token)) continue;
      const bool abandoned = consume(headers[i], token);
      result = (abandoned ? STATUS_ABANDONED_WAIT_0 : STATUS_WAIT_0) + static_cast<uint32_t>(i);
      return true;
    }
    return false;
  }
  for (uint32_t h : headers)
    if (!satisfiable(h, token)) return false;
  bool abandoned = false;
  for (uint32_t h : headers) abandoned = consume(h, token) || abandoned;
  result = abandoned ? STATUS_ABANDONED_WAIT_0 : STATUS_WAIT_0;
  return true;
}

void Dispatcher::wake_waiters(uint32_t header) {
  auto it = by_header_.find(header);
  if (it == by_header_.end()) return;
  const std::vector<Waiter*> list = it->second;  // FIFO
  for (Waiter* w : list) {
    if (w->done) continue;
    if (try_satisfy(w->headers, w->type, w->token, w->result)) {
      w->done = true;
      w->cv.notify_one();
    }
  }
}

uint32_t Dispatcher::wait(std::span<const uint32_t> headers, WaitType type, Timeout t, bool alertable) {
  std::unique_lock<std::mutex> lock(mutex_);
  return wait_locked(lock, headers, type, t, alertable);
}

uint32_t Dispatcher::wait_locked(std::unique_lock<std::mutex>& lock, std::span<const uint32_t> headers,
                                 WaitType type, Timeout t, bool alertable) {
  const uint32_t token = current_thread_token();
  uint32_t result = 0;
  if (try_satisfy(headers, type, token, result)) return result;
  if (alertable) {
    if (auto it = apc_pending_.find(token); it != apc_pending_.end()) {
      apc_pending_.erase(it);
      return STATUS_USER_APC;
    }
  }

  using Clock = std::chrono::steady_clock;
  bool infinite = t.kind == Timeout::Infinite;
  Clock::time_point deadline{};
  if (t.kind == Timeout::Poll) return STATUS_TIMEOUT;
  if (!infinite) {
    int64_t ticks = t.ticks_100ns;
    if (t.kind == Timeout::Absolute) {
      const int64_t now_ft =
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                  .count() / 100 + kUnixEpochFiletime;
      ticks = t.ticks_100ns - now_ft;
    }
    if (ticks <= 0) return STATUS_TIMEOUT;
    if (ticks > kMaxTicks) infinite = true;
    else deadline = Clock::now() + std::chrono::nanoseconds(ticks * 100);
  }

  Waiter w;
  w.token = token;
  w.headers.assign(headers.begin(), headers.end());
  w.type = type;
  w.infinite = infinite;
  w.alertable = alertable;
  std::vector<uint32_t> unique = w.headers;
  std::sort(unique.begin(), unique.end());
  unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
  for (uint32_t h : unique) by_header_[h].push_back(&w);
  waiters_.push_back(&w);

  while (!w.done) {
    if (infinite) {
      w.cv.wait(lock);
    } else if (w.cv.wait_until(lock, deadline) == std::cv_status::timeout && !w.done) {
      w.done = true;
      w.result = STATUS_TIMEOUT;
    }
  }

  for (uint32_t h : unique) {
    auto it = by_header_.find(h);
    std::erase(it->second, &w);
    if (it->second.empty()) by_header_.erase(it);
  }
  std::erase(waiters_, &w);
  return w.result;
}

void Dispatcher::signal(uint32_t header) {
  std::lock_guard<std::mutex> lock(mutex_);
  wake_waiters(header);
}

int32_t Dispatcher::set_event_locked(uint32_t header) {
  const int32_t prev = load_state(header);
  store_state(header, 1);
  wake_waiters(header);
  return prev;
}

int32_t Dispatcher::set_event(uint32_t header) {
  std::lock_guard<std::mutex> lock(mutex_);
  return set_event_locked(header);
}

int32_t Dispatcher::reset_event(uint32_t header) {
  std::lock_guard<std::mutex> lock(mutex_);
  const int32_t prev = load_state(header);
  store_state(header, 0);
  return prev;
}

int32_t Dispatcher::pulse_event(uint32_t header) {
  std::lock_guard<std::mutex> lock(mutex_);
  const int32_t prev = load_state(header);
  store_state(header, 1);
  wake_waiters(header);
  store_state(header, 0);
  return prev;
}

uint32_t Dispatcher::release_semaphore_locked(uint32_t header, int32_t count, int32_t* previous) {
  if (count <= 0) return STATUS_INVALID_PARAMETER;
  const int32_t prev = load_state(header);
  const int32_t limit = static_cast<int32_t>(load_u32(header + kSemaphoreLimit));
  if (static_cast<int64_t>(prev) + count > limit) return STATUS_SEMAPHORE_LIMIT_EXCEEDED;
  store_state(header, prev + count);
  if (previous != nullptr) *previous = prev;
  wake_waiters(header);
  return STATUS_SUCCESS;
}

uint32_t Dispatcher::release_semaphore(uint32_t header, int32_t count, int32_t* previous) {
  std::lock_guard<std::mutex> lock(mutex_);
  return release_semaphore_locked(header, count, previous);
}

void Dispatcher::init_mutant(uint32_t header, bool initial_owner) {
  std::lock_guard<std::mutex> lock(mutex_);
  base_[header] = kMutantObject;
  base_[header + 1] = 0;     // absolute
  base_[header + 2] = 0x20 / 4;  // size in dwords (X_KMUTANT)
  base_[header + 3] = 0;     // inserted
  store_u32(header + 8, header + 8);  // empty wait list: flink = blink = &list
  store_u32(header + 0xC, header + 8);
  store_u32(header + kMutantAbandoned, 0);
  if (initial_owner) {
    set_owner(header, current_thread_token());
    store_state(header, 0);
  } else {
    set_owner(header, 0);
    store_state(header, 1);
  }
}

uint32_t Dispatcher::release_mutant_locked(uint32_t header, bool abandon, int32_t* previous) {
  const int32_t prev = load_state(header);
  if (abandon) {
    store_state(header, 1);
    set_owner(header, 0);
    store_u32(header + kMutantAbandoned, 1);
  } else {
    if (load_u32(header + kMutantOwner) != current_thread_token()) return STATUS_MUTANT_NOT_OWNED;
    store_state(header, prev + 1);
    if (prev + 1 == 1) set_owner(header, 0);
  }
  if (previous != nullptr) *previous = prev;
  wake_waiters(header);
  return STATUS_SUCCESS;
}

uint32_t Dispatcher::release_mutant(uint32_t header, bool abandon, int32_t* previous) {
  std::lock_guard<std::mutex> lock(mutex_);
  return release_mutant_locked(header, abandon, previous);
}

uint32_t Dispatcher::signal_and_wait(uint32_t signal_header, uint32_t wait_header, Timeout t, bool alertable) {
  std::unique_lock<std::mutex> lock(mutex_);
  uint32_t s = STATUS_SUCCESS;
  switch (type_of(signal_header)) {
    case kNotificationEvent:
    case kSynchronizationEvent: (void)set_event_locked(signal_header); break;
    case kSemaphoreObject: s = release_semaphore_locked(signal_header, 1, nullptr); break;
    case kMutantObject: s = release_mutant_locked(signal_header, false, nullptr); break;
    default: s = STATUS_OBJECT_TYPE_MISMATCH; break;
  }
  if (s != STATUS_SUCCESS) return s;
  return wait_locked(lock, std::span<const uint32_t>(&wait_header, 1), WaitType::Any, t, alertable);
}

void Dispatcher::wake_for_apc(uint32_t thread_token) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (Waiter* w : waiters_) {
    if (w->token == thread_token && w->alertable && !w->done) {
      w->done = true;
      w->result = STATUS_USER_APC;
      w->cv.notify_one();
      return;
    }
  }
  apc_pending_[thread_token] = true;
}

void Dispatcher::set_owner(uint32_t header, uint32_t token) {
  const uint32_t old = load_u32(header + kMutantOwner);
  if (old != 0) {
    if (auto it = owned_.find(old); it != owned_.end()) {
      std::erase(it->second, header);
      if (it->second.empty()) owned_.erase(it);
    }
  }
  store_u32(header + kMutantOwner, token);
  if (token != 0) owned_[token].push_back(header);
}

void Dispatcher::thread_exited(uint32_t thread_token) {
  std::lock_guard<std::mutex> lock(mutex_);
  apc_pending_.erase(thread_token);
  auto it = owned_.find(thread_token);
  if (it == owned_.end()) return;
  const std::vector<uint32_t> headers = it->second;  // release_mutant_locked edits owned_
  for (uint32_t h : headers) {
    // Only what is still a mutant owned by this thread (the guest may have reused the memory since).
    if (type_of(h) == kMutantObject && load_u32(h + kMutantOwner) == thread_token)
      (void)release_mutant_locked(h, true, nullptr);
  }
  owned_.erase(thread_token);
}

std::vector<WaitInfo> Dispatcher::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<WaitInfo> out;
  for (const Waiter* w : waiters_)
    if (!w->done) out.push_back({w->token, w->headers, w->type, w->infinite, w->alertable});
  return out;
}

}  // namespace lr::kernel
