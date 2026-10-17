// SPDX-License-Identifier: MIT
// Time, timer, DPC and spinlock imports over runtime/kernel/timers.h. Argument order and status codes follow Xenia's
// xboxkrnl_threading.cc (NtCreateTimer, NtSetTimerEx, NtCancelTimer, KeInitializeDpc, KeInsertQueueDpc, Kf*SpinLock,
// Ke*SpinLock*RaisedIrql, KeQueryPerformanceFrequency, KeQuerySystemTime) and xboxkrnl_rtl.cc (RtlTimeToTimeFields,
// RtlTimeFieldsToTime); semantic reference only, no code copied. Differences from Xenia are noted per import.
#include <emmintrin.h>
#include <sched.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

#include "runtime/kernel/dispatcher.h"
#include "runtime/kernel/guest_structs.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/objects.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/status.h"
#include "runtime/kernel/threads.h"
#include "runtime/kernel/timers.h"

namespace lr::kernel {
namespace {

using U32Ptr = GuestPtr<be<uint32_t>>;
using I64Ptr = GuestPtr<be<int64_t>>;

constexpr uint8_t kDispatchLevel = 2;
constexpr uint32_t kDpcObjectType = 19;  // Xenia: DpcObject, top byte of X_KDPC's first word
constexpr int kSpinsBeforeYield = 1000;
// Due times further out than ~3 years are clamped (they never fire in practice; keeps time_point math in range).
constexpr int64_t kMaxDueTicks = 1'000'000'000'000'000;

// A timer created by NtCreateTimer: its X_KTIMER body comes from the kernel pool. Destroyed after its last handle and
// kernel reference: it disarms itself first, so the worker never touches freed memory.
class TimerObject final : public KObject {
 public:
  TimerObject(Kernel& k, uint32_t body) : KObject(ObjectKind::Timer), kernel_(k), body_(body) {}
  ~TimerObject() override {
    (void)kernel_.timers().cancel_timer(body_);
    kernel_.pool_free(body_);
  }

 private:
  Kernel& kernel_;
  uint32_t body_;
};

// The timer header behind `handle`, or a status (STATUS_INVALID_HANDLE / STATUS_OBJECT_TYPE_MISMATCH).
uint32_t resolve_timer(uint32_t handle, std::shared_ptr<KObject>& obj, uint32_t& header) {
  obj = kernel().objects().get(handle);
  if (!obj) return STATUS_INVALID_HANDLE;
  if (obj->kind() != ObjectKind::Timer || obj->guest_header() == 0) return STATUS_OBJECT_TYPE_MISMATCH;
  header = obj->guest_header();
  return STATUS_SUCCESS;
}

void put_u32(const GuestCall& c, U32Ptr p, uint32_t v) {
  if (p.valid_range(4)) *p.get(c.base) = v;
}

// Lock word of a spinlock: a guest BE u32, 0 when free, else the owner's KTHREAD address (1 off guest threads). Host
// atomics on it interoperate with the guest's lwarx/stwcx. (a real CAS in recompiled code).
std::atomic_ref<uint32_t> lock_word(const GuestCall& c, U32Ptr lock) {
  if (!lock.valid_range(4) || (lock.addr & 3) != 0) guest_fatal("spinlock at an invalid guest address");
  return std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(c.base + lock.addr));
}

void spin_acquire(const GuestCall& c, U32Ptr lock) {
  auto word = lock_word(c, lock);
  const GuestThread* t = current_thread();
  const uint32_t owner = __builtin_bswap32(t != nullptr ? t->kthread() : 1u);
  for (int spins = 0;; ++spins) {
    uint32_t expected = 0;
    if (word.load(std::memory_order_relaxed) == 0 &&
        word.compare_exchange_weak(expected, owner, std::memory_order_acquire, std::memory_order_relaxed))
      return;
    // Host threads can be preempted while holding the lock: spin briefly, then give the time slice away.
    if (spins < kSpinsBeforeYield) _mm_pause();
    else sched_yield();
  }
}

void spin_release(const GuestCall& c, U32Ptr lock) { lock_word(c, lock).store(0, std::memory_order_release); }

}  // namespace

