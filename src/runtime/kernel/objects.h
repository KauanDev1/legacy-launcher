// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/result.h"

// Xbox-style kernel objects and the handle table that owns them (spec section 8). Later tasks derive their object
// kinds from KObject (events, semaphores, threads, files, enumerators) and put them in Kernel::objects().

namespace lr::kernel {

enum class ObjectKind { Event, Semaphore, Mutant, Timer, Thread, File, Enumerator, NotifyListener, Other };

// Name of the Ex*ObjectType / IoFileObjectType kernel variable that ObReferenceObjectByHandle compares against
// the guest-supplied object type pointer; empty for kinds without one.
std::string_view object_type_variable(ObjectKind kind);

class KObject : public std::enable_shared_from_this<KObject> {
 public:
  explicit KObject(ObjectKind k) : kind_(k) {}
  KObject(const KObject&) = delete;
  KObject& operator=(const KObject&) = delete;
  virtual ~KObject();

  ObjectKind kind() const { return kind_; }

  // Guest address of the object's DISPATCHER_HEADER, 0 when it is not waitable / has none. Setting it registers
  // the object for ObjectTable::by_guest_header (until the object is destroyed or the header changes). The
  // object must be owned by a shared_ptr when a non-zero header is set.
  uint32_t guest_header() const;
  void set_guest_header(uint32_t guest);

  // The address handed to guest code by ObReferenceObjectByHandle (what ObDereferenceObject takes back):
  // the header address for now, 0 for objects without one.
  uint32_t guest_object() const { return guest_header(); }

 private:
  const ObjectKind kind_;
  mutable std::mutex mutex_;
  uint32_t guest_header_ = 0;
};

inline constexpr uint32_t kHandleBase = 0xF8000000;
inline constexpr uint32_t kCurrentThread = 0xFFFFFFFE;
inline constexpr uint32_t kCurrentProcess = 0xFFFFFFFF;

// Thread-safe. Handles are kHandleBase + 4 * slot; closed slots are reused (lowest free first).
class ObjectTable {
 public:
  uint32_t add(std::shared_ptr<KObject> obj);
  // The object behind `handle`; nullptr for closed, unknown or garbage handles. kCurrentThread resolves through
  // the hook set by set_current_thread_hook (nullptr until the thread task installs one); kCurrentProcess is
  // not an object here.
  std::shared_ptr<KObject> get(uint32_t handle) const;
  // STATUS_SUCCESS, or STATUS_INVALID_HANDLE for a closed/unknown handle. Pseudo handles close successfully
  // without effect.
  uint32_t close(uint32_t handle);
  // A new handle to the same object (the object lives until every handle is closed).
  Result<uint32_t> duplicate(uint32_t handle);
  // The live object whose guest_header() is `guest`, or nullptr.
  std::shared_ptr<KObject> by_guest_header(uint32_t guest) const;

  // Kernel references taken by ObReferenceObjectByHandle: they keep the object alive after its handles are
  // closed until a matching release_kernel_ref (ObDereferenceObject).
  void add_kernel_ref(const std::shared_ptr<KObject>& obj);
  // False when obj holds no kernel reference.
  bool release_kernel_ref(const KObject* obj);

  void set_current_thread_hook(std::function<std::shared_ptr<KObject>()> hook);

 private:
  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<KObject>> slots_;
  struct Pin {
    std::shared_ptr<KObject> obj;
    uint32_t count = 0;
  };
  std::unordered_map<const KObject*, Pin> pins_;
  std::function<std::shared_ptr<KObject>()> current_thread_hook_;
};

}  // namespace lr::kernel
