// SPDX-License-Identifier: MIT
// Host-thread helper and exit codes of runtime/guest_thread.h. Guest threads themselves (PCR, KTHREAD, TLS, exit
// paths) are covered by test_kernel_threads.cpp.
#include <catch2/catch_test_macros.hpp>
#include <pthread.h>

#include <atomic>
#include <thread>

#include "runtime/guest_thread.h"

using namespace lr::runtime;

namespace {
struct Seen {
  std::atomic<bool> done{false};
  pthread_t self{};
  size_t stack = 0;
};

void* record(void* p) {
  auto* s = static_cast<Seen*>(p);
  s->self = pthread_self();
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) == 0) {
    pthread_attr_getstacksize(&attr, &s->stack);
    pthread_attr_destroy(&attr);
  }
  s->done = true;
  return nullptr;
}
}  // namespace

TEST_CASE("start_host_thread runs fn on a detached thread with a 16 MiB stack", "[unit]") {
  Seen s;
  REQUIRE(start_host_thread(record, &s));
  while (!s.done) std::this_thread::yield();
  CHECK_FALSE(pthread_equal(s.self, pthread_self()));
  CHECK(s.stack >= kHostStackSize);
}

TEST_CASE("game exit codes", "[unit]") {
  STATIC_REQUIRE(kExitOk == 0);
  STATIC_REQUIRE(kExitUsage == 2);
  STATIC_REQUIRE(kExitUnimplementedImport == 3);
  STATIC_REQUIRE(kExitFault == 4);
}
