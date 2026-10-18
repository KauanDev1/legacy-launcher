// SPDX-License-Identifier: MIT
// Diagnostics (Task 20): the import trace (runtime/kernel/trace.h), the deadlock rule (runtime/kernel/deadlock.h),
// the dispatcher's per-waiter APC flag and the debug / bugcheck / HAL / SEH-minimum imports (exports/debug.cpp).
// The end-to-end detector run (exit 6) and --trace-imports through main.cpp are in test_fake_game.cpp.
#include <catch2/catch_test_macros.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "runtime/be.h"
#include "runtime/kernel/deadlock.h"
#include "runtime/kernel/dispatcher.h"
#include "runtime/kernel/guest_structs.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/trace.h"
#include "support/kernel_fixture.h"
#include "support/temp_dir.h"

using namespace lr::kernel;
using lr::runtime::be;
using lr::testing::KernelFixture;
using namespace std::chrono_literals;

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

std::string read_text(const std::filesystem::path& p) {
  std::ifstream f(p);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Calls an import with a full GuestCall (r1, r13, lr and r3..r10), as a thunk would.
GuestCall raw_call(KernelFixture& f, const char* name, std::vector<uint64_t> gpr, uint32_t r13 = 0, uint32_t lr = 0) {
  const ImportEntry e = find_import(name);
  REQUIRE(e.fn != nullptr);
  GuestCall c;
  c.base = f.mem.base();
  c.r1 = KernelFixture::kStackPointer;
  c.r13 = r13;
  c.lr = lr;
  for (size_t i = 0; i < gpr.size() && i < 8; ++i) c.gpr[i] = gpr[i];
  trace(name);
  e.fn(c);
  return c;
}

WaitInfo waiting(uint32_t token, bool infinite = true, bool alertable = false, bool apc = false) {
  return WaitInfo{token, {0x40000000}, WaitType::Any, infinite, alertable, apc};
}

}  // namespace

// --- Deadlock rule --------------------------------------------------------------------------------------------------

TEST_CASE("deadlock rule: every live guest thread in an infinite wait with nothing pending", "[unit][kernel][debug]") {
  const std::vector<uint32_t> live = {0x100, 0x200};
  SECTION("both blocked forever") {
    CHECK(all_threads_blocked(live, {waiting(0x100), waiting(0x200)}, false));
  }
  SECTION("an alertable wait without a pending APC still counts") {
    CHECK(all_threads_blocked(live, {waiting(0x100, true, true, false), waiting(0x200)}, false));
  }
  SECTION("a thread that is not in a dispatcher wait (running guest code) means no deadlock") {
    CHECK_FALSE(all_threads_blocked(live, {waiting(0x100)}, false));
  }
  SECTION("a finite wait means no deadlock") {
    CHECK_FALSE(all_threads_blocked(live, {waiting(0x100), waiting(0x200, false)}, false));
  }
  SECTION("an armed timer or queued DPC means no deadlock") {
    CHECK_FALSE(all_threads_blocked(live, {waiting(0x100), waiting(0x200)}, true));
  }
  SECTION("an alertable wait with an APC pending means no deadlock") {
    CHECK_FALSE(all_threads_blocked(live, {waiting(0x100, true, true, true), waiting(0x200)}, false));
  }
  SECTION("waiters that are not live guest threads (the host main thread) are ignored") {
    CHECK(all_threads_blocked(live, {waiting(0x100), waiting(0x200), waiting(0x999, false)}, false));
  }
  SECTION("no live guest thread is never a deadlock") {
    CHECK_FALSE(all_threads_blocked({}, {waiting(0x100)}, false));
  }
}

