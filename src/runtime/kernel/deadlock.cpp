// SPDX-License-Identifier: MIT
#include "runtime/kernel/deadlock.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <unordered_map>
#include <vector>

#include "runtime/guest_thread.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/objects.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/threads.h"
#include "runtime/kernel/timers.h"
#include "runtime/kernel/trace.h"

namespace lr::kernel {

namespace {

const char* kind_name(ObjectKind k) {
  switch (k) {
    case ObjectKind::Event: return "event";
    case ObjectKind::Semaphore: return "semaphore";
    case ObjectKind::Mutant: return "mutant";
    case ObjectKind::Timer: return "timer";
    case ObjectKind::Thread: return "thread";
    case ObjectKind::File: return "file";
    case ObjectKind::Enumerator: return "enumerator";
    case ObjectKind::NotifyListener: return "notification listener";
    case ObjectKind::Other: return "object";
  }
  return "object";
}

// A header no kernel object owns (initialized by the guest itself, e.g. an inlined KeInitializeEvent or a critical
// section's embedded event): named by its DISPATCHER_HEADER type byte.
std::string header_type_name(uint8_t type) {
  switch (type) {
    case kNotificationEvent: return "notification event, guest-initialized";
    case kSynchronizationEvent: return "synchronization event, guest-initialized";
    case kMutantObject: return "mutant, guest-initialized";
    case kSemaphoreObject: return "semaphore, guest-initialized";
    case kThreadObject: return "thread, guest-initialized";
    case kNotificationTimer: return "notification timer, guest-initialized";
    case kSynchronizationTimer: return "synchronization timer, guest-initialized";
    default: return "header type " + std::to_string(type);
  }
}

bool blocked_forever(const WaitInfo& w) { return w.infinite && !(w.alertable && w.apc_pending); }

}  // namespace

bool all_threads_blocked(const std::vector<uint32_t>& live_tokens, const std::vector<WaitInfo>& waits, bool pending_work) {
  if (live_tokens.empty() || pending_work) return false;
  for (uint32_t token : live_tokens) {
    const auto it = std::find_if(waits.begin(), waits.end(), [token](const WaitInfo& w) { return w.thread_token == token; });
    if (it == waits.end() || !blocked_forever(*it)) return false;
  }
  return true;
}

void DeadlockDetector::on_guest_thread_created() {
  const uint32_t seconds = kernel_.config().deadlock_seconds;
  if (seconds == 0) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (started_ || stop_) return;
  started_ = true;
  thread_ = std::thread([this, seconds] { loop(seconds); });
}

void DeadlockDetector::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
    cv_.notify_all();
  }
  if (thread_.joinable()) thread_.join();
}

void DeadlockDetector::loop(uint32_t seconds) {
  using Clock = std::chrono::steady_clock;
  Dispatcher& d = kernel_.dispatcher();
  bool blocked = false;
  uint64_t blocked_activity = 0;
  Clock::time_point since{};
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stop_) {
    if (cv_.wait_for(lock, std::chrono::seconds(1), [this] { return stop_; })) break;
    lock.unlock();
    // One observation, valid only if no thread entered or left a wait while it was taken.
    const uint64_t a1 = d.activity();
    std::vector<uint32_t> tokens;
    for (const auto& t : kernel_.threads().live()) tokens.push_back(t->kthread());
    const std::vector<WaitInfo> waits = d.snapshot();
    const bool pending = kernel_.timers().has_pending_work();
    const uint64_t a2 = d.activity();
    const auto now = Clock::now();
    if (a1 != a2 || !all_threads_blocked(tokens, waits, pending)) {
      blocked = false;
    } else if (!blocked || a1 != blocked_activity) {
      blocked = true;
      blocked_activity = a1;
      since = now;
    } else if (now - since >= std::chrono::seconds(seconds)) {
      const std::string msg = report(seconds);
      std::fputs(msg.c_str(), stderr);
      std::fflush(nullptr);
      flush_import_trace();
      _exit(runtime::kExitDeadlock);
    }
    lock.lock();
  }
}

std::string DeadlockDetector::report(uint32_t seconds) const {
  std::string s = "Deadlock: every guest thread is blocked in an infinite wait (no timer, DPC or APC pending) for " +
                  std::to_string(seconds) + " s\n";
  std::unordered_map<uint32_t, WaitInfo> by_token;
  for (WaitInfo& w : kernel_.dispatcher().snapshot()) by_token.emplace(w.thread_token, std::move(w));
  auto live = kernel_.threads().live();
  std::sort(live.begin(), live.end(), [](const auto& a, const auto& b) { return a->id() < b->id(); });
  const uint8_t* base = kernel_.memory().base();
  for (const auto& t : live) {
    s += "Thread " + std::to_string(t->id()) + ": waiting on ";
    const auto it = by_token.find(t->kthread());
    if (it == by_token.end()) {
      s += "nothing (no longer waiting)\n";
    } else {
      const WaitInfo& w = it->second;
      for (size_t i = 0; i < w.headers.size(); ++i) {
        const uint32_t h = w.headers[i];
        char addr[16];
        std::snprintf(addr, sizeof addr, "0x%08X", h);
        const auto obj = kernel_.objects().by_guest_header(h);
        s += std::string(i ? ", " : "") + addr + " (" + (obj ? kind_name(obj->kind()) : header_type_name(base[h])) + ")";
      }
      if (w.headers.size() > 1) s += w.type == WaitType::All ? " (all)" : " (any)";
      if (w.alertable) s += ", alertable";
      s += "\n";
    }
    s += format_trail(trail_of(t->trail_handle()));
  }
  return s;
}

}  // namespace lr::kernel
