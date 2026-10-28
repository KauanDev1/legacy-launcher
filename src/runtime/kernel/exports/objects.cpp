// SPDX-License-Identifier: MIT
// Object and handle imports over lr::kernel::ObjectTable. Argument order and status codes follow Xenia's
// xboxkrnl_ob.cc and xboxkrnl_io.cc (semantic reference only, no code copied).
#include <cstdint>

#include "runtime/kernel/guest_structs.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/objects.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/status.h"
#include "runtime/kernel/variables.h"

namespace lr::kernel {
namespace {

using U32Ptr = GuestPtr<be<uint32_t>>;

constexpr uint32_t DUPLICATE_CLOSE_SOURCE = 1;

be<uint32_t>* deref(const GuestCall& c, U32Ptr p) { return p.valid_range(4) ? p.get(c.base) : nullptr; }

}  // namespace

namespace exports {

// NtClose(handle): STATUS_INVALID_HANDLE when closed twice or never valid.
uint32_t NtClose(uint32_t handle) { return kernel().objects().close(handle); }

// NtDuplicateObject(handle, new_handle*, options): the new handle refers to the same object. With
// DUPLICATE_CLOSE_SOURCE the source handle is closed afterwards.
uint32_t NtDuplicateObject(GuestCall& c, uint32_t handle, U32Ptr new_handle, uint32_t options) {
  auto& table = kernel().objects();
  auto dup = table.duplicate(handle);
  if (!dup) return STATUS_INVALID_HANDLE;
  if (be<uint32_t>* out = deref(c, new_handle)) *out = dup.value();
  if ((options & DUPLICATE_CLOSE_SOURCE) != 0) (void)table.close(handle);
  return STATUS_SUCCESS;
}

// ObReferenceObjectByHandle(handle, object_type*, out_object*): writes the object's guest address and takes a
// kernel reference released by ObDereferenceObject. object_type 0 accepts any kind; otherwise it must equal the
// address of the kind's Ex*ObjectType variable block. An object without a guest address (guest_object() == 0, e.g. a
// bare KObject) is refused with STATUS_OBJECT_TYPE_MISMATCH: nothing is written and no kernel reference is taken, since
// ObDereferenceObject could never find it again to release it.
uint32_t ObReferenceObjectByHandle(GuestCall& c, uint32_t handle, uint32_t object_type, U32Ptr out_object) {
  auto& table = kernel().objects();
  auto obj = table.get(handle);
  if (!obj) return STATUS_INVALID_HANDLE;
  if (object_type != 0) {
    const std::string_view var = object_type_variable(obj->kind());
    if (var.empty() || variable_address(var) != object_type) return STATUS_OBJECT_TYPE_MISMATCH;
  }
  if (obj->guest_object() == 0) return STATUS_OBJECT_TYPE_MISMATCH;
  if (be<uint32_t>* out = deref(c, out_object)) *out = obj->guest_object();
  table.add_kernel_ref(obj);
  return STATUS_SUCCESS;
}

// ObDereferenceObject(object): releases a reference taken by ObReferenceObjectByHandle (found by guest address).
// Unknown objects are ignored.
void ObDereferenceObject(uint32_t object) {
  auto& table = kernel().objects();
  if (auto obj = table.by_guest_header(object)) (void)table.release_kernel_ref(obj.get());
}

// ObOpenObjectByPointer(object, out_handle*): a new handle for a referenced object found by guest address.
uint32_t ObOpenObjectByPointer(GuestCall& c, uint32_t object, U32Ptr out_handle) {
  auto& table = kernel().objects();
  auto obj = table.by_guest_header(object);
  if (!obj) return STATUS_INVALID_HANDLE;
  if (be<uint32_t>* out = deref(c, out_handle)) *out = table.add(std::move(obj));
  return STATUS_SUCCESS;
}

}  // namespace exports

LR_KERNEL(NtClose);
LR_KERNEL(NtDuplicateObject);
LR_KERNEL(ObReferenceObjectByHandle);
LR_KERNEL(ObDereferenceObject);
LR_KERNEL(ObOpenObjectByPointer);
// Named kernel objects (object directory lookups) are not modelled; the game does not use them in M2.
LR_KERNEL_STUB(ObOpenObjectByName, STATUS_NOT_IMPLEMENTED);
// Guest-created object types are not modelled; the game does not use them in M2.
LR_KERNEL_STUB(ObCreateObject, STATUS_NOT_IMPLEMENTED);

}  // namespace lr::kernel
