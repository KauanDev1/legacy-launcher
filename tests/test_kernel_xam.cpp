// SPDX-License-Identifier: MIT
// Minimal XAM (exports/xam.cpp), video query stubs (exports/video.cpp) and network stubs (exports/net.cpp). The
// XamTaskSchedule callback is a host lambda behind a test GuestGlue keyed by guest address, as in
// test_kernel_threads.cpp.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "runtime/be.h"
#include "runtime/kernel/dispatcher.h"
#include "runtime/kernel/glue.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/status.h"
#include "runtime/kernel/threads.h"
#include "runtime/kernel/xam.h"
#include "support/kernel_fixture.h"

using namespace lr::kernel;
using lr::runtime::be;
using lr::testing::KernelFixture;

namespace {

using GuestFn = std::function<uint64_t(const std::vector<uint64_t>&)>;
std::mutex g_fns_mutex;
std::map<uint32_t, GuestFn> g_fns;

uint64_t test_call(uint8_t*, uint32_t fn, const uint64_t* args, int nargs, uint32_t, uint32_t) {
  GuestFn f;
  {
    std::lock_guard<std::mutex> lock(g_fns_mutex);
    f = g_fns.at(fn);
  }
  return f(std::vector<uint64_t>(args, args + nargs));
}
bool test_has(uint32_t fn) {
  std::lock_guard<std::mutex> lock(g_fns_mutex);
  return g_fns.count(fn) != 0;
}
void def(uint32_t addr, GuestFn f) {
  set_guest_glue({test_call, test_has});
  std::lock_guard<std::mutex> lock(g_fns_mutex);
  g_fns[addr] = std::move(f);
}

uint32_t rd(KernelFixture& f, uint32_t a) { return *f.mem.translate<be<uint32_t>>(a); }
uint64_t rd64(KernelFixture& f, uint32_t a) { return *f.mem.translate<be<uint64_t>>(a); }
void wr(KernelFixture& f, uint32_t a, uint32_t v) { *f.mem.translate<be<uint32_t>>(a) = v; }
float rdf(KernelFixture& f, uint32_t a) { return *f.mem.translate<be<float>>(a); }
uint32_t u32(uint64_t v) { return static_cast<uint32_t>(v); }

uint32_t new_event(KernelFixture& f, uint32_t type = 0, uint32_t initial = 0) {
  const uint32_t out = f.alloc(4);
  REQUIRE(f.call("NtCreateEvent", out, 0u, type, initial) == STATUS_SUCCESS);
  return rd(f, out);
}

uint32_t poll(KernelFixture& f, uint32_t handle) {
  const uint32_t t = f.alloc(8);  // zero timeout: poll
  return u32(f.call("NtWaitForSingleObjectEx", handle, 0u, 0u, t));
}

constexpr uint32_t kErrorNoSuchUser = 1317;
constexpr uint32_t kErrorInsufficientBuffer = 122;
constexpr uint32_t kErrorIoIncomplete = 996;

}  // namespace

