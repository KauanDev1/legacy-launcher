// SPDX-License-Identifier: MIT
// Silent audio driver (exports/audio.cpp), message box reporting and relaunch (exports/xam_ui.cpp), absent Kinect
// (exports/nui.cpp).
#include <catch2/catch_test_macros.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

#include "runtime/be.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/status.h"
#include "runtime/kernel/xam.h"
#include "runtime/memory_map.h"
#include "support/kernel_fixture.h"

using namespace lr::kernel;
using lr::runtime::be;
using lr::testing::KernelFixture;

namespace {

struct ChildResult {
  int code;
  std::string err;
};

// Runs `body` in a forked child with stderr captured; returns its exit code (-1 when signaled) and stderr.
ChildResult in_child(const std::function<void()>& body) {
  int fds[2];
  REQUIRE(::pipe(fds) == 0);
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::dup2(fds[1], 2);
    ::close(fds[0]);
    body();
    ::_exit(99);
  }
  ::close(fds[1]);
  std::string out;
  char buf[256];
  for (ssize_t n; (n = ::read(fds[0], buf, sizeof buf)) > 0;) out.append(buf, static_cast<size_t>(n));
  ::close(fds[0]);
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, out};
}

uint32_t wstr(KernelFixture& f, const char16_t* s) {
  const size_t n = std::char_traits<char16_t>::length(s);
  const uint32_t a = f.alloc(static_cast<uint32_t>((n + 1) * 2));
  auto* p = f.mem.translate<be<uint16_t>>(a);
  for (size_t i = 0; i < n; ++i) p[i] = static_cast<uint16_t>(s[i]);
  return a;
}

uint32_t rd(KernelFixture& f, uint32_t a) { return *f.mem.translate<be<uint32_t>>(a); }
void wr32(KernelFixture& f, uint32_t a, uint32_t v) { *f.mem.translate<be<uint32_t>>(a) = v; }

}  // namespace

TEST_CASE("silent audio driver answers like a driver", "[unit][kernel][audio]") {
  KernelFixture f;
  const uint32_t cb = f.alloc(8), drv = f.alloc(4), vol = f.alloc(4);
  CHECK(f.call("XAudioRegisterRenderDriverClient", cb, drv) == 0);
  CHECK(rd(f, drv) == 0x41550000);
  CHECK(f.call("XAudioSubmitRenderDriverFrame", 0x41550000u, f.alloc(16)) == 0);
  CHECK(f.call("XAudioUnregisterRenderDriverClient", 0x41550000u) == 0);
  CHECK(f.call("XAudioGetVoiceCategoryVolume", 1u, vol) == 0);
  CHECK(*f.mem.translate<be<float>>(vol) == 1.0f);
  // The callback pointer is never read (as Xenia, nothing validates it); only the handle output must be writable.
  wr32(f, drv, 0);
  CHECK(f.call("XAudioRegisterRenderDriverClient", 0x1234u, drv) == 0);
  CHECK(rd(f, drv) == 0x41550000);
  CHECK(static_cast<uint32_t>(f.call("XAudioRegisterRenderDriverClient", cb, 0u)) == X_E_INVALIDARG);
}

