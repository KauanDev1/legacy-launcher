// SPDX-License-Identifier: MIT
// Dispatcher (runtime/kernel/dispatcher.h) and the event, semaphore, mutant and wait imports (exports/sync.cpp).
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <latch>
#include <memory>
#include <thread>
#include <vector>

#include "runtime/be.h"
#include "runtime/kernel/dispatcher.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/objects.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/status.h"
#include "support/kernel_fixture.h"

using namespace lr::kernel;
using lr::runtime::be;
using lr::testing::KernelFixture;
using namespace std::chrono_literals;

namespace {

uint32_t rd(KernelFixture& f, uint32_t addr) { return *f.mem.translate<be<uint32_t>>(addr); }
int32_t state(KernelFixture& f, uint32_t header) { return *f.mem.translate<be<int32_t>>(header + 4); }

// Thread-safe import call for worker threads (no Catch2 macros; at most 8 register arguments).
template <class... A>
uint32_t tcall(KernelFixture& f, const char* name, A... args) {
  static_assert(sizeof...(A) <= 8);
  const ImportEntry e = find_import(name);
  if (e.fn == nullptr) return 0xDEADBEEF;
  GuestCall c;
  c.base = f.mem.base();
  c.r1 = KernelFixture::kStackPointer;
  int i = 0;
  ((c.gpr[i++] = static_cast<uint64_t>(args)), ...);
  e.fn(c);
  return static_cast<uint32_t>(c.ret_gpr);
}

// Polls the dispatcher until `n` threads are blocked in a wait (bounded; no sleeps).
bool wait_for_waiters(size_t n) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (kernel().dispatcher().snapshot().size() != n) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::yield();
  }
  return true;
}

// A guest-initialized dispatcher header (what an inlined KeInitializeEvent leaves behind).
uint32_t guest_header(KernelFixture& f, uint8_t type, int32_t signal) {
  const uint32_t h = f.alloc(0x20);
  *f.mem.translate<uint8_t>(h) = type;
  *f.mem.translate<be<int32_t>>(h + 4) = signal;
  return h;
}

uint32_t create_event(KernelFixture& f, uint32_t type, uint32_t initial) {
  const uint32_t out = f.alloc(4);
  REQUIRE(f.call("NtCreateEvent", out, 0, type, initial) == STATUS_SUCCESS);
  return rd(f, out);
}

uint32_t timeout_ptr(KernelFixture& f, int64_t ticks) {
  const uint32_t p = f.alloc(8);
  *f.mem.translate<be<int64_t>>(p) = ticks;
  return p;
}

}  // namespace

TEST_CASE("timeout_from_guest", "[unit][kernel][sync]") {
  KernelFixture f;
  CHECK(timeout_from_guest({0}).kind == Timeout::Infinite);
  CHECK(timeout_from_guest({timeout_ptr(f, 0)}).kind == Timeout::Poll);
  const Timeout rel = timeout_from_guest({timeout_ptr(f, -200000)});
  CHECK(rel.kind == Timeout::Relative);
  CHECK(rel.ticks_100ns == 200000);
  const Timeout abs = timeout_from_guest({timeout_ptr(f, 130000000000000000)});
  CHECK(abs.kind == Timeout::Absolute);
  CHECK(abs.ticks_100ns == 130000000000000000);
}

TEST_CASE("manual-reset event releases all waiters", "[unit][kernel][sync]") {
  KernelFixture f;
  const uint32_t h = create_event(f, 0, 0);
  std::vector<uint32_t> results(3, 0xFFFFFFFF);
  std::vector<std::thread> ts;
  for (size_t i = 0; i < 3; ++i)
    ts.emplace_back([&, i] { results[i] = tcall(f, "NtWaitForSingleObjectEx", h, 1, 0, 0); });
  REQUIRE(wait_for_waiters(3));
  const uint32_t prev = f.alloc(4);
  CHECK(f.call("NtSetEvent", h, prev) == STATUS_SUCCESS);
  CHECK(rd(f, prev) == 0);
  for (auto& t : ts) t.join();
  for (uint32_t r : results) CHECK(r == STATUS_SUCCESS);
  // Notification events stay signaled until cleared.
  CHECK(f.call("NtWaitForSingleObjectEx", h, 1, 0, timeout_ptr(f, 0)) == STATUS_SUCCESS);
  CHECK(f.call("NtClearEvent", h) == STATUS_SUCCESS);
  CHECK(static_cast<uint32_t>(f.call("NtWaitForSingleObjectEx", h, 1, 0, timeout_ptr(f, 0))) == STATUS_TIMEOUT);
}