TEST_CASE("enumerator returns items then no more files", "[unit][kernel][xam]") {
  KernelFixture f;
  std::vector<uint8_t> items;
  for (uint8_t i = 1; i <= 3; ++i) items.insert(items.end(), 8, i);
  auto h = create_enumerator(8, 2, items);
  REQUIRE(h);
  const uint32_t buf = f.alloc(16), count = f.alloc(4);

  CHECK(f.call("XamEnumerate", h.value(), 0u, buf, 16u, count, 0u) == ERROR_SUCCESS);
  CHECK(rd(f, count) == 2);
  CHECK(*f.mem.translate<uint8_t>(buf) == 1);
  CHECK(*f.mem.translate<uint8_t>(buf + 8) == 2);
  CHECK(f.call("XamEnumerate", h.value(), 0u, buf, 16u, count, 0u) == ERROR_SUCCESS);
  CHECK(rd(f, count) == 1);
  CHECK(*f.mem.translate<uint8_t>(buf) == 3);
  CHECK(f.call("XamEnumerate", h.value(), 0u, buf, 16u, count, 0u) == ERROR_NO_MORE_FILES);
  CHECK(rd(f, count) == 0);

  SECTION("a buffer smaller than one batch is refused") {
    auto h2 = create_enumerator(8, 2, items);
    REQUIRE(h2);
    CHECK(f.call("XamEnumerate", h2.value(), 0u, buf, 8u, count, 0u) == kErrorInsufficientBuffer);
  }
  SECTION("unknown handle") { CHECK(f.call("XamEnumerate", 0xF8001230u, 0u, buf, 16u, count, 0u) == 6u); }
  SECTION("overlapped: completes at once, the event is set and the count is the length") {
    const uint32_t ev = new_event(f);
    const uint32_t ov = f.alloc(sizeof(XAM_OVERLAPPED));
    wr(f, ov + offsetof(XAM_OVERLAPPED, event), ev);
    auto h2 = create_enumerator(8, 2, items);
    REQUIRE(h2);
    CHECK(f.call("XamEnumerate", h2.value(), 0u, buf, 16u, 0u, ov) == ERROR_IO_PENDING);
    CHECK(rd(f, ov + offsetof(XAM_OVERLAPPED, result)) == ERROR_SUCCESS);
    CHECK(rd(f, ov + offsetof(XAM_OVERLAPPED, length)) == 2);
    CHECK(poll(f, ev) == STATUS_WAIT_0);
    const uint32_t len = f.alloc(4);
    CHECK(f.call("XamGetOverlappedResult", ov, len, 1u) == ERROR_SUCCESS);
    CHECK(rd(f, len) == 2);
  }
  SECTION("XamCreateEnumeratorHandle: private structure, nothing to enumerate") {
    const uint32_t out = f.alloc(4), priv = f.alloc(4);
    // (user_index, app_id, open_message, close_message, extra_size, items_per_enumerate, flags, handle*)
    REQUIRE(f.call("XamCreateEnumeratorHandle", 0u, 0xFEu, 0x1234u, 0x5678u, 0x20u, 4u, 0u, out) == ERROR_SUCCESS);
    const uint32_t handle = rd(f, out);
    CHECK(handle >= kHandleBase);
    REQUIRE(f.call("XamGetPrivateEnumStructureFromHandle", handle, priv) == STATUS_SUCCESS);
    const uint32_t obj = rd(f, priv);
    CHECK(rd(f, obj + 0x00) == 0xFEu);    // app_id
    CHECK(rd(f, obj + 0x04) == 0x1234u);  // open_message
    CHECK(rd(f, obj + 0x08) == 0x5678u);  // close_message
    CHECK(rd(f, obj + 0x10) == 4u);       // items_per_enumerate
    f.call("ObDereferenceObject", obj);
    // An invalid out pointer takes no kernel reference.
    CHECK(u32(f.call("XamGetPrivateEnumStructureFromHandle", handle, 0u)) == STATUS_INVALID_PARAMETER);
    CHECK_FALSE(kernel().objects().release_kernel_ref(kernel().objects().get(handle).get()));
    CHECK(f.call("XamEnumerate", handle, 0u, buf, 16u, count, 0u) == ERROR_NO_MORE_FILES);
    CHECK(f.call("NtClose", handle) == STATUS_SUCCESS);
  }
  SECTION("XamUserCreateAchievementEnumerator: no achievements") {
    const uint32_t size = f.alloc(4), out = f.alloc(4);
    // (title_id, user_index, xuid, flags, offset, count, buffer_size*, handle*)
    REQUIRE(f.call("XamUserCreateAchievementEnumerator", 0u, 0u, 0ull, 0x20u, 0u, 2u, size, out) == ERROR_SUCCESS);
    CHECK(rd(f, size) == 2 * 0x24u);
    CHECK(f.call("XamEnumerate", rd(f, out), 0u, buf, 0x48u, count, 0u) == ERROR_NO_MORE_FILES);
    CHECK(f.call("XamUserCreateAchievementEnumerator", 0u, 4u, 0ull, 0x20u, 0u, 2u, size, out) == ERROR_INVALID_PARAMETER);
  }
  SECTION("XamContentCreateEnumerator: no content") {
    const uint32_t size = f.alloc(4), out = f.alloc(4);
    // (user_index, device_id, content_type, content_flags, max_count, buffer_size*, handle*)
    REQUIRE(f.call("XamContentCreateEnumerator", 0u, 0u, 1u, 0u, 3u, size, out) == ERROR_SUCCESS);
    CHECK(rd(f, size) == 3 * 0x134u);
    CHECK(f.call("XamEnumerate", rd(f, out), 0u, buf, 0x39Cu, count, 0u) == ERROR_NO_MORE_FILES);
    CHECK(f.call("XamContentCreateEnumerator", 0u, 0u, 1u, 0u, 3u, size, 0u) == ERROR_INVALID_PARAMETER);
  }
  SECTION("XamContentCreateDeviceEnumerator: no storage devices") {
    const uint32_t size = f.alloc(4), out = f.alloc(4);
    REQUIRE(f.call("XamContentCreateDeviceEnumerator", 1u, 0u, 4u, size, out) == ERROR_SUCCESS);
    CHECK(rd(f, size) == 4 * 0x50u);
    CHECK(f.call("XamEnumerate", rd(f, out), 0u, buf, 0x140u, count, 0u) == ERROR_NO_MORE_FILES);
    CHECK(f.call("XamContentGetDeviceState", 1u, 0u) == ERROR_DEVICE_NOT_CONNECTED);
    const uint32_t ov = f.alloc(sizeof(XAM_OVERLAPPED));
    CHECK(f.call("XamContentGetDeviceState", 1u, ov) == ERROR_IO_PENDING);
    CHECK(rd(f, ov) == ERROR_DEVICE_NOT_CONNECTED);
    const uint32_t dev = f.alloc(0x50);
    wr(f, dev, 0xABABABAB);
    CHECK(f.call("XamContentGetDeviceData", 1u, dev) == ERROR_DEVICE_NOT_CONNECTED);  // no storage device either
    CHECK(rd(f, dev) == 0xABABABABu);
    const uint32_t mask = f.alloc(4);
    CHECK(f.call("XamContentGetLicenseMask", mask, 0u) == ERROR_SUCCESS);
    CHECK(rd(f, mask) == 0xFFFFFFFFu);
    // Only an event is set through the overlapped: a semaphore handle there is ignored.
    const uint32_t sem_out = f.alloc(4);
    REQUIRE(f.call("NtCreateSemaphore", sem_out, 0u, 0u, 1u) == STATUS_SUCCESS);
    const uint32_t ov2 = f.alloc(sizeof(XAM_OVERLAPPED));
    wr(f, ov2 + offsetof(XAM_OVERLAPPED, event), rd(f, sem_out));
    CHECK(f.call("XamContentGetLicenseMask", mask, ov2) == ERROR_IO_PENDING);
    CHECK(rd(f, ov2) == ERROR_SUCCESS);
    CHECK(poll(f, rd(f, sem_out)) == STATUS_TIMEOUT);
  }
}

