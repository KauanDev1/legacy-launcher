// SPDX-License-Identifier: MIT
// Guest threads (runtime/kernel/threads.h) and the thread imports (exports/threads.cpp). "Guest" functions here are
// host lambdas behind a test GuestGlue keyed by guest address; the end-to-end path through main.cpp and recompiled
// code is covered by test_fake_game.cpp.
#include <catch2/catch_test_macros.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "runtime/be.h"
#include "runtime/kernel/dispatcher.h"
#include "runtime/kernel/glue.h"
#include "runtime/kernel/guest_structs.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/objects.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/status.h"
#include "runtime/kernel/threads.h"
#include "runtime/loader.h"
#include "runtime/memory_map.h"
#include "support/kernel_fixture.h"

using namespace lr::kernel;
using lr::runtime::be;
using lr::testing::KernelFixture;
using namespace std::chrono_literals;

namespace {

// --- Test glue: guest address -> host function -------------------------------------------------------------------

using GuestFn = std::function<uint64_t(const std::vector<uint64_t>&)>;
std::mutex g_fns_mutex;
std::map<uint32_t, GuestFn> g_fns;
thread_local uint32_t t_r1 = 0, t_r13 = 0;  // the frame of the "guest" function running on this thread

uint64_t test_call(uint8_t*, uint32_t fn, const uint64_t* args, int nargs, uint32_t r1, uint32_t r13) {
  GuestFn f;
  {
    std::lock_guard<std::mutex> lock(g_fns_mutex);
    f = g_fns.at(fn);
  }
  const uint32_t saved_r1 = t_r1, saved_r13 = t_r13;
  t_r1 = r1;
  t_r13 = r13;
  const uint64_t r = f(std::vector<uint64_t>(args, args + nargs));
  t_r1 = saved_r1;
  t_r13 = saved_r13;
  return r;
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

// Import call from inside a "guest" function: like a thunk (trace + current frame), on the calling thread's frame.
template <class... A>
uint32_t gcall(KernelFixture& f, const char* name, A... args) {
  static_assert(sizeof...(A) <= 8);
  const ImportEntry e = find_import(name);
  if (e.fn == nullptr) return 0xDEADBEEF;
  GuestCall c;
  c.base = f.mem.base();
  c.r1 = t_r1 != 0 ? t_r1 : KernelFixture::kStackPointer;
  c.r13 = t_r13;
  int i = 0;
  ((c.gpr[i++] = static_cast<uint64_t>(args)), ...);
  trace(name);
  set_current_frame(c.r1, c.r13);
  e.fn(c);
  return static_cast<uint32_t>(c.ret_gpr);
}

uint32_t rd(KernelFixture& f, uint32_t addr) { return *f.mem.translate<be<uint32_t>>(addr); }
void wr(KernelFixture& f, uint32_t addr, uint32_t v) { *f.mem.translate<be<uint32_t>>(addr) = v; }

constexpr uint32_t kFnA = 0x82000400, kFnB = 0x82000410, kFnC = 0x82000420;

// ExCreateThread(handle*, stack_size, id*, xapi_startup, start, context, flags); returns {status, handle, id}.
struct Created {
  uint32_t status, handle, id;
};
Created create(KernelFixture& f, uint32_t start, uint32_t context, uint32_t flags = 0, uint32_t xapi = 0) {
  const uint32_t out = f.alloc(8);
  const uint32_t s = static_cast<uint32_t>(f.call("ExCreateThread", out, 0u, out + 4, xapi, start, context, flags));
  return {s, rd(f, out), rd(f, out + 4)};
}

uint32_t wait_handle(KernelFixture& f, uint32_t handle, int64_t relative_100ns = 0) {
  uint32_t timeout = 0;
  if (relative_100ns != 0) {
    timeout = f.alloc(8);
    *f.mem.translate<be<int64_t>>(timeout) = -relative_100ns;
  }
  return static_cast<uint32_t>(f.call("NtWaitForSingleObjectEx", handle, 0u, 0u, timeout));
}

bool wait_for_waiters(size_t n) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (kernel().dispatcher().snapshot().size() != n) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::yield();
  }
  return true;
}

std::shared_ptr<GuestThread> thread_of(uint32_t handle) {
  return std::static_pointer_cast<GuestThread>(kernel().objects().get(handle));
}

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

}  // namespace

