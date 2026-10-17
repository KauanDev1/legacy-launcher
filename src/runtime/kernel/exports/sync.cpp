// SPDX-License-Identifier: MIT
// Event, semaphore, mutant and wait imports over lr::kernel::Dispatcher. Argument order and status codes follow
// Xenia's xboxkrnl_threading.cc (semantic reference only, no code copied). KeInitializeMutant/KeReleaseMutant are
// not in Xenia; they follow the NT signatures (KeInitializeMutant(mutant, initial_owner),
// KeReleaseMutant(mutant, increment, abandoned, wait) -> previous signal state).
//
// Object attributes (names) of Nt*Create* are ignored: named objects are not modelled in M2, every create makes a
// new unnamed object. Increments and the KWAIT_BLOCK array of KeWaitForMultipleObjects are ignored too.
#include <cstdint>
#include <memory>
#include <vector>

#include "runtime/kernel/dispatcher.h"
#include "runtime/kernel/guest_structs.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/objects.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/status.h"

namespace lr::kernel {
namespace {

using U32Ptr = GuestPtr<be<uint32_t>>;
using TimeoutPtr = GuestPtr<be<int64_t>>;

constexpr uint32_t kMaxWaitObjects = 64;  // MAXIMUM_WAIT_OBJECTS

// An object created by a Nt*Create* import: its guest body comes from the kernel pool and is freed with the
// object (after its last handle and kernel reference). The Kernel destroys its objects before its heaps.
class PoolObject final : public KObject {
 public:
  PoolObject(ObjectKind kind, Kernel& k, uint32_t body) : KObject(kind), kernel_(k), body_(body) {}
  ~PoolObject() override { kernel_.pool_free(body_); }