TEST_CASE("overlapped result waits for completion", "[unit][kernel][xam]") {
  KernelFixture f;
  const uint32_t ev = new_event(f);
  const uint32_t ov = f.alloc(sizeof(XAM_OVERLAPPED)), len = f.alloc(4);
  wr(f, ov + offsetof(XAM_OVERLAPPED, result), ERROR_IO_PENDING);
  wr(f, ov + offsetof(XAM_OVERLAPPED, event), ev);

  CHECK(f.call("XamGetOverlappedResult", ov, len, 0u) == kErrorIoIncomplete);  // no wait asked

  auto waiter = std::async(std::launch::async, [&] {
    // Its own frame: the call runs on another host thread.
    const ImportEntry e = find_import("XamGetOverlappedResult");
    GuestCall c;
    c.base = f.mem.base();
    c.r1 = KernelFixture::kStackPointer - 0x10000;
    c.gpr[0] = ov;
    c.gpr[1] = len;
    c.gpr[2] = 1;
    e.fn(c);
    return static_cast<uint32_t>(c.ret_gpr);
  });
  CHECK(waiter.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout);
  wr(f, ov + offsetof(XAM_OVERLAPPED, result), ERROR_SUCCESS);
  wr(f, ov + offsetof(XAM_OVERLAPPED, length), 5);
  REQUIRE(f.call("NtSetEvent", ev, 0u) == STATUS_SUCCESS);
  CHECK(waiter.get() == ERROR_SUCCESS);
  CHECK(rd(f, len) == 5);

  // Without an event a pending overlapped is incomplete even when asked to wait.
  const uint32_t ov2 = f.alloc(sizeof(XAM_OVERLAPPED));
  wr(f, ov2, ERROR_IO_PENDING);
  CHECK(f.call("XamGetOverlappedResult", ov2, len, 1u) == kErrorIoIncomplete);
  // A finished failure is returned as is and leaves the length alone.
  wr(f, ov2, ERROR_NO_MORE_FILES);
  wr(f, len, 77);
  CHECK(f.call("XamGetOverlappedResult", ov2, len, 1u) == ERROR_NO_MORE_FILES);
  CHECK(rd(f, len) == 77);
}