TEST_CASE("auto-reset event releases one", "[unit][kernel][sync]") {
  KernelFixture f;
  const uint32_t h = create_event(f, 1, 0);
  std::atomic<int> done{0};
  std::vector<std::thread> ts;
  for (int i = 0; i < 2; ++i)
    ts.emplace_back([&] {
      if (tcall(f, "NtWaitForSingleObjectEx", h, 1, 0, 0) == STATUS_SUCCESS) done.fetch_add(1);
    });
  REQUIRE(wait_for_waiters(2));
  CHECK(f.call("NtSetEvent", h, 0) == STATUS_SUCCESS);
  // Satisfaction happens inside NtSetEvent: exactly one waiter is left and the event is consumed.
  CHECK(kernel().dispatcher().snapshot().size() == 1);
  CHECK(state(f, kernel().objects().get(h)->guest_header()) == 0);
  CHECK(f.call("NtSetEvent", h, 0) == STATUS_SUCCESS);
  for (auto& t : ts) t.join();
  CHECK(done.load() == 2);
  CHECK(state(f, kernel().objects().get(h)->guest_header()) == 0);
}

TEST_CASE("NtPulseEvent wakes current waiters and leaves the event clear", "[unit][kernel][sync]") {
  KernelFixture f;
  const uint32_t h = create_event(f, 0, 0);
  uint32_t r = 0xFFFFFFFF;
  std::thread t([&] { r = tcall(f, "NtWaitForSingleObjectEx", h, 1, 0, 0); });
  REQUIRE(wait_for_waiters(1));
  CHECK(f.call("NtPulseEvent", h, 0) == STATUS_SUCCESS);
  t.join();
  CHECK(r == STATUS_SUCCESS);
  CHECK(state(f, kernel().objects().get(h)->guest_header()) == 0);
}

TEST_CASE("semaphore counts and limit", "[unit][kernel][sync]") {
  KernelFixture f;
  const uint32_t out = f.alloc(4), prev = f.alloc(4), poll = timeout_ptr(f, 0);
  CHECK(static_cast<uint32_t>(f.call("NtCreateSemaphore", out, 0, 4, 3)) == STATUS_INVALID_PARAMETER);
  REQUIRE(f.call("NtCreateSemaphore", out, 0, 2, 3) == STATUS_SUCCESS);
  const uint32_t h = rd(f, out);
  const uint32_t header = kernel().objects().get(h)->guest_header();
  CHECK(*f.mem.translate<uint8_t>(header) == 5);
  CHECK(*f.mem.translate<be<int32_t>>(header + 0x10) == 3);
  CHECK(f.call("NtWaitForSingleObjectEx", h, 1, 0, poll) == STATUS_SUCCESS);
  CHECK(f.call("NtWaitForSingleObjectEx", h, 1, 0, poll) == STATUS_SUCCESS);
  CHECK(static_cast<uint32_t>(f.call("NtWaitForSingleObjectEx", h, 1, 0, poll)) == STATUS_TIMEOUT);
  CHECK(f.call("NtReleaseSemaphore", h, 2, prev) == STATUS_SUCCESS);
  CHECK(rd(f, prev) == 0);
  CHECK(static_cast<uint32_t>(f.call("NtReleaseSemaphore", h, 2, prev)) == STATUS_SEMAPHORE_LIMIT_EXCEEDED);
  CHECK(state(f, header) == 2);  // unchanged
  CHECK(f.call("NtReleaseSemaphore", h, 1, prev) == STATUS_SUCCESS);
  CHECK(rd(f, prev) == 2);
  CHECK(state(f, header) == 3);
}