TEST_CASE("ExCreateThread runs start(context) and returns handle and id", "[unit][kernel][threads]") {
  KernelFixture f;
  struct Seen {
    uint64_t context = 0;
    uint32_t id = 0, r1 = 0, r13 = 0, handle = 0;
    bool guest = false;
  } seen;
  def(kFnA, [&](const std::vector<uint64_t>& a) -> uint64_t {
    GuestThread* t = current_thread();
    seen.guest = t != nullptr;
    seen.context = a.at(0);
    seen.id = t ? t->id() : 0;
    seen.handle = t ? t->handle() : 0;
    seen.r1 = t_r1;
    seen.r13 = t_r13;
    return 7;
  });
  const Created c = create(f, kFnA, 0x1234);
  REQUIRE(c.status == STATUS_SUCCESS);
  CHECK(c.handle >= kHandleBase);
  CHECK(c.id >= 1);
  CHECK(wait_handle(f, c.handle) == STATUS_WAIT_0);
  CHECK(current_thread() == nullptr);  // the test thread is not a guest thread
  CHECK(seen.guest);
  CHECK(seen.context == 0x1234);
  CHECK(seen.id == c.id);
  CHECK(seen.handle == c.handle);

  // r13 is the thread's PCR; the PCR points at itself, the KTHREAD and the stack the thread runs on.
  const uint32_t pcr = seen.r13;
  auto t = thread_of(c.handle);
  REQUIRE(t);
  CHECK(pcr == t->pcr());
  CHECK(rd(f, pcr + offsetof(X_KPCR, pcr_ptr)) == pcr);
  CHECK(rd(f, pcr + offsetof(X_KPCR, current_thread)) == t->kthread());
  CHECK(t->guest_header() == t->kthread());
  const uint32_t top = rd(f, pcr + offsetof(X_KPCR, stack_base));
  const uint32_t bottom = rd(f, pcr + offsetof(X_KPCR, stack_end));
  CHECK(bottom >= lr::runtime::kStacksBase);
  CHECK(top <= lr::runtime::kStacksEnd);
  CHECK(top - bottom == lr::runtime::kDefaultStackSize);
  CHECK(seen.r1 < top);
  CHECK(seen.r1 > bottom);
  CHECK(seen.r1 % 16 == 0);
  const uint32_t kt = t->kthread();
  CHECK(*f.mem.translate<uint8_t>(kt) == kThreadObject);
  CHECK(rd(f, kt + offsetof(X_KTHREAD, stack_base)) == top);
  CHECK(rd(f, kt + offsetof(X_KTHREAD, stack_limit)) == bottom);
  CHECK(rd(f, kt + offsetof(X_KTHREAD, thread_id)) == c.id);
  CHECK(rd(f, kt + offsetof(X_KTHREAD, start_address)) == kFnA);
  // Guard page below the stack: reserved, not committed (and PROT_NONE on the host); the stack itself committed.
  CHECK(f.call("MmQueryAddressProtect", bottom - 0x1000) == 0);
  CHECK(f.call("MmQueryAddressProtect", bottom) == 0x04);

  // A second thread gets another id, PCR and stack.
  const Created d = create(f, kFnA, 1);
  REQUIRE(d.status == STATUS_SUCCESS);
  CHECK(wait_handle(f, d.handle) == STATUS_WAIT_0);
  CHECK(d.id != c.id);
  CHECK(seen.r13 != pcr);
  CHECK(f.call("NtClose", c.handle) == STATUS_SUCCESS);
  CHECK(f.call("NtClose", d.handle) == STATUS_SUCCESS);
}