TEST_CASE("task runs callback on a guest thread", "[unit][kernel][xam]") {
  KernelFixture f;
  constexpr uint32_t kCallback = 0x82000500;
  struct Seen {
    bool guest = false;
    uint64_t arg = 0;
  } seen;
  def(kCallback, [&](const std::vector<uint64_t>& a) -> uint64_t {
    seen.guest = current_thread() != nullptr;
    seen.arg = a.at(0);
    return 0;
  });
  const uint32_t msg = f.alloc(0x1C), handle_out = f.alloc(4);
  REQUIRE(f.call("XamTaskSchedule", kCallback, msg, 0u, handle_out) == STATUS_SUCCESS);
  const uint32_t h = rd(f, handle_out);
  REQUIRE(h >= kHandleBase);
  CHECK(f.call("NtWaitForSingleObjectEx", h, 0u, 0u, 0u) == STATUS_WAIT_0);
  CHECK(seen.guest);
  CHECK(seen.arg == msg);
  CHECK(f.call("NtClose", h) == STATUS_SUCCESS);
}

TEST_CASE("listener gets sign-in notification once", "[unit][kernel][xam]") {
  KernelFixture f;
  const uint32_t h = u32(f.call("XamNotifyCreateListener", uint64_t{1}, 0u));  // XNOTIFY_SYSTEM
  REQUIRE(h >= kHandleBase);
  CHECK(poll(f, h) == STATUS_WAIT_0);  // items pending: signaled (and a poll does not consume it)
  CHECK(poll(f, h) == STATUS_WAIT_0);
  const uint32_t id = f.alloc(4), param = f.alloc(4);

  SECTION("in order, then FALSE") {
    CHECK(f.call("XNotifyGetNext", h, 0u, id, param) == 1);
    CHECK(rd(f, id) == kXnSysSigninChanged);
    CHECK(rd(f, param) == 1);  // bit 0: user 0 signed in
    CHECK(f.call("XNotifyGetNext", h, 0u, id, param) == 1);
    CHECK(rd(f, id) == kXnSysStorageDevicesChanged);
    CHECK(f.call("XNotifyGetNext", h, 0u, id, param) == 0);
    CHECK(rd(f, id) == 0);
    CHECK(poll(f, h) == STATUS_TIMEOUT);
  }
  SECTION("by id") {
    CHECK(f.call("XNotifyGetNext", h, kXnSysStorageDevicesChanged, id, param) == 1);
    CHECK(rd(f, id) == kXnSysStorageDevicesChanged);
    CHECK(f.call("XNotifyGetNext", h, kXnSysStorageDevicesChanged, id, param) == 0);
    CHECK(f.call("XNotifyGetNext", h, kXnSysSigninChanged, id, param) == 1);
    CHECK(poll(f, h) == STATUS_TIMEOUT);
  }
  SECTION("a listener outside the system area gets nothing") {
    const uint32_t h2 = u32(f.call("XamNotifyCreateListener", uint64_t{2}, 0u));
    REQUIRE(h2 >= kHandleBase);
    CHECK(f.call("XNotifyGetNext", h2, 0u, id, param) == 0);
    CHECK(poll(f, h2) == STATUS_TIMEOUT);
  }
  CHECK(f.call("XNotifyGetNext", 0xF8001230u, 0u, id, param) == 0);
  f.call("XNotifyPositionUI", 0u);
  CHECK(f.call("NtClose", h) == STATUS_SUCCESS);
}

