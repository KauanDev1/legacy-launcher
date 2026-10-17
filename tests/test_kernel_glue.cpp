// SPDX-License-Identifier: MIT
// Kernel state (init_kernel / kernel()), guest_fatal and the guest-call glue (call_guest) of lr_runtime_core.
// The end-to-end path through a real main.cpp and recompiled callbacks is covered by test_fake_game.cpp.
#include <catch2/catch_test_macros.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

#include "runtime/guest_thread.h"
#include "runtime/kernel/glue.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/registry.h"
#include "support/kernel_fixture.h"

using lr::testing::KernelFixture;

namespace {

struct ChildResult {
  int code;
  std::string err;
};

// Runs `body` in a forked child with stderr captured; the child must end the process itself.
template <class F>
ChildResult run_child(F body) {
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

// A recording glue standing in for main.cpp's.
struct Recorded {
  uint8_t* base = nullptr;
  uint32_t fn = 0, r1 = 0, r13 = 0;
  std::vector<uint64_t> args;
  int calls = 0;
};
Recorded g_rec;
uint32_t g_callee_sets_frame = 0;  // when non-zero, the fake callee records this frame like a thunk in it would

uint64_t record_call(uint8_t* base, uint32_t fn, const uint64_t* args, int nargs, uint32_t r1, uint32_t r13) {
  g_rec.base = base;
  g_rec.fn = fn;
  g_rec.r1 = r1;
  g_rec.r13 = r13;
  g_rec.args.assign(args, args + nargs);
  ++g_rec.calls;
  if (g_callee_sets_frame != 0) lr::kernel::set_current_frame(g_callee_sets_frame, r13);
  return 0x1234;
}
bool only_82000200(uint32_t fn) { return fn == 0x82000200; }

}  // namespace

namespace lr::kernel::exports {
// Sums ten arguments: positions 8 and 9 come from the guest stack, so it checks KernelFixture::put_stack_arg.
uint32_t TestGlueSum10(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g, uint32_t h,
                       uint32_t i, uint32_t j) {
  return a + b + c + d + e + f + g + h + i + j;
}
}  // namespace lr::kernel::exports
LR_KERNEL(TestGlueSum10);

TEST_CASE("guest_fatal prints the trail", "[unit]") {
  auto r = run_child([] {
    lr::kernel::trace("KeFirst");
    lr::kernel::trace("KeSecond");
    lr::kernel::guest_fatal("guest raised an exception");
  });
  CHECK(r.code == lr::runtime::kExitGuestFatal);
  CHECK(r.code == 5);
  CHECK(r.err == "Guest fatal: guest raised an exception\nTrail (last 2): KeFirst -> KeSecond\n");
}

TEST_CASE("exit codes for guest fatal and deadlock", "[unit]") {
  STATIC_REQUIRE(lr::runtime::kExitGuestFatal == 5);
  STATIC_REQUIRE(lr::runtime::kExitDeadlock == 6);
}

TEST_CASE("init_kernel is once per process and kernel() exposes memory and config", "[unit]") {
  KernelFixture f;
  CHECK(&lr::kernel::kernel().memory() == &f.mem);
  CHECK(lr::kernel::kernel().config().strict_memory == false);
  auto again = lr::kernel::init_kernel(f.mem, {});
  REQUIRE_FALSE(again);
  CHECK(again.error().code == lr::ErrorCode::Internal);
}

TEST_CASE("init_kernel keeps the configuration", "[unit]") {
  auto m = lr::runtime::GuestMemory::reserve(0x10000);
  REQUIRE(m);
  lr::kernel::reset_kernel_for_tests();
  lr::kernel::KernelConfig cfg;
  cfg.game_dir = "/games/mc";
  cfg.user_dir = "/home/u/.local/share/lr";
  cfg.language = "pt";
  cfg.strict_memory = true;
  cfg.trace_file = "/tmp/trace.log";
  REQUIRE(lr::kernel::init_kernel(m.value(), cfg));
  const auto& k = lr::kernel::kernel().config();
  CHECK(k.game_dir == "/games/mc");
  CHECK(k.user_dir == "/home/u/.local/share/lr");
  CHECK(k.language == "pt");
  CHECK(k.strict_memory);
  CHECK(k.trace_file == std::filesystem::path("/tmp/trace.log"));
  lr::kernel::reset_kernel_for_tests();
}

TEST_CASE("KernelFixture calls an import with register and stack arguments", "[unit]") {
  KernelFixture f;
  CHECK(f.call("TestGlueSum10", 1, 2, 3, 4, 5, 6, 7, 8, 100, 1000) == 1136);
  const uint32_t a = f.alloc(3);
  const uint32_t b = f.alloc(16);
  CHECK(a == 0x20000000);
  CHECK(b == 0x20000010);
  CHECK(f.alloc(1) == 0x20000020);
}

TEST_CASE("call_guest runs the glue with the args and a fresh frame below the caller's", "[unit]") {
  KernelFixture f;
  lr::kernel::set_guest_glue({record_call, only_82000200});
  g_rec = {};
  g_callee_sets_frame = 0;
  lr::kernel::set_current_frame(0x30000008, 0x7FFF0000);
  const uint64_t args[] = {41, 0xFFFFFFFF'00000001ull, 3};
  CHECK(lr::kernel::call_guest(0x82000200, args) == 0x1234);
  CHECK(g_rec.calls == 1);
  CHECK(g_rec.base == f.mem.base());
  CHECK(g_rec.fn == 0x82000200);
  CHECK(g_rec.r1 == 0x2FFFFF00);  // (0x30000008 - 0x100) & ~0xF
  CHECK(g_rec.r13 == 0x7FFF0000);
  CHECK(g_rec.args == std::vector<uint64_t>{41, 0xFFFFFFFF'00000001ull, 3});

  // A thunk inside the callee moves the current frame; call_guest restores the caller's on return.
  g_callee_sets_frame = 0x2FFFF000;
  CHECK(lr::kernel::call_guest(0x82000200, {}) == 0x1234);
  CHECK(g_rec.args.empty());
  g_callee_sets_frame = 0;
  CHECK(lr::kernel::call_guest(0x82000200, {}) == 0x1234);
  CHECK(g_rec.r1 == 0x2FFFFF00);
}

struct CalleeThrew {};
static uint64_t throwing_call(uint8_t*, uint32_t, const uint64_t*, int, uint32_t r1, uint32_t r13) {
  lr::kernel::set_current_frame(r1 - 0x40, r13);  // a thunk inside the callee moved the frame, then it unwound
  throw CalleeThrew{};
}

TEST_CASE("call_guest restores the caller's frame when the callee unwinds", "[unit]") {
  KernelFixture f;
  lr::kernel::set_guest_glue({throwing_call, only_82000200});
  lr::kernel::set_current_frame(0x30000008, 0x7FFF0000);
  CHECK_THROWS_AS(lr::kernel::call_guest(0x82000200, {}), CalleeThrew);

  lr::kernel::set_guest_glue({record_call, only_82000200});
  g_rec = {};
  g_callee_sets_frame = 0;
  lr::kernel::call_guest(0x82000200, {});
  CHECK(g_rec.r1 == 0x2FFFFF00);  // computed from the restored caller frame 0x30000008
  CHECK(g_rec.r13 == 0x7FFF0000);
}

TEST_CASE("call_guest to an address without function is guest fatal (unit)", "[unit]") {
  auto r = run_child([] {
    KernelFixture f;
    lr::kernel::set_guest_glue({record_call, only_82000200});
    lr::kernel::set_current_frame(0x30000000, 0x7FFF0000);
    lr::kernel::call_guest(0x82000240, {});
  });
  CHECK(r.code == 5);
  CHECK(r.err.find("Guest fatal: ") == 0);
  CHECK(r.err.find("0x82000240") != std::string::npos);
}
