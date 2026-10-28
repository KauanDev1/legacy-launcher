// SPDX-License-Identifier: MIT
// Time, timers, DPCs and spinlocks (runtime/kernel/timers.h, exports/time.cpp). "Guest" functions are host lambdas
// behind a test GuestGlue keyed by guest address (same pattern as test_kernel_threads.cpp).
#include <catch2/catch_test_macros.hpp>

#include <sys/wait.h>
#include <unistd.h>
#include <x86intrin.h>

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
#include "runtime/kernel/registry.h"
#include "runtime/kernel/status.h"
#include "runtime/kernel/threads.h"
#include "runtime/kernel/timers.h"
#include "runtime/kernel/variables.h"
#include "support/kernel_fixture.h"

using namespace lr::kernel;
using lr::runtime::be;
using lr::testing::KernelFixture;
using namespace std::chrono_literals;

namespace {

using GuestFn = std::function<uint64_t(const std::vector<uint64_t>&)>;
std::mutex g_fns_mutex;
std::map<uint32_t, GuestFn> g_fns;
thread_local uint32_t t_r1 = 0, t_r13 = 0;

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

// Import call from inside a "guest" function, like a thunk, on the calling thread's frame.
template <class... A>
uint64_t gcall(KernelFixture& f, const char* name, A... args) {
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
  return c.ret_gpr;
}

uint32_t rd(KernelFixture& f, uint32_t addr) { return *f.mem.translate<be<uint32_t>>(addr); }

constexpr uint32_t kFnA = 0x82000500, kFnB = 0x82000510;

uint32_t create_timer(KernelFixture& f, uint32_t type) {
  const uint32_t out = f.alloc(4);
  REQUIRE(f.call("NtCreateTimer", out, 0u, type) == STATUS_SUCCESS);
  return rd(f, out);
}

uint32_t wait_handle(KernelFixture& f, uint32_t handle, int64_t relative_100ns) {
  const uint32_t timeout = f.alloc(8);
  *f.mem.translate<be<int64_t>>(timeout) = -relative_100ns;
  return static_cast<uint32_t>(f.call("NtWaitForSingleObjectEx", handle, 0u, 0u, timeout));
}

uint32_t create_thread(KernelFixture& f, uint32_t start, uint32_t context) {
  const uint32_t out = f.alloc(8);
  REQUIRE(f.call("ExCreateThread", out, 0u, out + 4, 0u, start, context, 0u) == STATUS_SUCCESS);
  return rd(f, out);
}

int64_t host_filetime() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                 .count() / 100 + 116444736000000000;
}

}  // namespace

TEST_CASE("performance frequency matches rdtsc rate within 2%", "[unit][kernel][time]") {
  KernelFixture f;
  const uint64_t reported = f.call("KeQueryPerformanceFrequency");
  const auto t0 = std::chrono::steady_clock::now();
  const uint64_t c0 = __rdtsc();
  std::this_thread::sleep_for(100ms);
  const uint64_t c1 = __rdtsc();
  const auto t1 = std::chrono::steady_clock::now();
  const double measured = static_cast<double>(c1 - c0) / std::chrono::duration<double>(t1 - t0).count();
  INFO("reported " << reported << " measured " << measured);
  CHECK(reported > 100'000'000ull);  // far from the console's 50 MHz
  CHECK(static_cast<double>(reported) > measured * 0.98);
  CHECK(static_cast<double>(reported) < measured * 1.02);
  CHECK(f.call("KeQueryPerformanceFrequency") == reported);  // cached
}