TEST_CASE("user is signed in offline as Player", "[unit][kernel][xam]") {
  KernelFixture f;
  CHECK(f.call("XamUserGetSigninState", 0u) == 1);
  CHECK(f.call("XamUserGetSigninState", 1u) == 0);

  const uint32_t xuid = f.alloc(8);
  CHECK(f.call("XamUserGetXUID", 0u, 1u, xuid) == 0);
  CHECK(rd64(f, xuid) == kLocalXuid);
  CHECK(rd64(f, xuid) == 0xE000000000000001ull);
  // As Xenia, the local profile answers every XUID type: offline (1) and online (2, 4).
  wr(f, xuid, 0);
  CHECK(f.call("XamUserGetXUID", 0u, 2u, xuid) == 0);
  CHECK(rd64(f, xuid) == kLocalXuid);
  CHECK(f.call("XamUserGetXUID", 0u, 4u, xuid) == 0);
  CHECK(u32(f.call("XamUserGetXUID", 0u, 0u, xuid)) == 0x80070525u);
  CHECK(u32(f.call("XamUserGetXUID", 1u, 1u, xuid)) == 0x80070525u);
  CHECK(u32(f.call("XamUserGetXUID", 4u, 1u, xuid)) == 0x80070057u);

  const uint32_t name = f.alloc(16);
  CHECK(f.call("XamUserGetName", 0u, name, 16u) == 0);
  CHECK(std::string(f.mem.translate<char>(name)) == "Player");
  CHECK(u32(f.call("XamUserGetName", 2u, name, 16u)) == 0x80070525u);

  const uint32_t info = f.alloc(0x28);
  CHECK(f.call("XamUserGetSigninInfo", 0u, 0u, info) == 0);
  CHECK(rd64(f, info) == kLocalXuid);
  CHECK(rd(f, info + 0x0C) == 1);
  CHECK(std::string(f.mem.translate<char>(info + 0x18)) == "Player");
  CHECK(u32(f.call("XamUserGetSigninInfo", 1u, 0u, info)) == 0x80070525u);
  CHECK(u32(f.call("XamUserGetSigninInfo", 4u, 0u, info)) == 0x80070057u);
  CHECK(rd64(f, info) == 0);

  SECTION("XamUserReadProfileSettings: size query, then defaults") {
    // (title_id, user_index, xuid_count, xuids, setting_count, setting_ids, buffer_size*, buffer, overlapped)
    const uint32_t ids = f.alloc(8), size = f.alloc(4);
    wr(f, ids, 0x10040003);      // XPROFILE_OPTION_CONTROLLER_VIBRATION (default 3)
    wr(f, ids + 4, 0x63E83FFF);  // XPROFILE_TITLE_SPECIFIC1 (binary, 1000 bytes, unset)
    CHECK(f.call("XamUserReadProfileSettings", 0u, 0u, 0u, 0u, 2u, ids, size, 0u, 0u) == kErrorInsufficientBuffer);
    const uint32_t need = rd(f, size);
    CHECK(need == 8 + 2 * 0x28 + 1000);
    const uint32_t buf = f.alloc(need);
    CHECK(f.call("XamUserReadProfileSettings", 0u, 0u, 0u, 0u, 2u, ids, size, buf, 0u) == ERROR_SUCCESS);
    CHECK(rd(f, buf) == 2);
    const uint32_t s = rd(f, buf + 4);
    CHECK(s == buf + 8);
    CHECK(rd(f, s + 0x00) == 1);  // from: default value
    CHECK(rd(f, s + 0x10) == 0x10040003u);
    CHECK(*f.mem.translate<uint8_t>(s + 0x18) == 1);  // INT32
    CHECK(rd(f, s + 0x20) == 3);
    CHECK(rd(f, s + 0x28) == 0);  // unset
    CHECK(rd(f, s + 0x28 + 0x10) == 0x63E83FFFu);
    CHECK(*f.mem.translate<uint8_t>(s + 0x28 + 0x18) == 6);  // BINARY
    CHECK(rd(f, s + 0x28 + 0x20) == 0);                      // no data

    CHECK(f.call("XamUserReadProfileSettings", 0u, 1u, 0u, 0u, 2u, ids, size, buf, 0u) == kErrorNoSuchUser);
    const uint32_t ov = f.alloc(sizeof(XAM_OVERLAPPED));
    CHECK(f.call("XamUserReadProfileSettings", 0u, 0u, 0u, 0u, 2u, ids, size, buf, ov) == ERROR_IO_PENDING);
    CHECK(rd(f, ov) == ERROR_SUCCESS);
    CHECK(f.call("XamUserReadProfileSettings", 0u, 0u, 0u, 0u, 0u, ids, size, buf, 0u) == 87u);
  }
}