// The game creates XMA decoder contexts before its title screen. No decoder runs them (silent, M4): a context is a
// zeroed block in physical memory the game fills itself, and releasing it frees the block.
TEST_CASE("XMA contexts are zeroed physical blocks that are never decoded", "[unit][kernel][audio]") {
  KernelFixture f;
  const uint32_t out1 = f.alloc(4), out2 = f.alloc(4);
  REQUIRE(f.call("XMACreateContext", out1) == 0);
  REQUIRE(f.call("XMACreateContext", out2) == 0);
  const uint32_t a = rd(f, out1), b = rd(f, out2);
  CHECK(a >= lr::runtime::kPhysicalViewA0);
  CHECK(b >= lr::runtime::kPhysicalViewA0);
  CHECK(a != b);
  CHECK(a % 64 == 0);
  CHECK(f.call("MmQueryAllocationSize", a) >= 64u);  // a real allocation, not a made-up address
  for (uint32_t k = 0; k < 64; k += 4) CHECK(rd(f, a + k) == 0);

  // A released context is freed; a new one is zeroed again even where the game left data.
  for (uint32_t k = 0; k < 64; k += 4) wr32(f, a + k, 0xDEADBEEF);
  CHECK(f.call("XMAReleaseContext", a) == 0);
  CHECK(f.call("MmQueryAllocationSize", a) == 0u);
  REQUIRE(f.call("XMACreateContext", out1) == 0);
  const uint32_t c = rd(f, out1);
  for (uint32_t k = 0; k < 64; k += 4) CHECK(rd(f, c + k) == 0);

  // Releasing something that is not a context is ignored; an unwritable output is an error.
  CHECK(f.call("XMAReleaseContext", 0u) == 0);
  CHECK(f.call("XMAReleaseContext", b + 4) == 0);
  CHECK(f.call("MmQueryAllocationSize", b) >= 64u);
  CHECK(static_cast<uint32_t>(f.call("XMACreateContext", 0u)) == STATUS_INVALID_PARAMETER);
}

TEST_CASE("XamShowMessageBoxUI reports to the terminal and presses the first button", "[unit][kernel][xam]") {
  KernelFixture f;
  const uint32_t title = wstr(f, u"Title"), text = wstr(f, u"Caf\u00E9 failed");
  const uint32_t buttons = f.alloc(8), result = f.alloc(4), ov = f.alloc(sizeof(XAM_OVERLAPPED));
  *f.mem.translate<be<uint32_t>>(buttons) = wstr(f, u"OK");
  *f.mem.translate<be<uint32_t>>(buttons + 4) = wstr(f, u"Cancel");
  *f.mem.translate<be<uint32_t>>(result) = 7;

  SECTION("with an overlapped") {
    auto r = in_child([&] {
      const uint64_t rc = f.call("XamShowMessageBoxUI", 0u, title, text, 2u, buttons, 1u, 0u, result, ov);
      ::_exit(rc == ERROR_IO_PENDING && rd(f, result) == 0 && rd(f, ov) == ERROR_SUCCESS ? 0 : 1);
    });
    CHECK(r.code == 0);
    CHECK(r.err == "[game] message box: Title \xE2\x80\x94 Caf\xC3\xA9 failed [OK, Cancel]\n");
  }
  SECTION("without an overlapped") {
    auto r = in_child([&] {
      const uint64_t rc = f.call("XamShowMessageBoxUI", 0u, title, text, 2u, buttons, 1u, 0u, result, 0u);
      ::_exit(rc == ERROR_SUCCESS && rd(f, result) == 0 ? 0 : 1);
    });
    CHECK(r.code == 0);
    CHECK(r.err.find("[OK, Cancel]") != std::string::npos);
  }
}

TEST_CASE("XamLoaderLaunchTitle is guest fatal", "[unit][kernel][xam]") {
  KernelFixture f;
  auto r = in_child([&] { f.call("XamLoaderLaunchTitle", 0u, 0u); });
  CHECK(r.code == 5);
  CHECK(r.err.find("Guest fatal: the game asked to relaunch itself (XamLoaderLaunchTitle)") != std::string::npos);
}

TEST_CASE("XamNuiGetDeviceStatus reports no Kinect sensor", "[unit][kernel][xam]") {
  KernelFixture f;
  const uint32_t status = f.alloc(24);
  std::memset(f.mem.translate<uint8_t>(status), 0xAB, 24);
  CHECK(f.call("XamNuiGetDeviceStatus", status) == 0);
  for (uint32_t i = 0; i < 24; i += 4) CHECK(rd(f, status + i) == 0);  // status (offset 12) 0: not connected
}