TEST_CASE("mutant recursion and release by non-owner", "[unit][kernel][sync]") {
  KernelFixture f;
  const uint32_t m = f.alloc(0x20), poll = timeout_ptr(f, 0);
  f.call("KeInitializeMutant", m, 0);
  CHECK(*f.mem.translate<uint8_t>(m) == 2);
  CHECK(state(f, m) == 1);
  CHECK(rd(f, m + 0x18) == 0);
  CHECK(f.call("KeWaitForSingleObject", m, 0, 0, 0, poll) == STATUS_SUCCESS);
  CHECK(f.call("KeWaitForSingleObject", m, 0, 0, 0, poll) == STATUS_SUCCESS);  // recursion by the owner
  CHECK(state(f, m) == -1);
  const uint32_t me = kernel().dispatcher().current_thread_token();
  CHECK(me != 0);
  CHECK(rd(f, m + 0x18) == me);

  uint32_t other_release = 0, other_wait = 0;
  std::thread t([&] {
    other_release = tcall(f, "KeReleaseMutant", m, 0, 0, 0);
    other_wait = tcall(f, "KeWaitForSingleObject", m, 0, 0, 0, poll);
  });
  t.join();
  CHECK(other_release == STATUS_MUTANT_NOT_OWNED);
  CHECK(other_wait == STATUS_TIMEOUT);

  CHECK(static_cast<int32_t>(f.call("KeReleaseMutant", m, 0, 0, 0)) == -1);  // previous signal state
  CHECK(static_cast<int32_t>(f.call("KeReleaseMutant", m, 0, 0, 0)) == 0);
  CHECK(state(f, m) == 1);
  CHECK(rd(f, m + 0x18) == 0);
  CHECK(static_cast<uint32_t>(f.call("KeReleaseMutant", m, 0, 0, 0)) == STATUS_MUTANT_NOT_OWNED);

  // A mutant initialized as owned belongs to the caller.
  const uint32_t m2 = f.alloc(0x20);
  f.call("KeInitializeMutant", m2, 1);
  CHECK(state(f, m2) == 0);
  CHECK(rd(f, m2 + 0x18) == me);

  // An abandoned mutant is acquired with STATUS_ABANDONED_WAIT_0.
  const uint32_t m3 = f.alloc(0x20);
  f.call("KeInitializeMutant", m3, 0);
  *f.mem.translate<be<uint32_t>>(m3 + 0x1C) = 1;
  CHECK(f.call("KeWaitForSingleObject", m3, 0, 0, 0, poll) == STATUS_ABANDONED_WAIT_0);
  CHECK(rd(f, m3 + 0x1C) == 0);
}

TEST_CASE("mutant released by its owner wakes a waiting thread", "[unit][kernel][sync]") {
  KernelFixture f;
  const uint32_t m = f.alloc(0x20);
  f.call("KeInitializeMutant", m, 1);
  uint32_t r = 0xFFFFFFFF, owner_seen = 0;
  std::thread t([&] {
    r = tcall(f, "KeWaitForSingleObject", m, 0, 0, 0, 0);
    owner_seen = kernel().dispatcher().current_thread_token();
    (void)tcall(f, "KeReleaseMutant", m, 0, 0, 0);
  });
  REQUIRE(wait_for_waiters(1));
  CHECK(f.call("KeReleaseMutant", m, 0, 0, 0) == 0);
  t.join();
  CHECK(r == STATUS_SUCCESS);
  CHECK(owner_seen != kernel().dispatcher().current_thread_token());
  CHECK(state(f, m) == 1);
}

TEST_CASE("wait any returns index", "[unit][kernel][sync]") {
  KernelFixture f;
  const uint32_t e0 = guest_header(f, 0, 0), e1 = guest_header(f, 1, 0), e2 = guest_header(f, 1, 1);
  const uint32_t arr = f.alloc(12);
  *f.mem.translate<be<uint32_t>>(arr) = e0;
  *f.mem.translate<be<uint32_t>>(arr + 4) = e1;
  *f.mem.translate<be<uint32_t>>(arr + 8) = e2;
  const uint32_t poll = timeout_ptr(f, 0);
  CHECK(f.call("KeWaitForMultipleObjects", 3, arr, 1 /* WaitAny */, 0, 0, 0, poll, 0) == 2);
  CHECK(state(f, e2) == 0);  // synchronization event consumed
  CHECK(static_cast<uint32_t>(f.call("KeWaitForMultipleObjects", 3, arr, 1, 0, 0, 0, poll, 0)) == STATUS_TIMEOUT);

  uint32_t r = 0xFFFFFFFF;
  std::thread t([&] { r = tcall(f, "KeWaitForMultipleObjects", 3, arr, 1, 0, 0, 0, 0, 0); });
  REQUIRE(wait_for_waiters(1));
  f.call("KeSetEvent", e1, 0, 0);
  t.join();
  CHECK(r == 1);
  CHECK(state(f, e1) == 0);

  // Handle variant; an invalid wait type is rejected.
  const uint32_t h0 = create_event(f, 0, 0), h1 = create_event(f, 0, 1);
  const uint32_t handles = f.alloc(8);
  *f.mem.translate<be<uint32_t>>(handles) = h0;
  *f.mem.translate<be<uint32_t>>(handles + 4) = h1;
  CHECK(f.call("NtWaitForMultipleObjectsEx", 2, handles, 1, 1, 0, poll) == 1);
  CHECK(static_cast<uint32_t>(f.call("NtWaitForMultipleObjectsEx", 2, handles, 7, 1, 0, poll)) ==
        STATUS_INVALID_PARAMETER);
}