TEST_CASE("XGetVideoMode reports 1280x720 widescreen", "[unit][kernel][xam][video]") {
  KernelFixture f;
  for (const char* fn : {"XGetVideoMode", "VdQueryVideoMode"}) {
    const uint32_t m = f.alloc(sizeof(X_VIDEO_MODE));
    std::memset(f.mem.translate<uint8_t>(m), 0xCC, sizeof(X_VIDEO_MODE));
    f.call(fn, m);
    CHECK(rd(f, m + offsetof(X_VIDEO_MODE, display_width)) == 1280);
    CHECK(rd(f, m + offsetof(X_VIDEO_MODE, display_height)) == 720);
    CHECK(rd(f, m + offsetof(X_VIDEO_MODE, is_interlaced)) == 0);
    CHECK(rd(f, m + offsetof(X_VIDEO_MODE, is_widescreen)) == 1);
    CHECK(rd(f, m + offsetof(X_VIDEO_MODE, is_hi_def)) == 1);
    CHECK(rdf(f, m + offsetof(X_VIDEO_MODE, refresh_rate)) == 60.0f);
    CHECK(rd(f, m + offsetof(X_VIDEO_MODE, reserved) + 8) == 0);
  }
  CHECK(f.call("VdQueryVideoFlags") == 3);  // widescreen | >= 1024 wide
  CHECK(f.call("XGetAVPack") == 6);
  CHECK(f.call("VdIsHSIOTrainingSucceeded") == 1);
  CHECK(f.call("XGetVideoCapabilities") == 0);

  const uint32_t di = f.alloc(sizeof(X_DISPLAY_INFO));
  f.call("VdGetCurrentDisplayInformation", di);
  CHECK(*f.mem.translate<be<uint16_t>>(di + offsetof(X_DISPLAY_INFO, front_buffer_width)) == 1280);
  CHECK(*f.mem.translate<be<uint16_t>>(di + offsetof(X_DISPLAY_INFO, display_height)) == 720);
  CHECK(rdf(f, di + offsetof(X_DISPLAY_INFO, display_refresh_rate)) == 60.0f);

  const uint32_t type = f.alloc(4), power = f.alloc(4);
  f.call("VdGetCurrentDisplayGamma", type, power);
  CHECK(rd(f, type) == 2);
  CHECK(rdf(f, power) > 2.2f);

  CHECK(f.call("XamIsUIActive") == 0);
  const uint32_t caps = f.alloc(0x20);
  CHECK(f.call("XamInputGetCapabilities", 0u, 1u, caps) == ERROR_DEVICE_NOT_CONNECTED);
  const uint32_t spk = f.alloc(4);
  CHECK(f.call("XAudioGetSpeakerConfig", spk) == ERROR_SUCCESS);
  CHECK(rd(f, spk) == 0x00010001u);
}

