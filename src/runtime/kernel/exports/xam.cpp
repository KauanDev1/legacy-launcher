// SPDX-License-Identifier: MIT
// Minimal XAM (spec section 10): enumerators, XOVERLAPPED completion, tasks, notification listeners, the local user,
// content probes and the probes of hardware M2 does not have (UI, input, audio). Argument orders, return codes and
// default values follow Xenia (semantic reference only, no code copied): xam_enum.cc, xenumerator.cc, xam_msg.cc
// (XamGetOverlappedResult), kernel_state.cc (CompleteOverlapped*), xam_task.cc, xam_notify.cc, xnotifylistener.cc,
// xam_user.cc, user_profile.{h,cc}, xam_content.cc, xam_content_device.cc, xam_ui.cc, xam_input.cc,
// xboxkrnl_audio.cc.
//
// Deviations from Xenia, all documented in docs/architecture.md:
// - XOVERLAPPED operations complete synchronously (Xenia defers XamEnumerate by 100 ms on a worker).
// - XamCreateEnumeratorHandle is a stub failing with ERROR_INVALID_PARAMETER in Xenia; here it creates an empty
//   enumerator with the X_KENUMERATOR fields Xenia's XEnumerator::Initialize takes, in that order (assumed).
// - Startup notifications are queued on every listener whose mask has the system area (Xenia: the first one only),
//   and they are sign-in changed + storage devices changed (Xenia also sends UI and input-device notifications).
// - XamTaskSchedule writes the task thread's real handle (Xenia: a placeholder 12345).
// - XamUserReadProfileSettings answers unknown settings as unset (from = 0) instead of failing, and always writes the
//   required size on ERROR_INSUFFICIENT_BUFFER.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "runtime/kernel/dispatcher.h"
#include "runtime/kernel/guest_structs.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/objects.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/status.h"
#include "runtime/kernel/threads.h"
#include "runtime/kernel/xam.h"

namespace lr::kernel {
namespace {

using U32Ptr = GuestPtr<be<uint32_t>>;
using U64Ptr = GuestPtr<be<uint64_t>>;
using OverlappedPtr = GuestPtr<XAM_OVERLAPPED>;

constexpr uint32_t kMaxUsers = 4;
constexpr uint32_t kSigninStateLocal = 1;   // eXUserSigninState_SignedInLocally
// XamUserGetXUID type mask bits the local profile answers: offline (1) and online (2, 4), as Xenia, whose local
// profile matches every XUID type.
constexpr uint32_t kUserTypeMask = 1 | 2 | 4;
constexpr uint32_t kContentDeviceDataSize = 0x50;  // X_CONTENT_DEVICE_DATA (Xenia xam_content_device.cc)
constexpr uint32_t kFullLicense = 0xFFFFFFFF;

void put(const GuestCall& c, U32Ptr p, uint32_t v) {
  if (p.valid_range(4)) *p.get(c.base) = v;
}

uint32_t hresult_from_win32(uint32_t e) { return e == 0 ? 0 : (e & 0xFFFF) | 0x80070000u; }

// --- Enumerators ------------------------------------------------------------------------------------------------

class Enumerator final : public KObject {
 public:
  Enumerator(Kernel& k, uint32_t body, uint32_t item_size, uint32_t per, std::vector<uint8_t> items)
      : KObject(ObjectKind::Enumerator), kernel_(k), body_(body), item_size_(item_size), per_(per),
        items_(std::move(items)) {}
  ~Enumerator() override { kernel_.pool_free(body_); }

  // Copies the next batch to guest `buffer`: ERROR_NO_MORE_FILES when all were handed out,
  // ERROR_INSUFFICIENT_BUFFER when the batch does not fit, ERROR_INVALID_PARAMETER for a bad buffer.
  uint32_t write_items(uint8_t* base, uint32_t buffer, uint32_t buffer_size, uint32_t& count) {
    std::lock_guard<std::mutex> lock(mutex_);
    count = 0;
    const size_t total = items_.size() / item_size_;
    const uint32_t n = static_cast<uint32_t>(std::min<size_t>(total - next_, per_));
    if (n == 0) return ERROR_NO_MORE_FILES;
    const uint32_t bytes = n * item_size_;
    if (buffer_size < bytes) return ERROR_INSUFFICIENT_BUFFER;
    if (!GuestPtr<uint8_t>{buffer}.valid_range(bytes)) return ERROR_INVALID_PARAMETER;
    std::memcpy(base + buffer, items_.data() + next_ * item_size_, bytes);
    next_ += n;
    count = n;
    return ERROR_SUCCESS;
  }