TEST_CASE("wait all", "[unit][kernel][sync]") {
  KernelFixture f;
  const uint32_t a = guest_header(f, 1, 1), b = guest_header(f, 1, 0);
  const uint32_t arr = f.alloc(8);
  *f.mem.translate<be<uint32_t>>(arr) = a;
  *f.mem.translate<be<uint32_t>>(arr + 4) = b;
  const uint32_t poll = timeout_ptr(f, 0);
  CHECK(static_cast<uint32_t>(f.call("KeWaitForMultipleObjects", 2, arr, 0 /* WaitAll */, 0, 0, 0, poll, 0)) ==
        STATUS_TIMEOUT);
  CHECK(state(f, a) == 1);  // all or nothing: a was not consumed

  uint32_t r = 0xFFFFFFFF;
  std::thread t([&] { r = tcall(f, "KeWaitForMultipleObjects", 2, arr, 0, 0, 0, 0, 0, 0); });
  REQUIRE(wait_for_waiters(1));
  f.call("KeSetEvent", b, 0, 0);
  t.join();
  CHECK(r == STATUS_SUCCESS);
  CHECK(state(f, a) == 0);
  CHECK(state(f, b) == 0);
}

TEST_CASE("relative timeout returns STATUS_TIMEOUT within 50 ms of 20 ms", "[unit][kernel][sync]") {
  KernelFixture f;
  const uint32_t e = guest_header(f, 0, 0);
  const uint32_t t20 = timeout_ptr(f, -200000);  // 20 ms in 100 ns units
  const auto start = std::chrono::steady_clock::now();
  CHECK(static_cast<uint32_t>(f.call("KeWaitForSingleObject", e, 0, 0, 0, t20)) == STATUS_TIMEOUT);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  CHECK(elapsed >= 20ms);
  CHECK(elapsed <= 70ms);
}

TEST_CASE("absolute timeout in the past times out at once", "[unit][kernel][sync]") {
  KernelFixture f;
  const uint32_t e = guest_header(f, 0, 0);
  const uint32_t past = timeout_ptr(f, 116444736000000000);  // 1970-01-01 as FILETIME
  const auto start = std::chrono::steady_clock::now();
  CHECK(static_cast<uint32_t>(f.call("KeWaitForSingleObject", e, 0, 0, 0, past)) == STATUS_TIMEOUT);
  CHECK(std::chrono::steady_clock::now() - start < 20ms);
}

TEST_CASE("poll returns immediately", "[unit][kernel][sync]") {
  KernelFixture f;
  const uint32_t e = guest_header(f, 0, 0);
  const auto start = std::chrono::steady_clock::now();
  CHECK(static_cast<uint32_t>(f.call("KeWaitForSingleObject", e, 0, 0, 0, timeout_ptr(f, 0))) == STATUS_TIMEOUT);
  CHECK(std::chrono::steady_clock::now() - start < 20ms);
  CHECK(kernel().dispatcher().snapshot().empty());
}