 private:
  Kernel& kernel_;
  uint32_t body_;
};

bool valid_header(uint32_t addr) { return (addr & 3) == 0 && GuestPtr<uint8_t>{addr}.valid_range(0x10); }

void put(const GuestCall& c, U32Ptr p, uint32_t v) {
  if (p.valid_range(4)) *p.get(c.base) = v;
}

// The waitable object behind `handle`, or a status: STATUS_INVALID_HANDLE, or STATUS_OBJECT_TYPE_MISMATCH for an
// object without a guest dispatcher header (a file, for instance).
uint32_t resolve(uint32_t handle, std::shared_ptr<KObject>& out, uint32_t& header) {
  out = kernel().objects().get(handle);
  if (!out) return STATUS_INVALID_HANDLE;
  header = out->guest_header();
  return header == 0 ? STATUS_OBJECT_TYPE_MISMATCH : STATUS_SUCCESS;
}

uint8_t header_type(const GuestCall& c, uint32_t header) { return c.base[header]; }

bool is_event(uint8_t t) { return t == kNotificationEvent || t == kSynchronizationEvent; }

void write_header(const GuestCall& c, uint32_t header, uint8_t type, uint8_t size_dwords, int32_t signal) {
  auto* h = reinterpret_cast<X_DISPATCHER_HEADER*>(c.base + header);
  h->type = type;
  h->absolute = 0;
  h->size = size_dwords;
  h->inserted = 0;
  h->signal_state = signal;
  h->wait_list_flink = header + 8;  // empty list: points at itself
  h->wait_list_blink = header + 8;
}

// Allocates a pool body of `bytes`, lets `init` fill it, registers the object and writes its handle.
template <class Init>
uint32_t create(const GuestCall& c, U32Ptr handle_out, ObjectKind kind, uint32_t bytes, Init init) {
  if (!handle_out.valid_range(4)) return STATUS_INVALID_PARAMETER;
  Kernel& k = kernel();
  const uint32_t body = k.pool_alloc(bytes, 0);
  if (body == 0) return STATUS_NO_MEMORY;
  init(body);
  auto obj = std::make_shared<PoolObject>(kind, k, body);
  obj->set_guest_header(body);
  put(c, handle_out, k.objects().add(std::move(obj)));
  return STATUS_SUCCESS;
}

uint32_t wait_one(uint32_t header, uint32_t alertable, TimeoutPtr timeout) {
  const uint32_t h[1] = {header};
  return kernel().dispatcher().wait(h, WaitType::Any, timeout_from_guest(timeout), alertable != 0);
}

}  // namespace

namespace exports {

// --- Events -----------------------------------------------------------------------------------------------------

// NtCreateEvent(handle*, object_attributes, event_type, initial_state): event_type 0 = notification (manual
// reset), anything else = synchronization (auto reset), as Xenia's `!event_type` manual-reset flag.
uint32_t NtCreateEvent(GuestCall& c, U32Ptr handle_out, uint32_t /*attributes*/, uint32_t event_type,
                       uint32_t initial_state) {
  const uint8_t type = event_type == 0 ? kNotificationEvent : kSynchronizationEvent;
  return create(c, handle_out, ObjectKind::Event, sizeof(X_KEVENT),
                [&](uint32_t body) { write_header(c, body, type, sizeof(X_KEVENT) / 4, initial_state != 0 ? 1 : 0); });
}

// NtSetEvent(handle, previous_state*), NtPulseEvent(handle, previous_state*), NtClearEvent(handle).
uint32_t NtSetEvent(GuestCall& c, uint32_t handle, U32Ptr previous) {
  std::shared_ptr<KObject> obj;
  uint32_t header = 0;
  if (uint32_t s = resolve(handle, obj, header); s != STATUS_SUCCESS) return s;
  if (!is_event(header_type(c, header))) return STATUS_OBJECT_TYPE_MISMATCH;
  put(c, previous, static_cast<uint32_t>(kernel().dispatcher().set_event(header)));
  return STATUS_SUCCESS;
}

uint32_t NtPulseEvent(GuestCall& c, uint32_t handle, U32Ptr previous) {
  std::shared_ptr<KObject> obj;
  uint32_t header = 0;
  if (uint32_t s = resolve(handle, obj, header); s != STATUS_SUCCESS) return s;
  if (!is_event(header_type(c, header))) return STATUS_OBJECT_TYPE_MISMATCH;
  put(c, previous, static_cast<uint32_t>(kernel().dispatcher().pulse_event(header)));
  return STATUS_SUCCESS;
}

uint32_t NtClearEvent(GuestCall& c, uint32_t handle) {
  std::shared_ptr<KObject> obj;
  uint32_t header = 0;
  if (uint32_t s = resolve(handle, obj, header); s != STATUS_SUCCESS) return s;
  if (!is_event(header_type(c, header))) return STATUS_OBJECT_TYPE_MISMATCH;
  (void)kernel().dispatcher().reset_event(header);
  return STATUS_SUCCESS;
}

// KeSetEvent(event, increment, wait) / KeResetEvent(event): previous signal state; 0 for a bad pointer.
int32_t KeSetEvent(uint32_t event, uint32_t /*increment*/, uint32_t /*wait*/) {
  return valid_header(event) ? kernel().dispatcher().set_event(event) : 0;
}

int32_t KeResetEvent(uint32_t event) { return valid_header(event) ? kernel().dispatcher().reset_event(event) : 0; }

// --- Semaphores -------------------------------------------------------------------------------------------------

// NtCreateSemaphore(handle*, object_attributes, initial_count, limit).
uint32_t NtCreateSemaphore(GuestCall& c, U32Ptr handle_out, uint32_t /*attributes*/, int32_t count, int32_t limit) {
  if (limit <= 0 || count < 0 || count > limit) return STATUS_INVALID_PARAMETER;
  return create(c, handle_out, ObjectKind::Semaphore, sizeof(X_KSEMAPHORE), [&](uint32_t body) {
    write_header(c, body, kSemaphoreObject, sizeof(X_KSEMAPHORE) / 4, count);
    reinterpret_cast<X_KSEMAPHORE*>(c.base + body)->limit = limit;
  });
}

// NtReleaseSemaphore(handle, release_count, previous_count*).
uint32_t NtReleaseSemaphore(GuestCall& c, uint32_t handle, int32_t count, U32Ptr previous) {
  std::shared_ptr<KObject> obj;
  uint32_t header = 0;
  if (uint32_t s = resolve(handle, obj, header); s != STATUS_SUCCESS) return s;
  if (header_type(c, header) != kSemaphoreObject) return STATUS_OBJECT_TYPE_MISMATCH;
  int32_t prev = 0;
  const uint32_t s = kernel().dispatcher().release_semaphore(header, count, &prev);
  if (s == STATUS_SUCCESS) put(c, previous, static_cast<uint32_t>(prev));
  return s;
}

// --- Mutants ----------------------------------------------------------------------------------------------------

// KeInitializeMutant(mutant, initial_owner): the owner is the calling thread (Dispatcher::current_thread_token).
void KeInitializeMutant(uint32_t mutant, uint32_t initial_owner) {
  if (GuestPtr<X_KMUTANT>{mutant}.valid_range(sizeof(X_KMUTANT)) && (mutant & 3) == 0)
    kernel().dispatcher().init_mutant(mutant, (initial_owner & 0xFF) != 0);
}

// KeReleaseMutant(mutant, increment, abandoned, wait): the previous signal state. NT raises
// STATUS_MUTANT_NOT_OWNED when the caller does not own it; we return that status instead (as a negative LONG).
int32_t KeReleaseMutant(uint32_t mutant, uint32_t /*increment*/, uint32_t abandoned, uint32_t /*wait*/) {
  if (!valid_header(mutant)) return static_cast<int32_t>(STATUS_INVALID_PARAMETER);
  int32_t prev = 0;
  const uint32_t s = kernel().dispatcher().release_mutant(mutant, (abandoned & 0xFF) != 0, &prev);
  return s == STATUS_SUCCESS ? prev : static_cast<int32_t>(s);
}

// --- Waits ------------------------------------------------------------------------------------------------------

// KeWaitForSingleObject(object, wait_reason, processor_mode, alertable, timeout*): `object` is the guest address
// of a dispatcher header, registered or not.
uint32_t KeWaitForSingleObject(uint32_t object, uint32_t /*reason*/, uint32_t /*mode*/, uint32_t alertable,
                               TimeoutPtr timeout) {
  if (!valid_header(object)) return STATUS_INVALID_PARAMETER;
  return wait_one(object, alertable, timeout);
}

// KeWaitForMultipleObjects(count, objects*, wait_type, reason, mode, alertable, timeout*, wait_blocks): objects is
// an array of header addresses; wait_type 0 = WaitAll, 1 = WaitAny (NT WAIT_TYPE).
uint32_t KeWaitForMultipleObjects(GuestCall& c, uint32_t count, U32Ptr objects, uint32_t wait_type,
                                  uint32_t /*reason*/, uint32_t /*mode*/, uint32_t alertable, TimeoutPtr timeout,
                                  uint32_t /*wait_blocks*/) {
  if (count == 0 || count > kMaxWaitObjects || wait_type > 1 || !objects.valid_range(count * 4))
    return STATUS_INVALID_PARAMETER;
  std::vector<uint32_t> headers(count);
  for (uint32_t i = 0; i < count; ++i) {
    headers[i] = objects.get(c.base)[i];
    if (!valid_header(headers[i])) return STATUS_INVALID_PARAMETER;
  }
  return kernel().dispatcher().wait(headers, static_cast<WaitType>(wait_type), timeout_from_guest(timeout),
                                    alertable != 0);
}

// NtWaitForSingleObjectEx(handle, wait_mode, alertable, timeout*).
uint32_t NtWaitForSingleObjectEx(uint32_t handle, uint32_t /*mode*/, uint32_t alertable, TimeoutPtr timeout) {
  std::shared_ptr<KObject> obj;  // keeps the object (and its pool body) alive during the wait
  uint32_t header = 0;
  if (uint32_t s = resolve(handle, obj, header); s != STATUS_SUCCESS) return s;
  return wait_one(header, alertable, timeout);
}

// NtWaitForMultipleObjectsEx(count, handles*, wait_type, wait_mode, alertable, timeout*).
uint32_t NtWaitForMultipleObjectsEx(GuestCall& c, uint32_t count, U32Ptr handles, uint32_t wait_type,
                                    uint32_t /*mode*/, uint32_t alertable, TimeoutPtr timeout) {
  if (count == 0 || count > kMaxWaitObjects || wait_type > 1 || !handles.valid_range(count * 4))
    return STATUS_INVALID_PARAMETER;
  std::vector<std::shared_ptr<KObject>> objs(count);
  std::vector<uint32_t> headers(count);
  for (uint32_t i = 0; i < count; ++i)
    if (uint32_t s = resolve(handles.get(c.base)[i], objs[i], headers[i]); s != STATUS_SUCCESS) return s;
  return kernel().dispatcher().wait(headers, static_cast<WaitType>(wait_type), timeout_from_guest(timeout),
                                    alertable != 0);
}

// NtSignalAndWaitForSingleObjectEx(signal_handle, wait_handle, alertable, unused, timeout*): signals (event set,
// semaphore +1, mutant release) and waits atomically.
uint32_t NtSignalAndWaitForSingleObjectEx(uint32_t signal_handle, uint32_t wait_handle, uint32_t alertable,
                                          uint32_t /*unused*/, TimeoutPtr timeout) {
  std::shared_ptr<KObject> sig, wt;
  uint32_t sig_header = 0, wait_header = 0;
  if (uint32_t s = resolve(signal_handle, sig, sig_header); s != STATUS_SUCCESS) return s;
  if (uint32_t s = resolve(wait_handle, wt, wait_header); s != STATUS_SUCCESS) return s;
  return kernel().dispatcher().signal_and_wait(sig_header, wait_header, timeout_from_guest(timeout), alertable != 0);
}

}  // namespace exports

LR_KERNEL(NtCreateEvent);
LR_KERNEL(NtSetEvent);
LR_KERNEL(NtPulseEvent);
LR_KERNEL(NtClearEvent);
LR_KERNEL(KeSetEvent);
LR_KERNEL(KeResetEvent);
LR_KERNEL(NtCreateSemaphore);
LR_KERNEL(NtReleaseSemaphore);
LR_KERNEL(KeInitializeMutant);
LR_KERNEL(KeReleaseMutant);
LR_KERNEL(KeWaitForSingleObject);
LR_KERNEL(KeWaitForMultipleObjects);
LR_KERNEL(NtWaitForSingleObjectEx);
LR_KERNEL(NtWaitForMultipleObjectsEx);
LR_KERNEL(NtSignalAndWaitForSingleObjectEx);

}  // namespace lr::kernel