TEST_CASE("suspended thread waits for resume", "[unit][kernel][threads]") {
  KernelFixture f;
  std::atomic<bool> ran{false};
  def(kFnA, [&](const std::vector<uint64_t>&) -> uint64_t {
    ran = true;
    return 0;
  });
  const Created c = create(f, kFnA, 0, /*flags: suspended*/ 1);
  REQUIRE(c.status == STATUS_SUCCESS);
  CHECK(wait_handle(f, c.handle, 200'000) == STATUS_TIMEOUT);  // 20 ms
  CHECK_FALSE(ran);
  const uint32_t prev = f.alloc(4);
  CHECK(f.call("NtResumeThread", c.handle, prev) == STATUS_SUCCESS);
  CHECK(rd(f, prev) == 1);
  CHECK(wait_handle(f, c.handle) == STATUS_WAIT_0);
  CHECK(ran);
  CHECK(f.call("NtClose", c.handle) == STATUS_SUCCESS);
}

TEST_CASE("NtSuspendThread stops a running thread at its next import", "[unit][kernel][threads]") {
  KernelFixture f;
  std::atomic<uint32_t> count{0};
  std::atomic<bool> stop{false};
  def(kFnA, [&](const std::vector<uint64_t>&) -> uint64_t {
    while (!stop) {
      (void)gcall(f, "NtYieldExecution");
      ++count;
    }
    return 0;
  });
  const Created c = create(f, kFnA, 0);
  REQUIRE(c.status == STATUS_SUCCESS);
  while (count < 10) std::this_thread::yield();
  const uint32_t prev = f.alloc(4);
  wr(f, prev, 0xFFFF);
  CHECK(f.call("NtSuspendThread", c.handle, prev) == STATUS_SUCCESS);
  CHECK(rd(f, prev) == 0);
  std::this_thread::sleep_for(10ms);  // lets the current iteration finish
  const uint32_t frozen = count;
  std::this_thread::sleep_for(30ms);
  CHECK(count == frozen);
  CHECK(f.call("NtResumeThread", c.handle, prev) == STATUS_SUCCESS);
  CHECK(rd(f, prev) == 1);
  while (count < frozen + 10) std::this_thread::yield();
  stop = true;
  CHECK(wait_handle(f, c.handle) == STATUS_WAIT_0);
  CHECK(f.call("NtClose", c.handle) == STATUS_SUCCESS);
}

TEST_CASE("xapi start routine wraps start", "[unit][kernel][threads]") {
  KernelFixture f;
  std::vector<uint64_t> xapi_args;
  uint64_t start_arg = 0;
  def(kFnA, [&](const std::vector<uint64_t>& a) -> uint64_t {
    start_arg = a.at(0);
    return 0x99;
  });
  def(kFnB, [&](const std::vector<uint64_t>& a) -> uint64_t {
    xapi_args = a;
    const uint64_t args[] = {a.at(1)};
    return call_guest(static_cast<uint32_t>(a.at(0)), args);
  });
  const Created c = create(f, kFnA, 0x55, 0, /*xapi*/ kFnB);
  REQUIRE(c.status == STATUS_SUCCESS);
  CHECK(wait_handle(f, c.handle) == STATUS_WAIT_0);
  CHECK(xapi_args == std::vector<uint64_t>{kFnA, 0x55});
  CHECK(start_arg == 0x55);
  CHECK(thread_of(c.handle)->exit_code() == 0);  // the trampoline's return is not the exit code (as Xenia)
  CHECK(f.call("NtClose", c.handle) == STATUS_SUCCESS);
}

TEST_CASE("thread exit code via wait + KTHREAD signal", "[unit][kernel][threads]") {
  KernelFixture f;
  def(kFnA, [&](const std::vector<uint64_t>&) -> uint64_t { return 0x1234ABCD; });
  const Created c = create(f, kFnA, 0);
  REQUIRE(c.status == STATUS_SUCCESS);
  auto t = thread_of(c.handle);
  REQUIRE(t);
  // KeWaitForSingleObject on the KTHREAD (what ObReferenceObjectByHandle hands out) works like the handle.
  CHECK(f.call("KeWaitForSingleObject", t->kthread(), 0u, 0u, 0u, 0u) == STATUS_WAIT_0);
  CHECK(static_cast<int32_t>(*f.mem.translate<be<int32_t>>(t->kthread() + 4)) == 1);
  CHECK(t->exit_code() == 0x1234ABCD);
  CHECK(wait_handle(f, c.handle) == STATUS_WAIT_0);  // stays signaled
  const uint32_t obj = f.alloc(4);
  CHECK(f.call("ObReferenceObjectByHandle", c.handle, 0u, obj) == STATUS_SUCCESS);
  CHECK(rd(f, obj) == t->kthread());
  f.call("ObDereferenceObject", t->kthread());
  t.reset();
  CHECK(f.call("NtClose", c.handle) == STATUS_SUCCESS);
}

TEST_CASE("TLS template copied per thread", "[unit][kernel][threads]") {
  KernelFixture f;
  const uint32_t tmpl = f.alloc(0x10);
  for (uint32_t i = 0; i < 0x10; ++i) *f.mem.translate<uint8_t>(tmpl + i) = static_cast<uint8_t>(0xA0 + i);
  kernel().threads().set_image(lr::formats::TlsInfo{0, tmpl, 0x20, 0x10}, 0);

  struct Seen {
    uint32_t tls = 0, kt_tls = 0;
    std::vector<uint8_t> bytes;
  };
  Seen s1, s2;
  auto body = [&](Seen& s) {
    return [&f, &s](const std::vector<uint64_t>&) -> uint64_t {
      s.tls = rd(f, t_r13 + offsetof(X_KPCR, tls_ptr));
      s.kt_tls = rd(f, current_thread()->kthread() + offsetof(X_KTHREAD, tls_address));
      s.bytes.assign(f.mem.translate<uint8_t>(s.tls), f.mem.translate<uint8_t>(s.tls) + 0x20);
      *f.mem.translate<uint8_t>(s.tls) = 0xEE;  // must not leak into the next thread
      return 0;
    };
  };
  def(kFnA, body(s1));
  def(kFnB, body(s2));
  const Created a = create(f, kFnA, 0);
  REQUIRE(a.status == STATUS_SUCCESS);
  CHECK(wait_handle(f, a.handle) == STATUS_WAIT_0);
  const Created b = create(f, kFnB, 0);
  REQUIRE(b.status == STATUS_SUCCESS);
  CHECK(wait_handle(f, b.handle) == STATUS_WAIT_0);

  std::vector<uint8_t> want(0x20, 0);
  for (uint32_t i = 0; i < 0x10; ++i) want[i] = static_cast<uint8_t>(0xA0 + i);
  CHECK(s1.bytes == want);
  CHECK(s2.bytes == want);
  CHECK(s1.tls != s2.tls);
  CHECK(s1.tls == s1.kt_tls);
  CHECK(s2.tls == s2.kt_tls);
  CHECK(f.call("NtClose", a.handle) == STATUS_SUCCESS);
  CHECK(f.call("NtClose", b.handle) == STATUS_SUCCESS);
}

TEST_CASE("KeTls slots are per thread", "[unit][kernel][threads]") {
  KernelFixture f;
  const uint32_t slot = static_cast<uint32_t>(f.call("KeTlsAlloc"));
  REQUIRE(slot != 0xFFFFFFFFu);
  std::vector<uint32_t> seen;
  auto body = [&](uint32_t value) {
    return [&f, &seen, slot, value](const std::vector<uint64_t>&) -> uint64_t {
      seen.push_back(gcall(f, "KeTlsGetValue", slot));
      seen.push_back(gcall(f, "KeTlsSetValue", slot, value));
      seen.push_back(gcall(f, "KeTlsGetValue", slot));
      return 0;
    };
  };
  def(kFnA, body(111));
  def(kFnB, body(222));
  const Created a = create(f, kFnA, 0);
  CHECK(wait_handle(f, a.handle) == STATUS_WAIT_0);
  const Created b = create(f, kFnB, 0);
  CHECK(wait_handle(f, b.handle) == STATUS_WAIT_0);
  CHECK(seen == std::vector<uint32_t>{0, 1, 111, 0, 1, 222});

  CHECK(f.call("KeTlsFree", slot) == 1);
  CHECK(f.call("KeTlsFree", 0xFFFFFFFFu) == 0);
  // 64 slots: the 65th allocation fails with TLS_OUT_OF_INDEXES.
  for (int i = 0; i < 64; ++i) CHECK(f.call("KeTlsAlloc") != 0xFFFFFFFFu);
  CHECK(static_cast<uint32_t>(f.call("KeTlsAlloc")) == 0xFFFFFFFFu);
  CHECK(f.call("NtClose", a.handle) == STATUS_SUCCESS);
  CHECK(f.call("NtClose", b.handle) == STATUS_SUCCESS);
}

TEST_CASE("ExTerminateThread ends the thread with its code", "[unit][kernel][threads]") {
  KernelFixture f;
  std::atomic<bool> after{false};
  def(kFnA, [&](const std::vector<uint64_t>&) -> uint64_t {
    (void)gcall(f, "ExTerminateThread", 0x77u);
    after = true;
    return 1;
  });
  const Created c = create(f, kFnA, 0);
  REQUIRE(c.status == STATUS_SUCCESS);
  CHECK(wait_handle(f, c.handle) == STATUS_WAIT_0);
  CHECK_FALSE(after);
  CHECK(thread_of(c.handle)->exit_code() == 0x77);
  CHECK(f.call("NtClose", c.handle) == STATUS_SUCCESS);
}

TEST_CASE("alertable delay runs queued APC and returns STATUS_USER_APC", "[unit][kernel][threads]") {
  KernelFixture f;
  std::vector<uint64_t> apc_args;
  uint32_t apc_thread = 0;
  def(kFnB, [&](const std::vector<uint64_t>& a) -> uint64_t {
    apc_args = a;
    apc_thread = current_thread() ? current_thread()->id() : 0;
    return 0;
  });
  def(kFnA, [&](const std::vector<uint64_t>&) -> uint64_t {
    const uint32_t interval = f.alloc(8);
    *f.mem.translate<be<int64_t>>(interval) = -100'000'000;  // 10 s
    return gcall(f, "KeDelayExecutionThread", 1u, 1u, interval);
  });
  const Created c = create(f, kFnA, 0);
  REQUIRE(c.status == STATUS_SUCCESS);
  REQUIRE(wait_for_waiters(1));
  CHECK(f.call("NtQueueApcThread", c.handle, kFnB, 0x11u, 0x22u, 0x33u) == STATUS_SUCCESS);
  CHECK(wait_handle(f, c.handle) == STATUS_WAIT_0);
  CHECK(thread_of(c.handle)->exit_code() == STATUS_USER_APC);
  CHECK(apc_args == std::vector<uint64_t>{0x11, 0x22, 0x33});
  CHECK(apc_thread == c.id);

  // Non-alertable delays and polls just return STATUS_SUCCESS.
  def(kFnC, [&](const std::vector<uint64_t>&) -> uint64_t {
    const uint32_t interval = f.alloc(8);
    *f.mem.translate<be<int64_t>>(interval) = -10'000;  // 1 ms
    return gcall(f, "KeDelayExecutionThread", 1u, 0u, interval);
  });
  const Created d = create(f, kFnC, 0);
  CHECK(wait_handle(f, d.handle) == STATUS_WAIT_0);
  CHECK(thread_of(d.handle)->exit_code() == STATUS_SUCCESS);
  CHECK(f.call("NtClose", c.handle) == STATUS_SUCCESS);
  CHECK(f.call("NtClose", d.handle) == STATUS_SUCCESS);
}

TEST_CASE("close handle of running thread", "[unit][kernel][threads]") {
  KernelFixture f;
  const uint32_t ev_out = f.alloc(4);
  REQUIRE(f.call("NtCreateEvent", ev_out, 0u, 0u, 0u) == STATUS_SUCCESS);
  const uint32_t ev = rd(f, ev_out);
  def(kFnA, [&, ev](const std::vector<uint64_t>&) -> uint64_t {
    return gcall(f, "NtWaitForSingleObjectEx", ev, 0u, 0u, 0u);
  });
  const Created c = create(f, kFnA, 0);
  REQUIRE(c.status == STATUS_SUCCESS);
  REQUIRE(wait_for_waiters(1));
  const uint32_t dup = f.alloc(4);
  REQUIRE(f.call("NtDuplicateObject", c.handle, dup, 0u) == STATUS_SUCCESS);
  CHECK(f.call("NtClose", c.handle) == STATUS_SUCCESS);
  CHECK(f.call("NtSetEvent", ev, 0u) == STATUS_SUCCESS);
  CHECK(wait_handle(f, rd(f, dup)) == STATUS_WAIT_0);
  CHECK(f.call("NtClose", rd(f, dup)) == STATUS_SUCCESS);
  CHECK(f.call("NtClose", ev) == STATUS_SUCCESS);
}

TEST_CASE("thread lookups, priority, affinity and process type", "[unit][kernel][threads]") {
  KernelFixture f;
  const uint32_t ev_out = f.alloc(4);
  REQUIRE(f.call("NtCreateEvent", ev_out, 0u, 0u, 0u) == STATUS_SUCCESS);
  const uint32_t ev = rd(f, ev_out);
  def(kFnA, [&, ev](const std::vector<uint64_t>&) -> uint64_t {
    return gcall(f, "NtWaitForSingleObjectEx", ev, 0u, 0u, 0u);
  });
  const Created c = create(f, kFnA, 0, /*cpu mask: cpu 2*/ 0x04000000u);
  REQUIRE(c.status == STATUS_SUCCESS);
  auto t = thread_of(c.handle);
  CHECK(*f.mem.translate<uint8_t>(t->pcr() + offsetof(X_KPCR, current_cpu)) == 2);

  const uint32_t obj = f.alloc(4);
  CHECK(f.call("ObLookupThreadByThreadId", c.id, obj) == STATUS_SUCCESS);
  CHECK(rd(f, obj) == t->kthread());
  f.call("ObDereferenceObject", t->kthread());
  CHECK(static_cast<uint32_t>(f.call("ObLookupThreadByThreadId", 0xFFFFu, obj)) == 0xC0000225u);  // STATUS_NOT_FOUND

  CHECK(f.call("KeQueryBasePriorityThread", t->kthread()) == 0);
  CHECK(f.call("KeSetBasePriorityThread", t->kthread(), 5u) == 0);
  CHECK(f.call("KeQueryBasePriorityThread", t->kthread()) == 5);
  const uint32_t prev = f.alloc(4);
  CHECK(static_cast<uint32_t>(f.call("KeSetAffinityThread", t->kthread(), 0u, prev)) == STATUS_INVALID_PARAMETER);
  CHECK(f.call("KeSetAffinityThread", t->kthread(), 0x8u, prev) == STATUS_SUCCESS);
  CHECK(rd(f, prev) == 0x4);

  CHECK(f.call("KeGetCurrentProcessType") == 1);
  f.call("KeSetCurrentProcessType", 2u);
  CHECK(f.call("KeGetCurrentProcessType") == 2);

  CHECK(f.call("NtSetEvent", ev, 0u) == STATUS_SUCCESS);
  CHECK(wait_handle(f, c.handle) == STATUS_WAIT_0);
  t.reset();
  CHECK(f.call("NtClose", c.handle) == STATUS_SUCCESS);
  CHECK(f.call("NtClose", ev) == STATUS_SUCCESS);
}

TEST_CASE("run_main_thread runs the entry as guest thread 1 and returns 0", "[unit][kernel][threads]") {
  KernelFixture f;
  uint32_t id = 0, token = 0, current = 0;
  def(kFnA, [&](const std::vector<uint64_t>&) -> uint64_t {
    id = current_thread()->id();
    token = kernel().dispatcher().current_thread_token();
    // kCurrentThread resolves to this thread.
    auto obj = kernel().objects().get(kCurrentThread);
    current = obj ? obj->guest_header() : 0;
    return 5;
  });
  CHECK(run_main_thread(kFnA, 64 * 1024) == 0);
  CHECK(id == 1);
  CHECK(token != 0);
  CHECK(token == current);  // the dispatcher identifies a guest thread by its KTHREAD
}

TEST_CASE("missing import on a secondary thread exits 3 with report", "[unit][kernel][threads]") {
  auto r = run_child([] {
    KernelFixture f;
    def(kFnB, [](const std::vector<uint64_t>&) -> uint64_t {
      trace("Before");
      unimplemented_import("NtMissingThing", 0x82001234);
    });
    def(kFnA, [&f](const std::vector<uint64_t>&) -> uint64_t {
      const uint32_t out = f.alloc(8);
      (void)gcall(f, "ExCreateThread", out, 0u, out + 4, 0u, kFnB, 0u, 0u);
      return gcall(f, "NtWaitForSingleObjectEx", rd(f, out), 0u, 0u, 0u);
    });
    (void)run_main_thread(kFnA, 64 * 1024);
  });
  CHECK(r.code == 3);
  CHECK(r.err.find("Unimplemented import: NtMissingThing") != std::string::npos);
  CHECK(r.err.find("Called from: 0x82001234") != std::string::npos);
  CHECK(r.err.find("Thread: 2\n") != std::string::npos);
  CHECK(r.err.find("Trail (last 1): Before") != std::string::npos);  // that thread's own trail
}

TEST_CASE("a stray exception on a guest thread exits 4", "[unit][kernel][threads]") {
  auto r = run_child([] {
    KernelFixture f;
    def(kFnA, [](const std::vector<uint64_t>&) -> uint64_t { throw 7; });
    (void)run_main_thread(kFnA, 64 * 1024);
  });
  CHECK(r.code == 4);
  CHECK(r.err.find("Thread: 1") != std::string::npos);
}