TEST_CASE("event initialized by guest memory only (no handle) can be waited on", "[unit][kernel][sync]") {
  KernelFixture f;
  const uint32_t e = guest_header(f, 1, 0);
  REQUIRE(kernel().objects().by_guest_header(e) == nullptr);
  std::latch started(1);
  uint32_t r = 0xFFFFFFFF;
  std::thread t([&] {
    started.count_down();
    r = tcall(f, "KeWaitForSingleObject", e, 0, 0, 0, 0);
  });
  started.wait();
  REQUIRE(wait_for_waiters(1));
  const auto snap = kernel().dispatcher().snapshot();
  REQUIRE(snap.size() == 1);
  CHECK(snap[0].headers == std::vector<uint32_t>{e});
  CHECK(snap[0].infinite);
  CHECK(snap[0].type == WaitType::Any);
  CHECK(f.call("KeSetEvent", e, 0, 0) == 0);  // previous state
  t.join();
  CHECK(r == STATUS_SUCCESS);
  *f.mem.translate<be<int32_t>>(e + 4) = 1;
  CHECK(f.call("KeResetEvent", e) == 1);
  CHECK(state(f, e) == 0);
}

TEST_CASE("wait on file handle", "[unit][kernel][sync]") {
  KernelFixture f;
  const uint32_t h = kernel().objects().add(std::make_shared<KObject>(ObjectKind::File));
  CHECK(static_cast<uint32_t>(f.call("NtWaitForSingleObjectEx", h, 1, 0, timeout_ptr(f, 0))) ==
        STATUS_OBJECT_TYPE_MISMATCH);
  CHECK(static_cast<uint32_t>(f.call("NtWaitForSingleObjectEx", 0xF8000400, 1, 0, 0)) == STATUS_INVALID_HANDLE);
  CHECK(static_cast<uint32_t>(f.call("NtSetEvent", h, 0)) == STATUS_OBJECT_TYPE_MISMATCH);
}

TEST_CASE("alertable waits return STATUS_USER_APC on wake_for_apc", "[unit][kernel][sync]") {
  KernelFixture f;
  const uint32_t e = guest_header(f, 0, 0);
  std::atomic<uint32_t> token{0};
  uint32_t r = 0xFFFFFFFF;
  std::thread t([&] {
    token = kernel().dispatcher().current_thread_token();
    r = tcall(f, "KeWaitForSingleObject", e, 0, 0, 1 /* alertable */, 0);
  });
  REQUIRE(wait_for_waiters(1));
  kernel().dispatcher().wake_for_apc(token.load());
  t.join();
  CHECK(r == STATUS_USER_APC);

  // Non-alertable waits ignore the wake (it stays pending for the next alertable wait).
  const uint32_t t20 = timeout_ptr(f, -200000);
  kernel().dispatcher().wake_for_apc(kernel().dispatcher().current_thread_token());
  CHECK(static_cast<uint32_t>(f.call("KeWaitForSingleObject", e, 0, 0, 0, t20)) == STATUS_TIMEOUT);
  CHECK(static_cast<uint32_t>(f.call("KeWaitForSingleObject", e, 0, 0, 1, t20)) == STATUS_USER_APC);
  CHECK(static_cast<uint32_t>(f.call("KeWaitForSingleObject", e, 0, 0, 1, timeout_ptr(f, 0))) == STATUS_TIMEOUT);
}

TEST_CASE("NtSignalAndWaitForSingleObjectEx signals then waits", "[unit][kernel][sync]") {
  KernelFixture f;
  const uint32_t sig = create_event(f, 0, 0), wait = create_event(f, 1, 1);
  CHECK(f.call("NtSignalAndWaitForSingleObjectEx", sig, wait, 0, 0, timeout_ptr(f, 0)) == STATUS_SUCCESS);
  CHECK(state(f, kernel().objects().get(sig)->guest_header()) == 1);
  CHECK(state(f, kernel().objects().get(wait)->guest_header()) == 0);
  CHECK(static_cast<uint32_t>(f.call("NtSignalAndWaitForSingleObjectEx", sig, 0xF8000400, 0, 0, 0)) ==
        STATUS_INVALID_HANDLE);
}

TEST_CASE("closing a created event frees nothing a waiter still uses", "[unit][kernel][sync]") {
  KernelFixture f;
  const uint32_t h = create_event(f, 0, 0);
  uint32_t r = 0xFFFFFFFF;
  std::thread t([&] { r = tcall(f, "NtWaitForSingleObjectEx", h, 1, 0, 0); });
  REQUIRE(wait_for_waiters(1));
  const uint32_t header = kernel().objects().get(h)->guest_header();
  CHECK(f.call("NtClose", h) == STATUS_SUCCESS);
  f.call("KeSetEvent", header, 0, 0);  // the header is still valid: the waiter holds the object
  t.join();
  CHECK(r == STATUS_SUCCESS);
}
