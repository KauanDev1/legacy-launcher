// SPDX-License-Identifier: MIT
// Thread, TLS, APC and process imports over runtime/kernel/threads.h. Argument order and status codes follow
// Xenia's xboxkrnl_threading.cc, xboxkrnl_ob.cc (ObLookupThreadByThreadId) and xboxkrnl_modules.cc
// (ExRegisterTitleTerminateNotification); semantic reference only, no code copied.
//
// Ke*Thread imports take a KTHREAD guest pointer (what ObReferenceObjectByHandle hands out); Nt*Thread imports take
// a handle. Priority and affinity are recorded, never applied to host threads (spec section 7).
#include <cstdint>
#include <memory>
#include <thread>

#include "runtime/kernel/dispatcher.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/objects.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/status.h"
#include "runtime/kernel/threads.h"

namespace lr::kernel {
namespace {

using U32Ptr = GuestPtr<be<uint32_t>>;
using TimeoutPtr = GuestPtr<be<int64_t>>;

constexpr uint32_t kCreateSuspended = 0x1;       // creation flag bit 0
constexpr uint32_t kCreateReturnObject = 0x80;   // Xenia: handle_ptr receives the KTHREAD instead of the handle

void put(const GuestCall& c, U32Ptr p, uint32_t v) {
  if (p.valid_range(4)) *p.get(c.base) = v;
}

std::shared_ptr<GuestThread> as_thread(std::shared_ptr<KObject> obj) {
  if (!obj || obj->kind() != ObjectKind::Thread) return nullptr;
  return std::static_pointer_cast<GuestThread>(std::move(obj));
}

std::shared_ptr<GuestThread> thread_by_handle(uint32_t handle) { return as_thread(kernel().objects().get(handle)); }
std::shared_ptr<GuestThread> thread_by_kthread(uint32_t kthread) {
  return as_thread(kernel().objects().by_guest_header(kthread));
}

}  // namespace

namespace exports {

// --- Creation and termination -----------------------------------------------------------------------------------

// ExCreateThread(handle*, stack_size, thread_id*, xapi_thread_startup, start_address, start_context,
// creation_flags): bit 0 = create suspended, 0x80 = write the KTHREAD instead of a handle, flags >> 24 = CPU mask.
uint32_t ExCreateThread(GuestCall& c, U32Ptr handle_out, uint32_t stack_size, U32Ptr id_out, uint32_t xapi_startup,
                        uint32_t start, uint32_t context, uint32_t flags) {
  ThreadParams p;
  p.stack_size = stack_size;
  p.start = start;
  p.context = context;
  p.xapi_start = xapi_startup;
  p.suspended = (flags & kCreateSuspended) != 0;
  p.cpu_mask = static_cast<uint8_t>(flags >> 24);
  auto t = create_guest_thread(p);
  if (!t) return STATUS_NO_MEMORY;
  put(c, handle_out, (flags & kCreateReturnObject) != 0 ? t.value()->kthread() : t.value()->handle());
  put(c, id_out, t.value()->id());
  return STATUS_SUCCESS;
}

// ExTerminateThread(exit_code): ends the calling guest thread (caught at its top).
void ExTerminateThread(uint32_t code) { throw GuestThreadExit{code}; }

// ExRegisterTitleTerminateNotification(registration*, create): registration = {routine +0, priority +4, list +8}.
// Recorded only: M2 has no title termination that would run them.
void ExRegisterTitleTerminateNotification(GuestCall& c, GuestPtr<be<uint32_t>> reg, uint32_t create) {
  if (!reg.valid_range(8)) return;
  const uint32_t routine = reg.get(c.base)[0];
  const uint32_t priority = reg.get(c.base)[1];
  if ((create & 0xFF) != 0) kernel().threads().add_terminate_notification({routine, priority});
  else kernel().threads().remove_terminate_notification(routine);
}

// --- Suspend / resume -------------------------------------------------------------------------------------------

// NtSuspendThread(handle, previous_count*): a thread suspending itself blocks here until resumed; another thread
// stops at its next import entry.
uint32_t NtSuspendThread(GuestCall& c, uint32_t handle, U32Ptr previous) {
  auto t = thread_by_handle(handle);
  if (!t) return STATUS_INVALID_HANDLE;
  put(c, previous, t->request_suspend());
  if (t.get() == current_thread()) t->wait_while_suspended();
  return STATUS_SUCCESS;
}

// NtResumeThread(handle, previous_count*).
uint32_t NtResumeThread(GuestCall& c, uint32_t handle, U32Ptr previous) {
  auto t = thread_by_handle(handle);
  if (!t) return STATUS_INVALID_HANDLE;
  put(c, previous, t->resume());
  return STATUS_SUCCESS;
}

// KeResumeThread(kthread): a status, as Xenia (NT would return the previous count).
uint32_t KeResumeThread(uint32_t kthread) {
  auto t = thread_by_kthread(kthread);
  if (!t) return STATUS_INVALID_HANDLE;
  (void)t->resume();
  return STATUS_SUCCESS;
}

// --- Delays and APCs --------------------------------------------------------------------------------------------

// NtYieldExecution(): gives up the host time slice.
uint32_t NtYieldExecution() {
  std::this_thread::yield();
  return STATUS_SUCCESS;
}

// KeDelayExecutionThread(processor_mode, alertable, interval*): STATUS_SUCCESS after the interval (relative < 0,
// absolute > 0, 0 = yield), or STATUS_USER_APC when an APC arrived during an alertable delay (the queued APCs
// have run by then).
// A null (or null-page) interval pointer is treated as 0, a yield: Xenia dereferences it unconditionally, and
// timeout_from_guest's null = infinite rule (right for waits) would turn a bad call into a thread that never wakes.
uint32_t KeDelayExecutionThread(uint32_t /*mode*/, uint32_t alertable, TimeoutPtr interval) {
  const Timeout t = interval.valid_range(8) ? timeout_from_guest(interval) : Timeout{Timeout::Poll, 0};
  const uint32_t s = kernel().dispatcher().wait({}, WaitType::Any, t, (alertable & 0xFF) != 0);
  if (s == STATUS_USER_APC) {
    deliver_current_apcs();
    return STATUS_USER_APC;
  }
  if (t.kind == Timeout::Poll) std::this_thread::yield();
  return STATUS_SUCCESS;
}

// NtQueueApcThread(handle, routine, routine_context, arg1, arg2): routine(routine_context, arg1, arg2) runs on the
// target thread in its next alertable wait.
uint32_t NtQueueApcThread(uint32_t handle, uint32_t routine, uint32_t context, uint32_t arg1, uint32_t arg2) {
  auto t = thread_by_handle(handle);
  if (!t) return STATUS_INVALID_HANDLE;
  if (routine == 0) return STATUS_INVALID_PARAMETER;
  t->queue_apc({routine, context, arg1, arg2});
  return STATUS_SUCCESS;
}

// KiApcNormalRoutineNop(a, b): the kernel's empty APC normal routine.
uint32_t KiApcNormalRoutineNop(uint32_t /*a*/, uint32_t /*b*/) { return 0; }

// KeEnterCriticalRegion() / KeLeaveCriticalRegion(): APC-disable counter of the calling thread.
void KeEnterCriticalRegion() {
  if (GuestThread* t = current_thread()) t->enter_critical_region();
}
void KeLeaveCriticalRegion() {
  if (GuestThread* t = current_thread()) t->leave_critical_region();
}

// --- Priority, affinity, lookups --------------------------------------------------------------------------------

// KeQueryBasePriorityThread(kthread) -> priority, 0 for an unknown thread.
int32_t KeQueryBasePriorityThread(uint32_t kthread) {
  auto t = thread_by_kthread(kthread);
  return t ? t->priority() : 0;
}

// KeSetBasePriorityThread(kthread, priority) -> previous priority, 0 for an unknown thread.
int32_t KeSetBasePriorityThread(uint32_t kthread, int32_t priority) {
  auto t = thread_by_kthread(kthread);
  return t ? t->set_priority(priority) : 0;
}

// KeSetAffinityThread(kthread, affinity, previous_affinity*): as Xenia, affinity 0 is STATUS_INVALID_PARAMETER and
// an unknown thread still succeeds.
uint32_t KeSetAffinityThread(GuestCall& c, uint32_t kthread, uint32_t affinity, U32Ptr previous) {
  if (affinity == 0) return STATUS_INVALID_PARAMETER;
  if (auto t = thread_by_kthread(kthread)) put(c, previous, t->set_affinity(affinity));
  return STATUS_SUCCESS;
}

// ObLookupThreadByThreadId(thread_id, out_object*): the KTHREAD, with a kernel reference released by
// ObDereferenceObject; STATUS_NOT_FOUND for an id with no live thread.
uint32_t ObLookupThreadByThreadId(GuestCall& c, uint32_t id, U32Ptr out) {
  auto t = kernel().threads().by_id(id);
  if (!t) return STATUS_NOT_FOUND;
  put(c, out, t->kthread());
  kernel().objects().add_kernel_ref(t);
  return STATUS_SUCCESS;
}

// KeGetCurrentProcessType() / KeSetCurrentProcessType(type): the title process type (default 1); values above 2
// are ignored (Xenia asserts them).
uint32_t KeGetCurrentProcessType() { return kernel().threads().process_type(); }
void KeSetCurrentProcessType(uint32_t type) {
  if (type <= 2) kernel().threads().set_process_type(type);
}

// --- TLS --------------------------------------------------------------------------------------------------------

// KeTlsAlloc() -> slot index, or TLS_OUT_OF_INDEXES (0xFFFFFFFF). The slot starts at 0 in every live thread.
uint32_t KeTlsAlloc(GuestCall& c) {
  Kernel& k = kernel();
  const uint32_t index = k.threads().tls_alloc();
  if (index == Threads::kTlsOutOfIndexes) return index;
  for (const auto& t : k.threads().live())
    if (const uint32_t a = t->tls_slot_address(index)) *reinterpret_cast<be<uint32_t>*>(c.base + a) = 0;
  return index;
}

// KeTlsFree(index) -> TRUE, or FALSE for TLS_OUT_OF_INDEXES / a slot that is not allocated.
uint32_t KeTlsFree(uint32_t index) { return kernel().threads().tls_free(index) ? 1 : 0; }

// KeTlsGetValue(index) -> the calling thread's value, 0 when out of range (the kernel reports no error).
uint32_t KeTlsGetValue(GuestCall& c, uint32_t index) {
  GuestThread* t = current_thread();
  const uint32_t a = t != nullptr ? t->tls_slot_address(index) : 0;
  return a != 0 ? static_cast<uint32_t>(*reinterpret_cast<be<uint32_t>*>(c.base + a)) : 0;
}

// KeTlsSetValue(index, value) -> TRUE, or FALSE when out of range.
uint32_t KeTlsSetValue(GuestCall& c, uint32_t index, uint32_t value) {
  GuestThread* t = current_thread();
  const uint32_t a = t != nullptr ? t->tls_slot_address(index) : 0;
  if (a == 0) return 0;
  *reinterpret_cast<be<uint32_t>*>(c.base + a) = value;
  return 1;
}

}  // namespace exports

LR_KERNEL(ExCreateThread);
LR_KERNEL(ExTerminateThread);
LR_KERNEL(ExRegisterTitleTerminateNotification);
LR_KERNEL(NtSuspendThread);
LR_KERNEL(NtResumeThread);
LR_KERNEL(KeResumeThread);
LR_KERNEL(NtYieldExecution);
LR_KERNEL(KeDelayExecutionThread);
LR_KERNEL(NtQueueApcThread);
LR_KERNEL(KiApcNormalRoutineNop);
LR_KERNEL(KeEnterCriticalRegion);
LR_KERNEL(KeLeaveCriticalRegion);
LR_KERNEL(KeQueryBasePriorityThread);
LR_KERNEL(KeSetBasePriorityThread);
LR_KERNEL(KeSetAffinityThread);
LR_KERNEL(ObLookupThreadByThreadId);
LR_KERNEL(KeGetCurrentProcessType);
LR_KERNEL(KeSetCurrentProcessType);
LR_KERNEL(KeTlsAlloc);
LR_KERNEL(KeTlsFree);
LR_KERNEL(KeTlsGetValue);
LR_KERNEL(KeTlsSetValue);
// Host threads have no boost to disable; Xenia does nothing either.
LR_KERNEL_STUB(KeSetDisableBoostThread, 0);
// The recompiled code keeps FP state in PPCContext per call; there is no kernel FP state to save or restore.
LR_KERNEL_STUB(KeSaveFloatingPointState, STATUS_SUCCESS);
LR_KERNEL_STUB(KeRestoreFloatingPointState, STATUS_SUCCESS);
// FP exceptions stay masked on the host (glue_call loads the host MXCSR); Xenia ignores this call too.
LR_KERNEL_STUB(KeEnableFpuExceptions, 0);

}  // namespace lr::kernel