namespace exports {

// --- Clocks and calendar ----------------------------------------------------------------------------------------

// KeQueryPerformanceFrequency() -> the measured host TSC rate (the guest's mftb is __rdtsc()), as a full 64-bit
// value (Xenia truncates its 50 MHz guest clock to 32 bits; a TSC rate can exceed 4 GHz).
uint64_t KeQueryPerformanceFrequency() { return kernel().timers().tsc_hz(); }

// KeQuerySystemTime(time*): host realtime clock as a FILETIME (BE); nothing for a null pointer.
void KeQuerySystemTime(GuestCall& c, I64Ptr out) {
  if (out.valid_range(8)) *out.get(c.base) = host_filetime_now();
}

// RtlTimeToTimeFields(time*, fields*).
void RtlTimeToTimeFields(GuestCall& c, I64Ptr time, GuestPtr<X_TIME_FIELDS> out) {
  if (!time.valid_range(8) || !out.valid_range(sizeof(X_TIME_FIELDS))) return;
  const TimeFields f = filetime_to_fields(*time.get(c.base));
  X_TIME_FIELDS* o = out.get(c.base);
  o->year = static_cast<int16_t>(f.year);
  o->month = static_cast<int16_t>(f.month);
  o->day = static_cast<int16_t>(f.day);
  o->hour = static_cast<int16_t>(f.hour);
  o->minute = static_cast<int16_t>(f.minute);
  o->second = static_cast<int16_t>(f.second);
  o->milliseconds = static_cast<int16_t>(f.milliseconds);
  o->weekday = static_cast<int16_t>(f.weekday);
}

// RtlTimeFieldsToTime(fields*, time*) -> TRUE, or FALSE (time untouched) for out-of-range fields.
uint32_t RtlTimeFieldsToTime(GuestCall& c, GuestPtr<X_TIME_FIELDS> in, I64Ptr time) {
  if (!in.valid_range(sizeof(X_TIME_FIELDS)) || !time.valid_range(8)) return 0;
  const X_TIME_FIELDS* i = in.get(c.base);
  const auto t = fields_to_filetime(TimeFields{i->year, i->month, i->day, i->hour, i->minute, i->second,
                                               i->milliseconds, i->weekday});
  if (!t) return 0;
  *time.get(c.base) = *t;
  return 1;
}

// --- Timers -----------------------------------------------------------------------------------------------------

// NtCreateTimer(handle*, object_attributes, timer_type): 0 = notification (header type 8), 1 = synchronization
// (type 9, auto-reset); other types are STATUS_INVALID_PARAMETER (Xenia asserts). Names are not modelled.
uint32_t NtCreateTimer(GuestCall& c, U32Ptr handle_out, uint32_t /*attributes*/, uint32_t timer_type) {
  if (timer_type > 1) return STATUS_INVALID_PARAMETER;
  if (!handle_out.valid_range(4)) return STATUS_INVALID_PARAMETER;
  Kernel& k = kernel();
  const uint32_t body = k.pool_alloc(sizeof(X_KTIMER), 0);
  if (body == 0) return STATUS_NO_MEMORY;
  auto* t = reinterpret_cast<X_KTIMER*>(c.base + body);
  t->header.type = static_cast<uint8_t>(kNotificationTimer + timer_type);
  t->header.size = sizeof(X_KTIMER) / 4;
  t->header.signal_state = 0;
  t->header.wait_list_flink = body + 8;  // empty list: points at itself
  t->header.wait_list_blink = body + 8;
  auto obj = std::make_shared<TimerObject>(k, body);
  obj->set_guest_header(body);
  put_u32(c, handle_out, k.objects().add(std::move(obj)));
  return STATUS_SUCCESS;
}

// NtSetTimerEx(handle, due_time*, apc_routine, apc_mode, apc_context, resume, period_ms, previous_state*): due time
// < 0 relative 100 ns, otherwise an absolute FILETIME (0 = now). The APC goes to the calling guest thread at each
// expiry. Xenia asserts the last argument is 0; we read it as NT's PreviousState pointer (written when non-null).
// resume -> STATUS_TIMER_RESUME_IGNORED, as Xenia.
uint32_t NtSetTimerEx(GuestCall& c, uint32_t handle, I64Ptr due_ptr, uint32_t apc_routine, uint32_t /*apc_mode*/,
                      uint32_t apc_context, uint32_t resume, uint32_t period_ms, U32Ptr previous) {
  std::shared_ptr<KObject> obj;
  uint32_t header = 0;
  if (uint32_t s = resolve_timer(handle, obj, header); s != STATUS_SUCCESS) return s;
  if (!due_ptr.valid_range(8)) return STATUS_INVALID_PARAMETER;
  const Timeout t = timeout_from_guest(due_ptr);
  int64_t ticks = 0;
  if (t.kind == Timeout::Relative) ticks = t.ticks_100ns;
  else if (t.kind == Timeout::Absolute) ticks = t.ticks_100ns - host_filetime_now();
  if (ticks < 0) ticks = 0;
  if (ticks > kMaxDueTicks) ticks = kMaxDueTicks;
  const auto due = std::chrono::steady_clock::now() + std::chrono::nanoseconds(ticks * 100);

  std::weak_ptr<GuestThread> apc_thread;
  if (GuestThread* self = current_thread(); self != nullptr && apc_routine != 0)
    apc_thread = std::static_pointer_cast<GuestThread>(self->shared_from_this());
  const int32_t prev =
      kernel().timers().set_timer(header, due, period_ms, apc_routine, apc_context, std::move(apc_thread));
  put_u32(c, previous, static_cast<uint32_t>(prev));
  return (resume & 0xFF) != 0 ? STATUS_TIMER_RESUME_IGNORED : STATUS_SUCCESS;
}

// NtCancelTimer(handle, current_state*): disarms; current_state receives the signal state (Xenia writes 0).
uint32_t NtCancelTimer(GuestCall& c, uint32_t handle, U32Ptr current_state) {
  std::shared_ptr<KObject> obj;
  uint32_t header = 0;
  if (uint32_t s = resolve_timer(handle, obj, header); s != STATUS_SUCCESS) return s;
  (void)kernel().timers().cancel_timer(header);
  const int32_t state = reinterpret_cast<const X_DISPATCHER_HEADER*>(c.base + header)->signal_state;
  put_u32(c, current_state, state != 0 ? 1u : 0u);
  return STATUS_SUCCESS;
}

// --- DPCs -------------------------------------------------------------------------------------------------------

// KeInitializeDpc(dpc*, routine, context).
void KeInitializeDpc(GuestCall& c, GuestPtr<X_KDPC> dpc, uint32_t routine, uint32_t context) {
  if (!dpc.valid_range(sizeof(X_KDPC))) return;
  X_KDPC* d = dpc.get(c.base);
  d->type_word = kDpcObjectType << 24;  // importance 0, number 0
  d->flink = 0;
  d->blink = 0;
  d->routine = routine;
  d->context = context;
  d->arg1 = 0;
  d->arg2 = 0;
}

// KeInsertQueueDpc(dpc*, arg1, arg2) -> TRUE when queued, FALSE when already queued. The kernel worker then calls
// routine(dpc, context, arg1, arg2) (Xenia's version never dispatches DPCs).
uint32_t KeInsertQueueDpc(GuestPtr<X_KDPC> dpc, uint32_t arg1, uint32_t arg2) {
  if (!dpc.valid_range(sizeof(X_KDPC))) return 0;
  return kernel().timers().queue_dpc(dpc.addr, arg1, arg2) ? 1 : 0;
}

// --- Spinlocks and IRQL -----------------------------------------------------------------------------------------

// KfAcquireSpinLock(lock*) -> previous IRQL; the calling thread is at DISPATCH_LEVEL while it holds the lock.
uint32_t KfAcquireSpinLock(GuestCall& c, U32Ptr lock) {
  spin_acquire(c, lock);
  const uint8_t old = current_irql();
  set_current_irql(kDispatchLevel);
  return old;
}

// KfReleaseSpinLock(lock*, old_irql): restores the IRQL, then frees the lock (Xenia's order).
void KfReleaseSpinLock(GuestCall& c, U32Ptr lock, uint32_t old_irql) {
  set_current_irql(static_cast<uint8_t>(old_irql));
  spin_release(c, lock);
}

// KeAcquireSpinLockAtRaisedIrql(lock*) / KeReleaseSpinLockFromRaisedIrql(lock*): no IRQL change. Unlike Xenia (pure
// spin) we also yield after a while: guest threads are preemptible host threads.
void KeAcquireSpinLockAtRaisedIrql(GuestCall& c, U32Ptr lock) { spin_acquire(c, lock); }
void KeReleaseSpinLockFromRaisedIrql(GuestCall& c, U32Ptr lock) { spin_release(c, lock); }

}  // namespace exports

LR_KERNEL(KeQueryPerformanceFrequency);
LR_KERNEL(KeQuerySystemTime);
LR_KERNEL(RtlTimeToTimeFields);
LR_KERNEL(RtlTimeFieldsToTime);
LR_KERNEL(NtCreateTimer);
LR_KERNEL(NtSetTimerEx);
LR_KERNEL(NtCancelTimer);
LR_KERNEL(KeInitializeDpc);
LR_KERNEL(KeInsertQueueDpc);
LR_KERNEL(KfAcquireSpinLock);
LR_KERNEL(KfReleaseSpinLock);
LR_KERNEL(KeAcquireSpinLockAtRaisedIrql);
LR_KERNEL(KeReleaseSpinLockFromRaisedIrql);

}  // namespace lr::kernel