 private:
  Kernel& kernel_;
  const uint32_t body_, item_size_, per_;
  std::mutex mutex_;
  std::vector<uint8_t> items_;
  size_t next_ = 0;
};

std::shared_ptr<Enumerator> enumerator_by_handle(uint32_t handle) {
  auto obj = kernel().objects().get(handle);
  if (!obj || obj->kind() != ObjectKind::Enumerator) return nullptr;
  return std::static_pointer_cast<Enumerator>(std::move(obj));
}

// --- Notification listeners -------------------------------------------------------------------------------------

// A waitable listener: its guest body is a notification (manual reset) event, signaled while items are queued.
class NotifyListener final : public KObject {
 public:
  NotifyListener(Kernel& k, uint32_t header, uint64_t mask, uint32_t max_version)
      : KObject(ObjectKind::NotifyListener), kernel_(k), header_(header), mask_(mask), max_version_(max_version) {}
  ~NotifyListener() override { kernel_.pool_free(header_); }

  // Queues `id` when its area is in the mask and its version is not newer than the listener's.
  void enqueue(uint32_t id, uint32_t param) {
    const uint32_t area = (id >> 25) & 0x3F;
    const uint32_t version = (id >> 16) & 0x1FF;
    if ((mask_ & (uint64_t{1} << area)) == 0 || version > max_version_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.emplace_back(id, param);
    kernel_.dispatcher().set_event(header_);
  }

  // The oldest item (match_id 0) or the oldest with id match_id.
  bool dequeue(uint32_t match_id, uint32_t& id, uint32_t& param) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(queue_.begin(), queue_.end(),
                           [&](const auto& n) { return match_id == 0 || n.first == match_id; });
    if (it == queue_.end()) return false;
    id = it->first;
    param = it->second;
    queue_.erase(it);
    if (queue_.empty()) kernel_.dispatcher().reset_event(header_);
    return true;
  }