TEST_CASE("dispatcher snapshot reports a pending APC per waiter", "[unit][kernel][debug]") {
  KernelFixture f;
  const uint32_t ev = f.alloc(16);  // zeroed: notification event (type 0), not signaled
  Dispatcher& d = kernel().dispatcher();
  std::thread waiter([&] {
    const uint32_t h[1] = {ev};
    (void)d.wait(h, WaitType::Any, Timeout{Timeout::Infinite, 0}, false);
  });
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (d.snapshot().empty() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  auto snap = d.snapshot();
  REQUIRE(snap.size() == 1);
  CHECK_FALSE(snap[0].apc_pending);
  const uint32_t token = snap[0].thread_token;
  d.wake_for_apc(token);  // non-alertable wait: stays blocked, the wake-up stays pending
  snap = d.snapshot();
  REQUIRE(snap.size() == 1);
  CHECK(snap[0].apc_pending);
  CHECK(d.apc_pending(token));
  d.thread_exited(token);
  CHECK_FALSE(d.apc_pending(token));
  (void)d.set_event(ev);
  waiter.join();
}

// --- Import trace ---------------------------------------------------------------------------------------------------

TEST_CASE("import trace writes one line per call with r3..r6 and the return", "[unit][kernel][debug][trace]") {
  lr::testing::TempDir td("lr_trace");
  const auto file = td.path / "logs" / "trace.log";  // the parent directory is created
  REQUIRE(enable_import_trace(file));
  CHECK(import_trace_enabled());
  GuestCall c;
  c.gpr[0] = 0x1'2345'6789;  // low 32 bits only
  c.gpr[1] = 2;
  c.gpr[2] = 3;
  c.gpr[3] = 0xFFFFFFFF;
  c.ret_gpr = 0xFFFFFFFF'C0000001;
  trace_import_return("NtFoo", c);
  trace_import_missing("NtBar", c);
  disable_import_trace();
  CHECK_FALSE(import_trace_enabled());
  trace_import_return("NotLogged", c);
  CHECK(read_text(file) ==
        "[0] NtFoo r3=0x23456789 r4=0x00000002 r5=0x00000003 r6=0xFFFFFFFF -> 0xC0000001\n"
        "[0] NtBar r3=0x23456789 r4=0x00000002 r5=0x00000003 r6=0xFFFFFFFF -> missing\n");
}

TEST_CASE("import trace is flushed on the guest fatal path", "[unit][kernel][debug][trace]") {
  lr::testing::TempDir td("lr_trace");
  const auto file = td.path / "trace.log";
  KernelFixture f;
  auto r = in_child([&] {
    if (!enable_import_trace(file)) ::_exit(98);
    GuestCall c;
    trace_import_return("BeforeFatal", c);
    guest_fatal("test");
  });
  CHECK(r.code == 5);
  CHECK(read_text(file).find("[0] BeforeFatal r3=0x00000000") != std::string::npos);
}

// --- Debug, bugcheck, HAL, SEH ---------------------------------------------------------------------------------------

TEST_CASE("KeBugCheckEx exits 5 with code and parameters", "[unit][kernel][debug]") {
  KernelFixture f;
  auto r = in_child([&] { f.call("KeBugCheckEx", 0xDEADu, 1u, 2u, 3u, 0xFFFFFFFFu); });
  CHECK(r.code == 5);
  CHECK(r.err.find("Guest fatal: KeBugCheckEx 0x0000DEAD (0x00000001, 0x00000002, 0x00000003, 0xFFFFFFFF)") !=
        std::string::npos);
  CHECK(r.err.find("Trail (last 1): KeBugCheckEx") != std::string::npos);

  auto b = in_child([&] { f.call("KeBugCheck", 0x1234u); });
  CHECK(b.code == 5);
  CHECK(b.err.find("Guest fatal: KeBugCheck 0x00001234") != std::string::npos);
}

TEST_CASE("RtlRaiseException is guest fatal naming the code, except the thread-name exception", "[unit][kernel][debug]") {
  KernelFixture f;
  const uint32_t rec = f.alloc(sizeof(X_EXCEPTION_RECORD));
  auto* record = f.mem.translate<X_EXCEPTION_RECORD>(rec);
  record->code = 0xE06D7363;
  auto r = in_child([&] { f.call("RtlRaiseException", rec); });
  CHECK(r.code == 5);
  CHECK(r.err.find("Guest fatal: RtlRaiseException 0xE06D7363") != std::string::npos);

  // MSVC's SetThreadName (0x406D1388) is raised on purpose and caught by the debugger: it returns, as in Xenia.
  record->code = 0x406D1388;
  f.call("RtlRaiseException", rec);
  SUCCEED("returned");
}

TEST_CASE("RtlUnwind and __C_specific_handler are guest fatal by name", "[unit][kernel][debug]") {
  KernelFixture f;
  auto u = in_child([&] { f.call("RtlUnwind", 0u, 0u, 0u, 0u); });
  CHECK(u.code == 5);
  CHECK(u.err.find("Guest fatal: RtlUnwind") != std::string::npos);
  auto h = in_child([&] { f.call("__C_specific_handler", 0u, 0u, 0u, 0u); });
  CHECK(h.code == 5);
  CHECK(h.err.find("Guest fatal: __C_specific_handler") != std::string::npos);
}

TEST_CASE("RtlCaptureContext fills the guest CONTEXT from the call registers", "[unit][kernel][debug]") {
  KernelFixture f;
  const uint32_t ctx = f.alloc(sizeof(X_CONTEXT));
  std::memset(f.mem.translate<uint8_t>(ctx), 0xAB, sizeof(X_CONTEXT));
  (void)raw_call(f, "RtlCaptureContext", {ctx, 4, 5, 6, 7, 8, 9, 10}, 0x7000, 0x82001234);
  const auto& x = *f.mem.translate<X_CONTEXT>(ctx);
  CHECK(x.context_flags == 0x5u);  // CONTEXT_CONTROL | CONTEXT_INTEGER
  CHECK(x.lr == 0x82001234u);
  CHECK(x.iar == 0x82001234u);
  CHECK(x.gpr[1] == KernelFixture::kStackPointer);
  CHECK(x.gpr[3] == ctx);
  CHECK(x.gpr[4] == 4u);
  CHECK(x.gpr[10] == 10u);
  CHECK(x.gpr[13] == 0x7000u);
  // Only the known fields are written; the rest of the (unverified) layout keeps whatever the guest had there.
  constexpr uint64_t kFill = 0xABABABABABABABABull;
  CHECK(x.msr == 0xABABABABu);
  CHECK(x.ctr == kFill);
  CHECK(x.gpr[0] == kFill);
  CHECK(x.gpr[2] == kFill);
  CHECK(x.gpr[11] == kFill);
  CHECK(x.gpr[12] == kFill);
  CHECK(x.gpr[14] == kFill);
  CHECK(x.gpr[31] == kFill);
  CHECK(x.cr == 0xABABABABu);
  CHECK(x.vr[127][3] == 0xABABABABu);
  const uint8_t* bytes = f.mem.translate<uint8_t>(ctx);
  for (size_t i = offsetof(X_CONTEXT, gpr) + 14 * 8; i < sizeof(X_CONTEXT); ++i) {
    if (bytes[i] != 0xAB) {
      FAIL("byte " << i << " of the CONTEXT was written");
    }
  }
}

TEST_CASE("DbgPrint prints the format string prefixed [guest]", "[unit][kernel][debug]") {
  KernelFixture f;
  const uint32_t fmt = f.alloc(32);
  std::strcpy(f.mem.translate<char>(fmt), "hello %d\n");
  auto r = in_child([&] {
    const uint64_t s = f.call("DbgPrint", fmt, 5u);
    std::fflush(nullptr);
    ::_exit(s == 0 ? 0 : 1);
  });
  CHECK(r.code == 0);
  CHECK(r.err == "[guest] hello %d\n");
}

TEST_CASE("debug stubs return success and DbgBreakPoint continues", "[unit][kernel][debug]") {
  KernelFixture f;
  for (const char* n : {"EtxProducerRegister", "EtxProducerUnregister", "EtxProducerLog", "KeLockL2", "KeUnlockL2"}) {
    INFO(n);
    CHECK(find_import(n).state == lr::kernel::ImportState::Stub);
    CHECK(f.call(n, 0u, 0u) == 0);
  }
  auto r = in_child([&] {
    f.call("DbgBreakPoint");
    ::_exit(0);
  });
  CHECK(r.code == 0);
  CHECK(r.err.find("DbgBreakPoint") != std::string::npos);
}

TEST_CASE("HalReturnToFirmware ends the process with exit 0", "[unit][kernel][debug]") {
  KernelFixture f;
  auto r = in_child([&] { f.call("HalReturnToFirmware", 1u); });
  CHECK(r.code == 0);
}