TEST_CASE("system time is close to host clock", "[unit][kernel][time]") {
  KernelFixture f;
  const uint32_t out = f.alloc(8);
  const int64_t before = host_filetime();
  f.call("KeQuerySystemTime", out);
  const int64_t after = host_filetime();
  const int64_t t = *f.mem.translate<be<int64_t>>(out);
  CHECK(t >= before - 10'000);  // 1 ms of slack between the two clocks reads
  CHECK(t <= after + 10'000);
}

TEST_CASE("time fields round trip (2026-10-16 12:34:56.789)", "[unit][kernel][time]") {
  KernelFixture f;
  const uint32_t fields = f.alloc(sizeof(X_TIME_FIELDS));
  const uint32_t time = f.alloc(8);
  auto* tf = f.mem.translate<X_TIME_FIELDS>(fields);
  tf->year = 2026;
  tf->month = 10;
  tf->day = 16;
  tf->hour = 12;
  tf->minute = 34;
  tf->second = 56;
  tf->milliseconds = 789;
  tf->weekday = 0;  // ignored on input
  REQUIRE((f.call("RtlTimeFieldsToTime", fields, time) & 0xFF) == 1);
  const int64_t ft = *f.mem.translate<be<int64_t>>(time);
  // 2026-10-16 is day 20742 since 1970-01-01.
  const int64_t expected = (int64_t{20742} * 86400 + 12 * 3600 + 34 * 60 + 56) * 10'000'000 + 789 * 10'000 +
                           116444736000000000;
  CHECK(ft == expected);

  const uint32_t back = f.alloc(sizeof(X_TIME_FIELDS));
  f.call("RtlTimeToTimeFields", time, back);
  const auto* bf = f.mem.translate<X_TIME_FIELDS>(back);
  CHECK(bf->year == 2026);
  CHECK(bf->month == 10);
  CHECK(bf->day == 16);
  CHECK(bf->hour == 12);
  CHECK(bf->minute == 34);
  CHECK(bf->second == 56);
  CHECK(bf->milliseconds == 789);
  CHECK(bf->weekday == 5);  // Friday

  // Epoch of FILETIME: 1601-01-01 was a Monday.
  *f.mem.translate<be<int64_t>>(time) = 0;
  f.call("RtlTimeToTimeFields", time, back);
  CHECK(bf->year == 1601);
  CHECK(bf->month == 1);
  CHECK(bf->day == 1);
  CHECK(bf->weekday == 1);

  // Leap day and invalid fields.
  tf->year = 2024;
  tf->month = 2;
  tf->day = 29;
  CHECK((f.call("RtlTimeFieldsToTime", fields, time) & 0xFF) == 1);
  tf->year = 2026;
  CHECK((f.call("RtlTimeFieldsToTime", fields, time) & 0xFF) == 0);  // 2026 is not a leap year
  tf->month = 13;
  CHECK((f.call("RtlTimeFieldsToTime", fields, time) & 0xFF) == 0);
  tf->month = 1;
  tf->year = 1600;
  CHECK((f.call("RtlTimeFieldsToTime", fields, time) & 0xFF) == 0);
}

TEST_CASE("periodic timer signals and requeues", "[unit][kernel][time]") {
  KernelFixture f;
  const uint32_t h = create_timer(f, 1);  // synchronization: auto-reset on wait
  auto obj = kernel().objects().get(h);
  REQUIRE(obj);
  CHECK(*f.mem.translate<uint8_t>(obj->guest_header()) == kSynchronizationTimer);
  CHECK(rd(f, obj->guest_header() + 4) == 0);

  const uint32_t due = f.alloc(8);
  *f.mem.translate<be<int64_t>>(due) = -100'000;  // 10 ms
  const auto start = std::chrono::steady_clock::now();
  CHECK(f.call("NtSetTimerEx", h, due, 0u, 1u, 0u, 0u, /*period_ms*/ 10u, 0u) == STATUS_SUCCESS);
  for (int i = 0; i < 3; ++i) CHECK(wait_handle(f, h, 10'000'000) == STATUS_WAIT_0);  // 1 s each
  CHECK(std::chrono::steady_clock::now() - start >= 25ms);

  // A one-shot notification timer stays signaled and does not requeue.
  const uint32_t n = create_timer(f, 0);
  CHECK(*f.mem.translate<uint8_t>(kernel().objects().get(n)->guest_header()) == kNotificationTimer);
  CHECK(f.call("NtSetTimerEx", n, due, 0u, 1u, 0u, 0u, 0u, 0u) == STATUS_SUCCESS);
  CHECK(wait_handle(f, n, 10'000'000) == STATUS_WAIT_0);
  CHECK(wait_handle(f, n, 10'000) == STATUS_WAIT_0);  // still signaled
  CHECK(f.call("NtClose", h) == STATUS_SUCCESS);
  CHECK(f.call("NtClose", n) == STATUS_SUCCESS);

  CHECK(static_cast<uint32_t>(f.call("NtCreateTimer", f.alloc(4), 0u, 2u)) == STATUS_INVALID_PARAMETER);
  CHECK(static_cast<uint32_t>(f.call("NtSetTimerEx", 0xF8FFFFF0u, due, 0u, 1u, 0u, 0u, 0u, 0u)) ==
        STATUS_INVALID_HANDLE);
}

TEST_CASE("timer APC runs on the thread that set the timer", "[unit][kernel][time]") {
  KernelFixture f;
  const uint32_t h = create_timer(f, 0);
  const uint32_t due = f.alloc(8);
  *f.mem.translate<be<int64_t>>(due) = -50'000;  // 5 ms
  const uint32_t interval = f.alloc(8);
  *f.mem.translate<be<int64_t>>(interval) = -100'000'000;  // 10 s
  std::vector<uint64_t> apc_args;
  GuestThread* apc_thread = nullptr;
  GuestThread* setter = nullptr;
  def(kFnB, [&](const std::vector<uint64_t>& a) -> uint64_t {
    apc_args = a;
    apc_thread = current_thread();
    return 0;
  });
  def(kFnA, [&](const std::vector<uint64_t>&) -> uint64_t {
    setter = current_thread();
    if (gcall(f, "NtSetTimerEx", h, due, kFnB, 1u, 0x77u, 0u, 0u, 0u) != STATUS_SUCCESS) return 1;
    return gcall(f, "KeDelayExecutionThread", 1u, 1u, interval);  // alertable
  });
  const int64_t before = host_filetime();
  const uint32_t t = create_thread(f, kFnA, 0);
  CHECK(wait_handle(f, t, 50'000'000) == STATUS_WAIT_0);
  CHECK(std::static_pointer_cast<GuestThread>(kernel().objects().get(t))->exit_code() == STATUS_USER_APC);
  CHECK(apc_thread == setter);
  REQUIRE(apc_args.size() == 3);
  CHECK(apc_args[0] == 0x77);
  const int64_t fired = static_cast<int64_t>((apc_args[2] << 32) | (apc_args[1] & 0xFFFFFFFF));  // (low, high)
  CHECK(fired >= before);
  CHECK(fired <= host_filetime());
  CHECK(f.call("NtClose", t) == STATUS_SUCCESS);
  CHECK(f.call("NtClose", h) == STATUS_SUCCESS);
}

TEST_CASE("cancel timer", "[unit][kernel][time]") {
  KernelFixture f;
  const uint32_t h = create_timer(f, 0);
  const uint32_t due = f.alloc(8);
  *f.mem.translate<be<int64_t>>(due) = -500'000;  // 50 ms
  const uint32_t state = f.alloc(4);
  CHECK(f.call("NtSetTimerEx", h, due, 0u, 1u, 0u, 0u, 5u, 0u) == STATUS_SUCCESS);
  *f.mem.translate<be<uint32_t>>(state) = 0xFFFF;
  CHECK(f.call("NtCancelTimer", h, state) == STATUS_SUCCESS);
  CHECK(rd(f, state) == 0);  // was not signaled
  CHECK(wait_handle(f, h, 1'500'000) == STATUS_TIMEOUT);  // 150 ms: never fires
  CHECK_FALSE(kernel().timers().has_pending_work());

  // Cancel keeps an already signaled state; re-arming resets it.
  *f.mem.translate<be<int64_t>>(due) = -10'000;
  CHECK(f.call("NtSetTimerEx", h, due, 0u, 1u, 0u, 0u, 0u, 0u) == STATUS_SUCCESS);
  CHECK(wait_handle(f, h, 10'000'000) == STATUS_WAIT_0);
  CHECK(f.call("NtCancelTimer", h, state) == STATUS_SUCCESS);
  CHECK(rd(f, state) == 1);
  *f.mem.translate<be<int64_t>>(due) = -100'000'000;  // 10 s
  CHECK(f.call("NtSetTimerEx", h, due, 0u, 1u, 0u, 0u, 0u, 0u) == STATUS_SUCCESS);
  CHECK(rd(f, kernel().objects().get(h)->guest_header() + 4) == 0);
  // Closing the last handle of an armed timer disarms it before its body is freed.
  CHECK(f.call("NtClose", h) == STATUS_SUCCESS);
  CHECK_FALSE(kernel().timers().has_pending_work());
}

TEST_CASE("DPC routine runs once on worker", "[unit][kernel][time]") {
  KernelFixture f;
  const uint32_t blocker = f.alloc(sizeof(X_KDPC));
  const uint32_t dpc = f.alloc(sizeof(X_KDPC));
  std::atomic<bool> release{false};
  std::atomic<int> blocker_runs{0}, runs{0};
  std::vector<uint64_t> args;
  GuestThread* worker = nullptr;
  uint32_t irql = 0xFF;
  uint32_t r13 = 0;
  def(kFnA, [&](const std::vector<uint64_t>&) -> uint64_t {
    ++blocker_runs;
    while (!release) std::this_thread::yield();
    return 0;
  });
  def(kFnB, [&](const std::vector<uint64_t>& a) -> uint64_t {
    args = a;
    worker = current_thread();
    irql = current_irql();
    r13 = t_r13;
    ++runs;
    return 0;
  });
  f.call("KeInitializeDpc", blocker, kFnA, 0u);
  f.call("KeInitializeDpc", dpc, kFnB, 0xC0u);
  CHECK(rd(f, dpc) >> 24 == 19);  // DpcObject
  CHECK(rd(f, dpc + offsetof(X_KDPC, routine)) == kFnB);
  CHECK(rd(f, dpc + offsetof(X_KDPC, context)) == 0xC0);

  // Hold the worker in the first DPC so the second one stays queued.
  CHECK((f.call("KeInsertQueueDpc", blocker, 0u, 0u) & 0xFF) == 1);
  while (blocker_runs == 0) std::this_thread::yield();
  CHECK((f.call("KeInsertQueueDpc", dpc, 0xA1u, 0xA2u) & 0xFF) == 1);
  CHECK((f.call("KeInsertQueueDpc", dpc, 0xB1u, 0xB2u) & 0xFF) == 0);  // already queued
  CHECK(kernel().timers().has_pending_work());
  release = true;
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (runs == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  std::this_thread::sleep_for(20ms);
  CHECK(runs == 1);
  CHECK(blocker_runs == 1);
  CHECK(args == std::vector<uint64_t>{dpc, 0xC0, 0xA1, 0xA2});
  // On the kernel worker: a guest context of its own, at DISPATCH_LEVEL, but no handle and not a live guest thread.
  REQUIRE(worker != nullptr);
  CHECK(worker->handle() == 0);
  CHECK(r13 == worker->pcr());
  CHECK(irql == 2);
  CHECK(kernel().threads().live().empty());

  // Once run, it can be queued again.
  CHECK((f.call("KeInsertQueueDpc", dpc, 1u, 2u) & 0xFF) == 1);
  while (runs < 2 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  CHECK(runs == 2);
}

TEST_CASE("spinlock excludes two threads", "[unit][kernel][time]") {
  KernelFixture f;
  const uint32_t lock = f.alloc(4);
  constexpr int kIters = 20000;
  uint64_t counter = 0;  // deliberately not atomic: the spinlock protects it
  std::atomic<int> bad_irql{0}, bad_owner{0};
  def(kFnA, [&](const std::vector<uint64_t>& a) -> uint64_t {
    const bool raised = a.at(0) != 0;
    for (int i = 0; i < kIters; ++i) {
      if (raised) {
        gcall(f, "KeAcquireSpinLockAtRaisedIrql", lock);
        if (rd(f, lock) != current_thread()->kthread()) ++bad_owner;
        ++counter;
        gcall(f, "KeReleaseSpinLockFromRaisedIrql", lock);
      } else {
        const uint32_t old = static_cast<uint32_t>(gcall(f, "KfAcquireSpinLock", lock));
        if (old != 0 || current_irql() != 2) ++bad_irql;
        if (rd(f, lock) != current_thread()->kthread()) ++bad_owner;
        ++counter;
        gcall(f, "KfReleaseSpinLock", lock, old);
        if (current_irql() != 0) ++bad_irql;
      }
    }
    return 0;
  });
  const uint32_t a = create_thread(f, kFnA, 0);
  const uint32_t b = create_thread(f, kFnA, 0);
  const uint32_t c = create_thread(f, kFnA, 1);
  CHECK(wait_handle(f, a, 300'000'000) == STATUS_WAIT_0);
  CHECK(wait_handle(f, b, 300'000'000) == STATUS_WAIT_0);
  CHECK(wait_handle(f, c, 300'000'000) == STATUS_WAIT_0);
  CHECK(counter == 3 * kIters);
  CHECK(bad_irql == 0);
  CHECK(bad_owner == 0);
  CHECK(rd(f, lock) == 0);
  for (uint32_t h : {a, b, c}) CHECK(f.call("NtClose", h) == STATUS_SUCCESS);
}

TEST_CASE("KeTimeStampBundle is kept current once imported", "[unit][kernel][time]") {
  KernelFixture f;
  CHECK(variable_state("KeTimeStampBundle") == ImportState::Implemented);
  const uint32_t bundle = f.alloc(sizeof(X_TIME_STAMP_BUNDLE));
  kernel().timers().enable_time_stamp_bundle(bundle);
  const auto* b = f.mem.translate<X_TIME_STAMP_BUNDLE>(bundle);
  const uint32_t t0 = b->tick_count;
  const uint64_t i0 = b->interrupt_time;
  std::this_thread::sleep_for(50ms);
  const uint32_t t1 = b->tick_count;
  CHECK(t1 - t0 >= 30);
  CHECK(t1 - t0 <= 1000);
  CHECK(b->interrupt_time > i0);
  const int64_t now = host_filetime();
  const int64_t sys = static_cast<int64_t>(static_cast<uint64_t>(b->system_time));
  CHECK(sys <= now);
  CHECK(sys > now - 10'000'000);  // within 1 s
}

TEST_CASE("the TSC rate is measured at kernel init", "[unit][kernel][time]") {
  KernelFixture f;
  CHECK(kernel().timers().tsc_hz() > 100'000'000ull);  // before any KeQueryPerformanceFrequency call
  CHECK(kernel().timers().tsc_hz() == process_tsc_hz());
}

TEST_CASE("NtSetTimerEx rejects a negative period", "[unit][kernel][time]") {
  KernelFixture f;
  const uint32_t h = create_timer(f, 0);
  const uint32_t due = f.alloc(8);
  *f.mem.translate<be<int64_t>>(due) = -10'000;
  CHECK(static_cast<uint32_t>(f.call("NtSetTimerEx", h, due, 0u, 1u, 0u, 0u, -5, 0u)) == STATUS_INVALID_PARAMETER);
  CHECK_FALSE(kernel().timers().has_pending_work());
  CHECK(f.call("NtClose", h) == STATUS_SUCCESS);
}

TEST_CASE("a DPC without a routine is a guest fatal error", "[unit][kernel][time]") {
  int fds[2];
  REQUIRE(::pipe(fds) == 0);
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::dup2(fds[1], 2);
    ::close(fds[0]);
    {
      KernelFixture f;
      const uint32_t dpc = f.alloc(sizeof(X_KDPC));
      f.call("KeInitializeDpc", dpc, 0u, 0u);
      f.call("KeInsertQueueDpc", dpc, 0u, 0u);
    }
    ::_exit(99);
  }
  ::close(fds[1]);
  std::string out;
  char buf[256];
  for (ssize_t n; (n = ::read(fds[0], buf, sizeof buf)) > 0;) out.append(buf, static_cast<size_t>(n));
  ::close(fds[0]);
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 5);
  CHECK(out.find("DPC without a routine") != std::string::npos);
}

TEST_CASE("the worker's TLS block holds the XEX TLS template", "[unit][kernel][time]") {
  KernelFixture f;
  const uint32_t tmpl = f.alloc(0x10);
  for (uint32_t i = 0; i < 0x10; ++i) *f.mem.translate<uint8_t>(tmpl + i) = static_cast<uint8_t>(0xB0 + i);
  REQUIRE(kernel().threads().set_image(lr::formats::TlsInfo{100, tmpl, 0x20, 0x10}, 0, tmpl, 0x10));
  const uint32_t dpc = f.alloc(sizeof(X_KDPC));
  std::atomic<bool> done{false};
  std::vector<uint8_t> bytes;
  uint32_t kt_tls = 0, tls = 0;
  uint64_t set_ok = 0, got = 0;
  def(kFnA, [&](const std::vector<uint64_t>&) -> uint64_t {
    tls = rd(f, t_r13 + offsetof(X_KPCR, tls_ptr));
    kt_tls = rd(f, current_thread()->kthread() + offsetof(X_KTHREAD, tls_address));
    bytes.assign(f.mem.translate<uint8_t>(tls), f.mem.translate<uint8_t>(tls) + 0x20);
    set_ok = gcall(f, "KeTlsSetValue", 99u, 0x1234u);  // slot 99 exists: the header asks for 100
    got = gcall(f, "KeTlsGetValue", 99u);
    done = true;
    return 0;
  });
  f.call("KeInitializeDpc", dpc, kFnA, 0u);
  CHECK((f.call("KeInsertQueueDpc", dpc, 0u, 0u) & 0xFF) == 1);
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!done && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  REQUIRE(done);
  std::vector<uint8_t> want(0x20, 0);
  for (uint32_t i = 0; i < 0x10; ++i) want[i] = static_cast<uint8_t>(0xB0 + i);
  CHECK(bytes == want);
  CHECK(tls == kt_tls);
  CHECK(set_ok == 1);
  CHECK(got == 0x1234);
  // The worker froze the TLS layout like any thread.
  CHECK_FALSE(kernel().threads().set_image(std::nullopt, 0, tmpl, 0x10));
}
