// SPDX-License-Identifier: MIT
// Rtl critical sections (exports/critsec.cpp) over X_RTL_CRITICAL_SECTION in guest memory.
#include <catch2/catch_test_macros.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "runtime/be.h"
#include "runtime/kernel/guest_structs.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/registry.h"
#include "support/kernel_fixture.h"

using namespace lr::kernel;
using lr::runtime::be;
using lr::testing::KernelFixture;

namespace {

// Thread-safe import call for worker threads (no Catch2 macros).
uint32_t tcall(KernelFixture& f, const char* name, uint32_t a0, uint32_t a1 = 0) {
  const ImportEntry e = find_import(name);
  if (e.fn == nullptr) return 0xDEADBEEF;
  GuestCall c;
  c.base = f.mem.base();
  c.r1 = KernelFixture::kStackPointer;
  c.gpr[0] = a0;
  c.gpr[1] = a1;
  e.fn(c);
  return static_cast<uint32_t>(c.ret_gpr);
}

X_RTL_CRITICAL_SECTION& cs_at(KernelFixture& f, uint32_t a) { return *f.mem.translate<X_RTL_CRITICAL_SECTION>(a); }

uint32_t new_cs(KernelFixture& f, uint32_t spin = 0) {
  const uint32_t a = f.alloc(sizeof(X_RTL_CRITICAL_SECTION));
  if (spin == 0) f.call("RtlInitializeCriticalSection", a);
  else f.call("RtlInitializeCriticalSectionAndSpinCount", a, spin);
  return a;
}

}  // namespace

TEST_CASE("critical section initialize layout", "[unit][kernel][critsec]") {
  KernelFixture f;
  const uint32_t a = new_cs(f, 4000);
  auto& cs = cs_at(f, a);
  CHECK(cs.header.type == 1);
  CHECK(cs.header.absolute == 16);  // ceil(4000 / 256)
  CHECK(cs.header.signal_state == 0);
  CHECK(static_cast<int32_t>(cs.lock_count) == -1);
  CHECK(static_cast<int32_t>(cs.recursion_count) == 0);
  CHECK(static_cast<uint32_t>(cs.owning_thread) == 0);
}

TEST_CASE("recursive enter by owner", "[unit][kernel][critsec]") {
  KernelFixture f;
  const uint32_t a = new_cs(f);
  auto& cs = cs_at(f, a);
  f.call("RtlEnterCriticalSection", a);
  CHECK(static_cast<int32_t>(cs.lock_count) == 0);
  CHECK(static_cast<int32_t>(cs.recursion_count) == 1);
  CHECK(static_cast<uint32_t>(cs.owning_thread) != 0);
  f.call("RtlEnterCriticalSection", a);
  CHECK(static_cast<int32_t>(cs.lock_count) == 1);
  CHECK(static_cast<int32_t>(cs.recursion_count) == 2);
  CHECK(f.call("RtlTryEnterCriticalSection", a) == 1);
  CHECK(static_cast<int32_t>(cs.recursion_count) == 3);
  for (int i = 0; i < 3; ++i) f.call("RtlLeaveCriticalSection", a);
  CHECK(static_cast<int32_t>(cs.lock_count) == -1);
  CHECK(static_cast<int32_t>(cs.recursion_count) == 0);
  CHECK(static_cast<uint32_t>(cs.owning_thread) == 0);
}

TEST_CASE("try enter fails when owned by another thread", "[unit][kernel][critsec]") {
  KernelFixture f;
  const uint32_t a = new_cs(f);
  f.call("RtlEnterCriticalSection", a);
  std::atomic<uint32_t> r{99};
  std::thread t([&] { r = tcall(f, "RtlTryEnterCriticalSection", a); });
  t.join();
  CHECK(r == 0);
  f.call("RtlLeaveCriticalSection", a);
  std::thread t2([&] {
    r = tcall(f, "RtlTryEnterCriticalSection", a);
    tcall(f, "RtlLeaveCriticalSection", a);
  });
  t2.join();
  CHECK(r == 1);
}

TEST_CASE("four threads incrementing a counter 10000 times each under the section reach 40000",
          "[unit][kernel][critsec]") {
  for (uint32_t spin : {0u, 2000u}) {
    KernelFixture f;
    const uint32_t a = new_cs(f, spin);
    const uint32_t counter = f.alloc(4);
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i)
      ts.emplace_back([&] {
        for (int n = 0; n < 10000; ++n) {
          tcall(f, "RtlEnterCriticalSection", a);
          auto* p = f.mem.translate<be<uint32_t>>(counter);
          *p = static_cast<uint32_t>(*p) + 1;  // plain read-modify-write, safe only under the lock
          tcall(f, "RtlLeaveCriticalSection", a);
        }
      });
    for (auto& t : ts) t.join();
    CHECK(static_cast<uint32_t>(*f.mem.translate<be<uint32_t>>(counter)) == 40000);
    CHECK(static_cast<int32_t>(cs_at(f, a).lock_count) == -1);
  }
}

TEST_CASE("leave by non-owner is guest fatal", "[unit][kernel][critsec]") {
  KernelFixture f;
  const uint32_t a = new_cs(f);
  int fds[2];
  REQUIRE(::pipe(fds) == 0);
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::dup2(fds[1], 2);
    ::close(fds[0]);
    tcall(f, "RtlLeaveCriticalSection", a);
    ::_exit(99);
  }
  ::close(fds[1]);
  std::string out;
  char buf[256];
  for (ssize_t n; (n = ::read(fds[0], buf, sizeof buf)) > 0;) out.append(buf, static_cast<size_t>(n));
  ::close(fds[0]);
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  CHECK(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 5);
  CHECK(out.find("non-owner") != std::string::npos);
}