TEST_CASE("WSAStartup stub returns 0", "[unit][kernel][xam][net]") {
  KernelFixture f;
  const uint32_t data = f.alloc(0x190);
  // NetDll_* take the caller in r3.
  CHECK(f.call("NetDll_WSAStartup", 1u, 0x0202u, data) == 0);
  CHECK(*f.mem.translate<be<uint16_t>>(data) == 0x0202);
  CHECK(f.call("NetDll_XNetStartup", 1u, 0u) == 0);
  CHECK(f.call("NetDll_XNetGetEthernetLinkStatus", 1u) == 0);  // no link
}

TEST_CASE("XNetGet*XnAddr report no network address (offline)", "[unit][kernel][xam][net]") {
  KernelFixture f;
  const uint32_t addr = f.alloc(36);
  std::memset(f.mem.translate<uint8_t>(addr), 0xAB, 36);
  CHECK(f.call("NetDll_XNetGetTitleXnAddr", 1u, addr) == 1);  // XNET_GET_XNADDR_NONE
  for (uint32_t i = 0; i < 36; ++i) CHECK(f.mem.translate<uint8_t>(addr)[i] == 0);
  std::memset(f.mem.translate<uint8_t>(addr), 0xAB, 36);
  CHECK(f.call("NetDll_XNetGetDebugXnAddr", 1u, addr) == 1);
  CHECK(f.mem.translate<uint8_t>(addr)[35] == 0);
}

// The game sets the background download mode before its title screen; offline there is no download service, so the
// mode is accepted and reads back as 0 (kernel-imports.md, "HLE mínima" of the LIVE group).
TEST_CASE("background download mode: set succeeds, mode reads 0", "[unit][kernel][xam][net]") {
  KernelFixture f;
  CHECK(f.call("XamBackgroundDownloadSetMode", 1u) == 0);
  CHECK(f.call("XamBackgroundDownloadGetMode") == 0);
}

TEST_CASE("voice chat: inactive, no headset", "[unit][kernel][xam]") {
  KernelFixture f;
  CHECK(f.call("XamVoiceIsActiveProcess") == 0);
  CHECK(f.call("XamVoiceHeadsetPresent", 0u) == 0);
}

TEST_CASE("XamUserCheckPrivilege grants the local profile everything", "[unit][kernel][xam]") {
  KernelFixture f;
  const uint32_t out = f.alloc(4);
  *f.mem.translate<be<uint32_t>>(out) = 7;
  CHECK(f.call("XamUserCheckPrivilege", 0u, 0xFCu, out) == ERROR_SUCCESS);
  CHECK(*f.mem.translate<be<uint32_t>>(out) == 1u);
  CHECK(f.call("XamUserCheckPrivilege", 1u, 0xFCu, out) == ERROR_NO_SUCH_USER);
  CHECK(f.call("XamUserCheckPrivilege", 0u, 0xFCu, 0u) == ERROR_INVALID_PARAMETER);
}

