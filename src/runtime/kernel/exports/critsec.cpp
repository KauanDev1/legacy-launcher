// SPDX-License-Identifier: MIT
// Rtl*CriticalSection over X_RTL_CRITICAL_SECTION in guest memory. Semantics follow Xenia's xboxkrnl_rtl.cc (semantic
// reference only, no code copied), with every field kept big-endian in guest memory: lock_count is a BE word that the
// host changes with std::atomic_ref CAS loops, so it stays coherent with the recompiled lwarx/stwcx. (a real CAS) on
// the same word.
//
// lock_count is -1 when free; each Enter that is not a recursion increments it, so 0 means "owned, nobody waiting"
// and > 0 means waiters. Contended Enters wait on the embedded dispatcher header, a synchronization (auto-reset)
// event: Leave sets it when lock_count stays >= 0 after releasing. The auto-reset event keeps the wake-up if the
// waiter has not blocked yet, so none is lost.
//
// Spin count: Xenia keeps it in header.absolute as ceil(spin_count / 256) capped at 255, and Enter spins
// absolute * 256 times; we do the same (the field sits in the guest struct, so guest code that reads it agrees).
#include <immintrin.h>

#include <atomic>
#include <cstdint>

#include "runtime/kernel/dispatcher.h"
#include "runtime/kernel/guest_structs.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/registry.h"

namespace lr::kernel {
namespace {

constexpr uint32_t kFree = 0xFFFFFFFFu;  // lock_count -1

struct Cs {
  X_RTL_CRITICAL_SECTION* s;
  uint32_t addr;
  std::atomic_ref<uint32_t> lock() const {
    return std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(&s->lock_count));
  }
};

Cs open(const GuestCall& c, uint32_t addr) {
  if ((addr & 3) != 0 || !GuestPtr<uint8_t>{addr}.valid_range(sizeof(X_RTL_CRITICAL_SECTION)))
    guest_fatal("critical section at an invalid guest address");
  return {reinterpret_cast<X_RTL_CRITICAL_SECTION*>(c.base + addr), addr};
}

uint32_t me() { return kernel().dispatcher().current_thread_token(); }

void init(const Cs& cs, uint32_t spin_count) {
  auto* s = cs.s;
  const uint32_t units = (spin_count + 255) / 256;
  s->header.type = kSynchronizationEvent;
  s->header.absolute = static_cast<uint8_t>(units > 255 ? 255 : units);
  s->header.size = sizeof(X_RTL_CRITICAL_SECTION) / 4;
  s->header.inserted = 0;
  s->header.signal_state = 0;
  s->header.wait_list_flink = cs.addr + 8;
  s->header.wait_list_blink = cs.addr + 8;
  s->lock_count = -1;
  s->recursion_count = 0;
  s->owning_thread = 0;
}

// Atomic add on the BE word; returns the new value (host order).
uint32_t add_lock(const Cs& cs, int32_t delta) {
  auto w = cs.lock();
  uint32_t old = w.load(std::memory_order_relaxed);
  for (;;) {
    const uint32_t nv = __builtin_bswap32(__builtin_bswap32(old) + static_cast<uint32_t>(delta));
    if (w.compare_exchange_weak(old, nv, std::memory_order_acq_rel, std::memory_order_relaxed))
      return __builtin_bswap32(nv);
  }
}

bool try_free_to_owned(const Cs& cs) {
  uint32_t expected = __builtin_bswap32(kFree);
  return cs.lock().compare_exchange_strong(expected, 0, std::memory_order_acquire, std::memory_order_relaxed);
}

void take(const Cs& cs, uint32_t token) {
  cs.s->owning_thread = token;
  cs.s->recursion_count = 1;
}

}  // namespace

namespace exports {

void RtlInitializeCriticalSection(GuestCall& c, uint32_t cs) { init(open(c, cs), 0); }

uint32_t RtlInitializeCriticalSectionAndSpinCount(GuestCall& c, uint32_t cs, uint32_t spin_count) {
  init(open(c, cs), spin_count);
  return 0;
}

void RtlEnterCriticalSection(GuestCall& c, uint32_t addr) {
  const Cs cs = open(c, addr);
  const uint32_t token = me();
  if (cs.s->owning_thread == token) {
    cs.s->recursion_count = cs.s->recursion_count + 1;
    add_lock(cs, 1);
    return;
  }
  for (uint32_t n = static_cast<uint32_t>(cs.s->header.absolute) * 256; n > 0; --n) {
    if (try_free_to_owned(cs)) {
      take(cs, token);
      return;
    }
    _mm_pause();
  }
  if (add_lock(cs, 1) != 0) {
    const uint32_t h[1] = {addr};
    kernel().dispatcher().wait(h, WaitType::Any, Timeout{Timeout::Infinite, 0}, false);
  }
  take(cs, token);
}

void RtlLeaveCriticalSection(GuestCall& c, uint32_t addr) {
  const Cs cs = open(c, addr);
  if (cs.s->owning_thread != me()) guest_fatal("RtlLeaveCriticalSection by non-owner");
  const int32_t rec = cs.s->recursion_count - 1;
  cs.s->recursion_count = rec;
  if (rec > 0) {
    add_lock(cs, -1);
    return;
  }
  cs.s->owning_thread = 0;
  if (static_cast<int32_t>(add_lock(cs, -1)) >= 0) kernel().dispatcher().set_event(addr);
}

uint32_t RtlTryEnterCriticalSection(GuestCall& c, uint32_t addr) {
  const Cs cs = open(c, addr);
  const uint32_t token = me();
  if (cs.s->owning_thread == token) {
    cs.s->recursion_count = cs.s->recursion_count + 1;
    add_lock(cs, 1);
    return 1;
  }
  if (!try_free_to_owned(cs)) return 0;
  take(cs, token);
  return 1;
}

}  // namespace exports

LR_KERNEL(RtlInitializeCriticalSection);
LR_KERNEL(RtlInitializeCriticalSectionAndSpinCount);
LR_KERNEL(RtlEnterCriticalSection);
LR_KERNEL(RtlLeaveCriticalSection);
LR_KERNEL(RtlTryEnterCriticalSection);

}  // namespace lr::kernel