 private:
  Kernel& kernel_;
  const uint32_t header_;
  const uint64_t mask_;
  const uint32_t max_version_;
  std::mutex mutex_;
  std::deque<std::pair<uint32_t, uint32_t>> queue_;
};

// --- Profile settings -------------------------------------------------------------------------------------------

// X_USER_PROFILE_SETTING (Xenia user_profile.h, 40 bytes) and X_USER_READ_PROFILE_SETTINGS (8 bytes).
struct X_USER_PROFILE_SETTING {
  be<uint32_t> from;        // 0x00: 0 = unset, 1 = default/profile, 2 = title specific
  be<uint32_t> unk04;       // 0x04
  be<uint64_t> user;        // 0x08: user index (high word) or XUID
  be<uint32_t> setting_id;  // 0x10
  be<uint32_t> unk14;       // 0x14
  uint8_t data_type;        // 0x18
  uint8_t data_pad[7];      // 0x19
  be<uint64_t> data_value;  // 0x20: s32 (high word) / s64 / f32 (high word) / f64 / {size, ptr}
};
static_assert(sizeof(X_USER_PROFILE_SETTING) == 0x28);
static_assert(offsetof(X_USER_PROFILE_SETTING, setting_id) == 0x10);
static_assert(offsetof(X_USER_PROFILE_SETTING, data_type) == 0x18);
static_assert(offsetof(X_USER_PROFILE_SETTING, data_value) == 0x20);

constexpr uint32_t kSettingInt32 = 1, kSettingWString = 4, kSettingFloat = 5, kSettingBinary = 6;

uint32_t setting_type(uint32_t id) { return id >> 28; }
uint32_t setting_size(uint32_t id) { return (id >> 16) & 0xFFF; }

// Xenia's UserProfile defaults (int32/float settings; strings and binaries stay unset here).
struct DefaultSetting {
  uint32_t id, value;
};
constexpr DefaultSetting kDefaultSettings[] = {
    {0x10040002, 0},           // XPROFILE_GAMER_YAXIS_INVERSION
    {0x10040003, 3},           // XPROFILE_OPTION_CONTROLLER_VIBRATION
    {0x10040004, 0},           // XPROFILE_GAMERCARD_ZONE
    {0x10040005, 0},           // XPROFILE_GAMERCARD_REGION
    {0x10040006, 0xFA},        // XPROFILE_GAMERCARD_CRED
    {0x5004000B, 0},           // XPROFILE_GAMERCARD_REP (0.0f)
    {0x1004000C, 0},           // XPROFILE_OPTION_VOICE_MUTED
    {0x1004000D, 0},           // XPROFILE_OPTION_VOICE_THRU_SPEAKERS
    {0x1004000E, 0x64},        // XPROFILE_OPTION_VOICE_VOLUME
    {0x10040012, 1},           // XPROFILE_GAMERCARD_TITLES_PLAYED
    {0x10040013, 0},           // XPROFILE_GAMERCARD_ACHIEVEMENTS_EARNED
    {0x10040015, 0},           // XPROFILE_GAMER_DIFFICULTY
    {0x10040018, 0},           // XPROFILE_GAMER_CONTROL_SENSITIVITY
    {0x1004001D, 0xFFFF0000},  // XPROFILE_GAMER_PREFERRED_COLOR_FIRST
    {0x1004001E, 0xFF00FF00},  // XPROFILE_GAMER_PREFERRED_COLOR_SECOND
    {0x10040022, 1},           // XPROFILE_GAMER_ACTION_AUTO_AIM
    {0x10040023, 0},           // XPROFILE_GAMER_ACTION_AUTO_CENTER
    {0x10040024, 0},           // XPROFILE_GAMER_ACTION_MOVEMENT_CONTROL
    {0x10040026, 0},           // XPROFILE_GAMER_RACE_TRANSMISSION
    {0x10040027, 0},           // XPROFILE_GAMER_RACE_CAMERA_LOCATION
    {0x10040028, 0},           // XPROFILE_GAMER_RACE_BRAKE_CONTROL
    {0x10040029, 0},           // XPROFILE_GAMER_RACE_ACCELERATOR_CONTROL
    {0x10040038, 0},           // XPROFILE_GAMERCARD_TITLE_CRED_EARNED
    {0x10040039, 0},           // XPROFILE_GAMERCARD_TITLE_ACHIEVEMENTS_EARNED
};

const DefaultSetting* default_setting(uint32_t id) {
  for (const auto& d : kDefaultSettings)
    if (d.id == id) return &d;
  return nullptr;
}

// Answers through the overlapped (ERROR_IO_PENDING) when there is one, else directly.
uint32_t finish(uint32_t overlapped, uint32_t result, uint32_t length = 0) {
  if (overlapped == 0) return result;
  complete_overlapped(overlapped, result, length);
  return ERROR_IO_PENDING;
}

}  // namespace

Result<uint32_t> create_enumerator(uint32_t item_size, uint32_t items_per_enumerate, std::vector<uint8_t> items,
                                   const X_KENUMERATOR& header, uint32_t extra_size) {
  if (item_size == 0 || items.size() % item_size != 0 || extra_size > (1u << 24))
    return Error{ErrorCode::OutOfRange, "bad enumerator item or extra size", ""};
  Kernel& k = kernel();
  const uint32_t body = k.pool_alloc(static_cast<uint32_t>(sizeof(X_KENUMERATOR)) + extra_size, 0);
  if (body == 0) return Error{ErrorCode::InsufficientResources, "kernel pool exhausted", ""};
  auto* guest = reinterpret_cast<X_KENUMERATOR*>(k.memory().base() + body);
  *guest = header;
  guest->items_per_enumerate = items_per_enumerate;
  auto obj = std::make_shared<Enumerator>(k, body, item_size, items_per_enumerate, std::move(items));
  obj->set_guest_body(body);
  return k.objects().add(std::move(obj));
}

void complete_overlapped(uint32_t overlapped, uint32_t result, uint32_t length) {
  if (!OverlappedPtr{overlapped}.valid_range(sizeof(XAM_OVERLAPPED))) return;
  Kernel& k = kernel();
  auto* ov = OverlappedPtr{overlapped}.get(k.memory().base());
  GuestThread* t = current_thread();
  ov->context = t != nullptr ? t->handle() : 0;
  ov->result = result;
  ov->length = length;
  ov->extended_error = hresult_from_win32(result);
  if (const uint32_t ev = ov->event; ev != 0) {
    // Only an event handle is set; any other object (a semaphore, a thread...) is ignored: the operation itself
    // completed, and the guest sees the result in the overlapped.
    auto obj = k.objects().get(ev);
    if (obj && obj->kind() == ObjectKind::Event && obj->guest_header() != 0)
      k.dispatcher().set_event(obj->guest_header());
  }
  if (const uint32_t routine = ov->completion_routine; routine != 0 && t != nullptr)
    t->queue_apc({routine, result, length, overlapped});
}

namespace exports {

// --- Enumerators and XOVERLAPPED --------------------------------------------------------------------------------

// XamCreateEnumeratorHandle(user_index, app_id, open_message, close_message, extra_size, items_per_enumerate,
// flags, handle*): an enumerator with nothing to enumerate (no XAM app answers its messages in M2).
uint32_t XamCreateEnumeratorHandle(GuestCall& c, uint32_t user_index, uint32_t app_id, uint32_t open_message,
                                   uint32_t close_message, uint32_t extra_size, uint32_t items_per_enumerate,
                                   uint32_t flags, U32Ptr handle_out) {
  if (!handle_out.valid_range(4)) return ERROR_INVALID_PARAMETER;
  X_KENUMERATOR h{};
  h.app_id = app_id;
  h.open_message = open_message;
  h.close_message = close_message;
  h.user_index = user_index;
  h.flags = flags;
  auto r = create_enumerator(1, items_per_enumerate, {}, h, extra_size);
  if (!r) return r.error().code == ErrorCode::OutOfRange ? ERROR_INVALID_PARAMETER : ERROR_FUNCTION_FAILED;
  put(c, handle_out, r.value());
  return ERROR_SUCCESS;
}

// XamEnumerate(handle, flags, buffer, buffer_length, items_returned*, overlapped*).
uint32_t XamEnumerate(GuestCall& c, uint32_t handle, uint32_t /*flags*/, uint32_t buffer, uint32_t buffer_length,
                      U32Ptr items_returned, uint32_t overlapped) {
  auto e = enumerator_by_handle(handle);
  if (!e) return ERROR_INVALID_HANDLE;
  uint32_t count = 0;
  const uint32_t result = buffer == 0 ? ERROR_INVALID_PARAMETER : e->write_items(c.base, buffer, buffer_length, count);
  if (overlapped != 0) return finish(overlapped, result, count);
  put(c, items_returned, count);
  return result;
}

// XamGetPrivateEnumStructureFromHandle(handle, object*): the enumerator's guest X_KENUMERATOR (as Xenia's
// guest_object), with a kernel reference the caller releases through ObDereferenceObject.
uint32_t XamGetPrivateEnumStructureFromHandle(GuestCall& c, uint32_t handle, U32Ptr out) {
  auto e = enumerator_by_handle(handle);
  if (!e) return STATUS_INVALID_HANDLE;
  if (!out.valid_range(4)) return STATUS_INVALID_PARAMETER;  // no reference taken: nobody could release it
  kernel().objects().add_kernel_ref(e);
  *out.get(c.base) = e->guest_body();
  return STATUS_SUCCESS;
}

// XamGetOverlappedResult(overlapped*, length*, wait): a finished result as is; a pending one is ERROR_IO_INCOMPLETE
// unless `wait` is set and it has an event, which is then waited on. *length is written on success only.
uint32_t XamGetOverlappedResult(GuestCall& c, OverlappedPtr overlapped, U32Ptr length, uint32_t wait) {
  if (!overlapped.valid_range(sizeof(XAM_OVERLAPPED))) return ERROR_INVALID_PARAMETER;
  XAM_OVERLAPPED* ov = overlapped.get(c.base);
  uint32_t result = ov->result;
  if (result == ERROR_IO_PENDING) {
    const uint32_t ev = ov->event;
    if (ev == 0 || wait == 0) return ERROR_IO_INCOMPLETE;
    auto obj = kernel().objects().get(ev);
    if (!obj || obj->guest_header() == 0) return ERROR_INVALID_HANDLE;
    const uint32_t h[1] = {obj->guest_header()};
    if (kernel().dispatcher().wait(h, WaitType::Any, Timeout{Timeout::Infinite, 0}, false) != STATUS_WAIT_0)
      return ERROR_FUNCTION_FAILED;
    result = ov->result;
  }
  if (result == ERROR_SUCCESS) put(c, length, ov->length);
  return result;
}

// --- Tasks ------------------------------------------------------------------------------------------------------

// XamTaskSchedule(callback, message, unknown, handle*): runs callback(message) on a new guest thread now.
uint32_t XamTaskSchedule(GuestCall& c, uint32_t callback, uint32_t message, uint32_t /*unknown*/, U32Ptr handle_out) {
  ThreadParams p;
  p.start = callback;
  p.context = message;
  auto t = create_guest_thread(p);
  if (!t) return STATUS_NO_MEMORY;
  put(c, handle_out, t.value()->handle());
  return STATUS_SUCCESS;
}

// --- Notifications ----------------------------------------------------------------------------------------------

// XamNotifyCreateListener(mask (64-bit), max_version): the listener handle, 0 when out of memory. A listener of
// the system area starts with the sign-in (user 0 signed in) and storage-devices notifications queued.
uint32_t XamNotifyCreateListener(uint64_t mask, uint32_t max_version) {
  Kernel& k = kernel();
  const uint32_t header = k.pool_alloc(sizeof(X_KEVENT), 0);
  if (header == 0) return 0;
  auto* h = reinterpret_cast<X_DISPATCHER_HEADER*>(k.memory().base() + header);
  h->type = kNotificationEvent;
  h->size = sizeof(X_KEVENT) / 4;
  h->signal_state = 0;
  h->wait_list_flink = header + 8;
  h->wait_list_blink = header + 8;
  auto listener = std::make_shared<NotifyListener>(k, header, mask, std::min<uint32_t>(max_version, 10));
  listener->set_guest_header(header);
  listener->enqueue(kXnSysSigninChanged, 1);  // bit per signed-in user index
  listener->enqueue(kXnSysStorageDevicesChanged, 0);
  return k.objects().add(std::move(listener));
}

// XNotifyGetNext(handle, match_id, id*, param*): TRUE with the next (or next matching) notification, else FALSE
// with *id = *param = 0.
uint32_t XNotifyGetNext(GuestCall& c, uint32_t handle, uint32_t match_id, U32Ptr id_out, U32Ptr param_out) {
  if (!id_out.valid_range(4)) return 0;
  put(c, id_out, 0);
  put(c, param_out, 0);
  auto obj = kernel().objects().get(handle);
  if (!obj || obj->kind() != ObjectKind::NotifyListener) return 0;
  uint32_t id = 0, param = 0;
  if (!static_cast<NotifyListener&>(*obj).dequeue(match_id, id, param)) return 0;
  put(c, id_out, id);
  put(c, param_out, param);
  return 1;
}

// --- User -------------------------------------------------------------------------------------------------------

uint32_t XamUserGetSigninState(uint32_t user_index) { return user_index == 0 ? kSigninStateLocal : 0; }

// XamUserCheckPrivilege(user_index, privilege, result*): the local profile (user 0, or 0xFF = any user) has every
// privilege (*result = TRUE), as Xenia; other users are not signed in.
uint32_t XamUserCheckPrivilege(GuestCall& c, uint32_t user_index, uint32_t /*privilege*/, U32Ptr result_out) {
  if (!result_out.valid_range(4)) return ERROR_INVALID_PARAMETER;
  if (user_index != 0xFF && user_index >= kMaxUsers) return ERROR_INVALID_PARAMETER;
  if (user_index != 0xFF && user_index != 0) return ERROR_NO_SUCH_USER;
  *result_out.get(c.base) = 1;
  return ERROR_SUCCESS;
}

// XamUserGetDeviceContext(user_index, unknown, context*): *context = 0 always (games treat a failure as 0 too);
// success for the local profile (0) or any user (0xFF), X_E_INVALIDARG otherwise, as Xenia.
uint32_t XamUserGetDeviceContext(GuestCall& c, uint32_t user_index, uint32_t /*unknown*/, U32Ptr context_out) {
  if (context_out.valid_range(4)) *context_out.get(c.base) = 0;
  return (user_index == 0 || (user_index & 0xFF) == 0xFF) ? ERROR_SUCCESS : X_E_INVALIDARG;
}

// XamUserGetXUID(user_index, type_mask, xuid*): the local XUID for any offline/online type bit.
uint32_t XamUserGetXUID(GuestCall& c, uint32_t user_index, uint32_t type_mask, U64Ptr xuid_out) {
  if (!xuid_out.valid_range(8)) return X_E_INVALIDARG;
  if (user_index >= kMaxUsers) return X_E_INVALIDARG;
  uint64_t xuid = 0;
  uint32_t result = X_E_NO_SUCH_USER;
  if (user_index == 0 && (type_mask & kUserTypeMask) != 0) {
    xuid = kLocalXuid;
    result = ERROR_SUCCESS;
  }
  *xuid_out.get(c.base) = xuid;
  return result;
}

// XamUserGetName(user_index, buffer, buffer_length): the name, truncated to min(buffer_length, 16) with its NUL.
uint32_t XamUserGetName(GuestCall& c, uint32_t user_index, uint32_t buffer, uint32_t buffer_length) {
  if (user_index >= kMaxUsers) return X_E_INVALIDARG;
  if (user_index != 0) return X_E_NO_SUCH_USER;
  const uint32_t n = std::min<uint32_t>(buffer_length, 16);
  if (n == 0 || !GuestPtr<char>{buffer}.valid_range(n)) return X_E_INVALIDARG;
  const uint32_t len = std::min<uint32_t>(sizeof(kLocalUserName) - 1, n - 1);
  std::memcpy(c.base + buffer, kLocalUserName, len);
  c.base[buffer + len] = 0;
  return ERROR_SUCCESS;
}

// XamUserGetSigninInfo(user_index, flags, info*): X_USER_SIGNIN_INFO {xuid +0, unk +8, signin_state +0xC,
// unk +0x10, unk +0x14, name[16] +0x18} (0x28 bytes, Xenia xam_user.cc), zeroed first.
uint32_t XamUserGetSigninInfo(GuestCall& c, uint32_t user_index, uint32_t /*flags*/, uint32_t info) {
  if (!GuestPtr<uint8_t>{info}.valid_range(0x28)) return X_E_INVALIDARG;
  uint8_t* p = c.base + info;
  std::memset(p, 0, 0x28);
  if (user_index >= kMaxUsers) return X_E_INVALIDARG;
  if (user_index != 0) return X_E_NO_SUCH_USER;
  *reinterpret_cast<be<uint64_t>*>(p) = kLocalXuid;
  *reinterpret_cast<be<uint32_t>*>(p + 0x0C) = kSigninStateLocal;
  std::memcpy(p + 0x18, kLocalUserName, sizeof(kLocalUserName));
  return ERROR_SUCCESS;
}

// XamUserReadProfileSettings(title_id, user_index, xuid_count, xuids*, setting_count, setting_ids*, buffer_size*,
// buffer, overlapped*). There is no stored profile: Xenia's defaults (from = 1) or unset (from = 0). Size protocol:
// a buffer smaller than needed (or none) -> *buffer_size = needed, ERROR_INSUFFICIENT_BUFFER. Layout:
// X_USER_READ_PROFILE_SETTINGS {count, settings*} then the X_USER_PROFILE_SETTING array, then the room Xenia
// reserves for string/binary data (left unused: such settings are unset).
uint32_t XamUserReadProfileSettings(GuestCall& c, uint32_t /*title_id*/, uint32_t user_index, uint32_t xuid_count,
                                    GuestPtr<be<uint64_t>> xuids, uint32_t setting_count, U32Ptr setting_ids,
                                    U32Ptr buffer_size, uint32_t buffer, uint32_t overlapped) {
  if (setting_count == 0 || setting_count > 32 || !buffer_size.valid_range(4) ||
      !setting_ids.valid_range(setting_count * 4))
    return ERROR_INVALID_PARAMETER;
  const uint32_t size = *buffer_size.get(c.base);
  if (size != 0 && buffer == 0) return ERROR_INVALID_PARAMETER;
  const uint32_t users = xuids ? xuid_count : 1;
  if (users == 0 || users > kMaxUsers || (xuids && !xuids.valid_range(users * 8))) return ERROR_INVALID_PARAMETER;
  const be<uint32_t>* ids = setting_ids.get(c.base);
  uint32_t header_size = 0, data_size = 0;
  for (uint32_t i = 0; i < setting_count; ++i) {
    header_size += sizeof(X_USER_PROFILE_SETTING);
    const uint32_t t = setting_type(ids[i]);
    if (t == kSettingWString || t == kSettingBinary) data_size += setting_size(ids[i]);
  }
  const uint32_t needed = 8 + users * (header_size + data_size);
  if (buffer == 0 || size < needed) {
    *buffer_size.get(c.base) = needed;
    return ERROR_INSUFFICIENT_BUFFER;
  }
  if (!GuestPtr<uint8_t>{buffer}.valid_range(needed)) return ERROR_INVALID_PARAMETER;
  if (!xuids && user_index != 0) return finish(overlapped, ERROR_NO_SUCH_USER);

  std::memset(c.base + buffer, 0, needed);
  auto* out = reinterpret_cast<be<uint32_t>*>(c.base + buffer);
  out[0] = users * setting_count;
  out[1] = buffer + 8;
  auto* s = reinterpret_cast<X_USER_PROFILE_SETTING*>(c.base + buffer + 8);
  for (uint32_t u = 0; u < users; ++u) {
    const uint64_t xuid = xuids ? static_cast<uint64_t>(xuids.get(c.base)[u]) : 0;
    for (uint32_t i = 0; i < setting_count; ++i, ++s) {
      const uint32_t id = ids[i];
      s->user = xuids ? xuid : uint64_t{user_index} << 32;
      s->setting_id = id;
      s->data_type = static_cast<uint8_t>(setting_type(id));
      const DefaultSetting* d = (!xuids || xuid == kLocalXuid) ? default_setting(id) : nullptr;
      if (d != nullptr && (setting_type(id) == kSettingInt32 || setting_type(id) == kSettingFloat)) {
        s->from = 1;
        s->data_value = uint64_t{d->value} << 32;
      }
    }
  }
  return finish(overlapped, ERROR_SUCCESS);
}

// --- Content ----------------------------------------------------------------------------------------------------

// XamContentGetDeviceState(device_id, overlapped*): M2 has no storage device (saves are M4).
uint32_t XamContentGetDeviceState(uint32_t /*device_id*/, uint32_t overlapped) {
  return finish(overlapped, ERROR_DEVICE_NOT_CONNECTED);
}

// XamContentGetDeviceData(device_id, X_CONTENT_DEVICE_DATA*): same, no device to describe; the data is untouched.
uint32_t XamContentGetDeviceData(uint32_t /*device_id*/, uint32_t /*data*/) { return ERROR_DEVICE_NOT_CONNECTED; }

// XamUserCreateAchievementEnumerator(title_id, user_index, xuid, flags, offset, count, buffer_size*, handle*): an
// empty enumerator of X_ACHIEVEMENT_DETAILS (0x24 bytes) items: achievements are not tracked (no profile storage).
constexpr uint32_t kAchievementDetailsSize = 0x24;
uint32_t XamUserCreateAchievementEnumerator(GuestCall& c, uint32_t /*title_id*/, uint32_t user_index, uint64_t /*xuid*/,
                                            uint32_t /*flags*/, uint32_t /*offset*/, uint32_t count,
                                            U32Ptr buffer_size, U32Ptr handle_out) {
  if (user_index >= kMaxUsers || !handle_out.valid_range(4)) return ERROR_INVALID_PARAMETER;
  put(c, buffer_size, kAchievementDetailsSize * count);
  auto r = create_enumerator(kAchievementDetailsSize, count, {});
  if (!r) return ERROR_FUNCTION_FAILED;
  put(c, handle_out, r.value());
  return ERROR_SUCCESS;
}

// XamContentCreateEnumerator(user_index, device_id, content_type, content_flags, max_count, buffer_size*, handle*): an
// empty enumerator of XCONTENT_DATA (0x134 bytes) items: there is no storage device, so no saves or DLC to list.
constexpr uint32_t kContentDataSize = 0x134;
uint32_t XamContentCreateEnumerator(GuestCall& c, uint32_t /*user_index*/, uint32_t /*device_id*/,
                                    uint32_t /*content_type*/, uint32_t /*content_flags*/, uint32_t max_count,
                                    U32Ptr buffer_size, U32Ptr handle_out) {
  if (!handle_out.valid_range(4)) return ERROR_INVALID_PARAMETER;
  put(c, buffer_size, kContentDataSize * max_count);
  auto r = create_enumerator(kContentDataSize, max_count, {});
  if (!r) return ERROR_FUNCTION_FAILED;
  put(c, handle_out, r.value());
  return ERROR_SUCCESS;
}

// XamContentCreateDeviceEnumerator(content_type, content_flags, max_count, buffer_size*, handle*): an empty
// enumerator of X_CONTENT_DEVICE_DATA (0x50 bytes) items.
uint32_t XamContentCreateDeviceEnumerator(GuestCall& c, uint32_t /*content_type*/, uint32_t /*content_flags*/,
                                          uint32_t max_count, U32Ptr buffer_size, U32Ptr handle_out) {
  if (!handle_out.valid_range(4)) return ERROR_INVALID_PARAMETER;
  put(c, buffer_size, kContentDeviceDataSize * max_count);
  auto r = create_enumerator(kContentDeviceDataSize, max_count, {});
  if (!r) return ERROR_FUNCTION_FAILED;
  put(c, handle_out, r.value());
  return ERROR_SUCCESS;
}

// XamContentGetLicenseMask(mask*, overlapped*): the full game (all license bits).
uint32_t XamContentGetLicenseMask(GuestCall& c, U32Ptr mask, uint32_t overlapped) {
  put(c, mask, kFullLicense);
  return finish(overlapped, ERROR_SUCCESS);
}

// --- UI, input and audio probes ---------------------------------------------------------------------------------

// XamShowDirtyDiscErrorUI(user_index): the title gave up on a disc read; on the console this never returns.
void XamShowDirtyDiscErrorUI(uint32_t /*user_index*/) { guest_fatal("dirty disc"); }

// XAudioGetSpeakerConfig(config*): stereo (Xenia's 0x00010001); audio output itself is M4.
uint32_t XAudioGetSpeakerConfig(GuestCall& c, U32Ptr config) {
  put(c, config, 0x00010001);
  return ERROR_SUCCESS;
}

}  // namespace exports

LR_KERNEL(XamCreateEnumeratorHandle);
LR_KERNEL(XamEnumerate);
LR_KERNEL(XamGetPrivateEnumStructureFromHandle);
LR_KERNEL(XamGetOverlappedResult);
LR_KERNEL(XamTaskSchedule);
LR_KERNEL(XamNotifyCreateListener);
LR_KERNEL(XNotifyGetNext);
LR_KERNEL_STUB(XNotifyPositionUI, 0);  // no system UI to position (void import; r3 ignored)
LR_KERNEL(XamUserGetSigninState);
LR_KERNEL(XamUserGetXUID);
LR_KERNEL(XamUserCheckPrivilege);
LR_KERNEL(XamUserGetDeviceContext);
LR_KERNEL(XamUserGetName);
LR_KERNEL(XamUserGetSigninInfo);
LR_KERNEL(XamUserReadProfileSettings);
LR_KERNEL(XamContentGetDeviceState);
LR_KERNEL(XamContentGetDeviceData);
LR_KERNEL(XamContentCreateDeviceEnumerator);
LR_KERNEL(XamContentCreateEnumerator);
LR_KERNEL(XamUserCreateAchievementEnumerator);
LR_KERNEL(XamContentGetLicenseMask);
LR_KERNEL_STUB(XamVoiceIsActiveProcess, 0);  // voice chat is not active in this process (offline, no voice)
LR_KERNEL_STUB(XamVoiceHeadsetPresent, 0);   // no headset on any controller port
LR_KERNEL_STUB(XamIsUIActive, 0);  // no system UI (guide, dialogs) ever opens in M2
LR_KERNEL(XamShowDirtyDiscErrorUI);
LR_KERNEL_STUB(XamInputGetCapabilities, ERROR_DEVICE_NOT_CONNECTED);  // no controller until input lands (M4)
LR_KERNEL_STUB(XamInputGetState, ERROR_DEVICE_NOT_CONNECTED);         // same: nothing to read
LR_KERNEL_STUB(XamInputSetState, ERROR_DEVICE_NOT_CONNECTED);         // same: no motor to drive
LR_KERNEL_STUB(XamInputGetKeystrokeEx, ERROR_DEVICE_NOT_CONNECTED);   // same: no keystroke ever
LR_KERNEL_STUB_FN(XAudioGetSpeakerConfig);  // fixed stereo answer, no audio device until M4

}  // namespace lr::kernel