TEST_CASE("XamUserGetDeviceContext gives the local profile a zero context", "[unit][kernel][xam]") {
  KernelFixture f;
  const uint32_t out = f.alloc(4);
  *f.mem.translate<be<uint32_t>>(out) = 7;
  CHECK(f.call("XamUserGetDeviceContext", 0u, 0u, out) == ERROR_SUCCESS);
  CHECK(*f.mem.translate<be<uint32_t>>(out) == 0u);
  *f.mem.translate<be<uint32_t>>(out) = 7;
  CHECK(static_cast<uint32_t>(f.call("XamUserGetDeviceContext", 2u, 0u, out)) == X_E_INVALIDARG);
  CHECK(*f.mem.translate<be<uint32_t>>(out) == 0u);
}

TEST_CASE("input reports no controller connected", "[unit][kernel][xam]") {
  KernelFixture f;
  const uint32_t user = f.alloc(4), buf = f.alloc(0x40);
  *f.mem.translate<be<uint32_t>>(user) = 0xFF;
  CHECK(f.call("XamInputGetKeystrokeEx", user, 0x400000FFu, buf) == ERROR_DEVICE_NOT_CONNECTED);
  CHECK(f.call("XamInputGetState", 0u, 0u, buf) == ERROR_DEVICE_NOT_CONNECTED);
  CHECK(f.call("XamInputSetState", 0u, 0u, buf) == ERROR_DEVICE_NOT_CONNECTED);
}

TEST_CASE("XMsg requests to the XAM app (0xFE) complete at once", "[unit][kernel][xam]") {
  KernelFixture f;
  constexpr uint32_t kXamApp = 0xFE, kXFail = 0x80004005, kXNotFound = 0x80070490;
  // 0x00022005: 8-byte buffer, success, nothing written.
  const uint32_t buf = f.alloc(0x4C);
  wr(f, buf, 0x11111111);
  wr(f, buf + 4, 0x22222222);
  CHECK(f.call("XMsgStartIORequest", kXamApp, 0x00022005u, 0u, buf, 8u) == 0u);
  CHECK(rd(f, buf) == 0x11111111u);
  // Through an overlapped: ERROR_IO_PENDING, the result lands in the overlapped.
  const uint32_t ov = f.alloc(sizeof(XAM_OVERLAPPED));
  wr(f, ov, 0xFFFFFFFF);
  CHECK(f.call("XMsgStartIORequest", kXamApp, 0x00022005u, ov, buf, 8u) == ERROR_IO_PENDING);
  CHECK(rd(f, ov + offsetof(XAM_OVERLAPPED, result)) == 0u);
  // 0x00020021: 0x4C-byte buffer, writes 0 through the pointer at +0x44.
  const uint32_t out = f.alloc(4);
  wr(f, out, 0x33333333);
  wr(f, buf + 0x44, out);
  CHECK(f.call("XMsgStartIORequest", kXamApp, 0x00020021u, 0u, buf, 0x4Cu) == 0u);
  CHECK(rd(f, out) == 0u);
  CHECK(f.call("XMsgInProcessCall", kXamApp, 0x00021012u, 0u, 0u) == 0u);
  // Unknown messages fail; unknown apps are "not found" (X_E_INVALIDARG from XMsgStartIORequest, as Xenia).
  CHECK(static_cast<uint32_t>(f.call("XMsgStartIORequest", kXamApp, 0x00029999u, 0u, buf, 8u)) == kXFail);
  CHECK(static_cast<uint32_t>(f.call("XMsgStartIORequest", 0x42u, 0x00022005u, 0u, buf, 8u)) == X_E_INVALIDARG);
  CHECK(static_cast<uint32_t>(f.call("XMsgInProcessCall", 0x42u, 0x00021012u, 0u, 0u)) == kXNotFound);
  CHECK(f.call("XMsgSystemProcessCall", kXamApp, 0x00022005u, buf, 8u) == 0u);
}
