// SPDX-License-Identifier: MIT
#include "runtime/kernel/objects.h"

#include "runtime/kernel/status.h"

namespace lr::kernel {

namespace {

// guest header -> object, for ObjectTable::by_guest_header. Process-wide: guest addresses are unique per kernel.
struct HeaderRegistry {
  std::mutex mutex;
  std::unordered_map<uint32_t, std::weak_ptr<KObject>> map;
};
HeaderRegistry& registry() {
  static HeaderRegistry* r = new HeaderRegistry;  // leaked: objects may die during process exit
  return *r;
}

bool is_pseudo(uint32_t h) { return h == kCurrentThread || h == kCurrentProcess; }

}  // namespace

std::string_view object_type_variable(ObjectKind kind) {
  switch (kind) {
    case ObjectKind::Event: return "ExEventObjectType";
    case ObjectKind::Semaphore: return "ExSemaphoreObjectType";
    case ObjectKind::Mutant: return "ExMutantObjectType";
    case ObjectKind::Timer: return "ExTimerObjectType";
    case ObjectKind::Thread: return "ExThreadObjectType";
    case ObjectKind::File: return "IoFileObjectType";
    default: return {};
  }
}

KObject::~KObject() {
  const uint32_t h = guest_header_;
  if (h == 0) return;
  HeaderRegistry& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  auto it = r.map.find(h);
  if (it != r.map.end() && it->second.expired()) r.map.erase(it);
}

uint32_t KObject::guest_header() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return guest_header_;
}

void KObject::set_guest_header(uint32_t guest) {
  HeaderRegistry& r = registry();
  std::lock_guard<std::mutex> rl(r.mutex);
  std::lock_guard<std::mutex> lock(mutex_);
  if (guest_header_ != 0) {
    auto it = r.map.find(guest_header_);
    if (it != r.map.end() && it->second.lock().get() == this) r.map.erase(it);
  }
  guest_header_ = guest;
  if (guest != 0) r.map[guest] = weak_from_this();
}

uint32_t ObjectTable::add(std::shared_ptr<KObject> obj) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (size_t i = 0; i < slots_.size(); ++i) {
    if (!slots_[i]) {
      slots_[i] = std::move(obj);
      return kHandleBase + 4 * static_cast<uint32_t>(i);
    }
  }
  slots_.push_back(std::move(obj));
  return kHandleBase + 4 * static_cast<uint32_t>(slots_.size() - 1);
}

std::shared_ptr<KObject> ObjectTable::get(uint32_t handle) const {
  std::function<std::shared_ptr<KObject>()> hook;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (handle == kCurrentThread) {
      hook = current_thread_hook_;
    } else {
      if (handle < kHandleBase || (handle & 3) != 0) return nullptr;
      const uint64_t slot = (handle - kHandleBase) / 4;
      return slot < slots_.size() ? slots_[slot] : nullptr;
    }
  }
  return hook ? hook() : nullptr;
}

uint32_t ObjectTable::close(uint32_t handle) {
  std::shared_ptr<KObject> dying;  // destroyed outside the lock
  std::lock_guard<std::mutex> lock(mutex_);
  if (is_pseudo(handle)) return STATUS_SUCCESS;
  if (handle < kHandleBase || (handle & 3) != 0) return STATUS_INVALID_HANDLE;
  const uint64_t slot = (handle - kHandleBase) / 4;
  if (slot >= slots_.size() || !slots_[slot]) return STATUS_INVALID_HANDLE;
  dying = std::move(slots_[slot]);
  slots_[slot] = nullptr;
  return STATUS_SUCCESS;
}

Result<uint32_t> ObjectTable::duplicate(uint32_t handle) {
  auto obj = get(handle);
  if (!obj) return Error{ErrorCode::NotFound, "invalid handle", ""};
  return add(std::move(obj));
}

std::shared_ptr<KObject> ObjectTable::by_guest_header(uint32_t guest) const {
  if (guest == 0) return nullptr;
  HeaderRegistry& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  auto it = r.map.find(guest);
  return it == r.map.end() ? nullptr : it->second.lock();
}

void ObjectTable::add_kernel_ref(const std::shared_ptr<KObject>& obj) {
  std::lock_guard<std::mutex> lock(mutex_);
  Pin& p = pins_[obj.get()];
  p.obj = obj;
  ++p.count;
}

bool ObjectTable::release_kernel_ref(const KObject* obj) {
  std::shared_ptr<KObject> dying;
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = pins_.find(obj);
  if (it == pins_.end()) return false;
  if (--it->second.count == 0) {
    dying = std::move(it->second.obj);
    pins_.erase(it);
  }
  return true;
}

void ObjectTable::set_current_thread_hook(std::function<std::shared_ptr<KObject>()> hook) {
  std::lock_guard<std::mutex> lock(mutex_);
  current_thread_hook_ = std::move(hook);
}

}  // namespace lr::kernel
