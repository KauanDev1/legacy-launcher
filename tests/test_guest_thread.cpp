// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>

#include "fake_generated/ppc_config.h"
#include "ppc_context.h"
#include "runtime/guest_thread.h"
#include "runtime/kernel/registry.h"

using namespace lr::runtime;

namespace {
uint64_t g_r1, g_r13;
uint8_t* g_base;
pthread_t g_thread;
stack_t g_ss;

void entry_ok(PPCContext& ctx, uint8_t* base) {
  g_r1 = ctx.r1.u64;
  g_r13 = ctx.r13.u64;
  g_base = base;
  g_thread = pthread_self();
  sigaltstack(nullptr, &g_ss);
}

void entry_unimpl(PPCContext& ctx, uint8_t* base) {
  (void)base;
  ctx.lr = 0x82001234;
  lr::kernel::trace("Prev");
  lr::kernel::unimplemented_import("NtFoo", uint32_t(ctx.lr));
}

void entry_throws(PPCContext&, uint8_t*) { throw 7; }

// Runs fn with stderr redirected to a temp file; returns what was written.
template <class F>
std::string capture_stderr(F fn) {
  char path[] = "/tmp/lr_stderr_XXXXXX";
  int fd = mkstemp(path);
  REQUIRE(fd >= 0);
  std::fflush(stderr);
  int saved = dup(2);
  dup2(fd, 2);
  fn();
  std::fflush(stderr);
  dup2(saved, 2);
  close(saved);
  close(fd);
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  std::remove(path);
  return ss.str();
}
}  // namespace

TEST_CASE("run_guest builds the context and runs on another thread with an altstack", "[unit]") {
  auto mem = GuestMemory::reserve(0x1'0000'0000ull);
  REQUIRE(mem.ok());
  g_ss = {};
  CHECK(run_guest(mem.value(), 0x82000100, entry_ok) == 0);
  CHECK(g_r1 == 0x70000000ull + 0x100000 - 0x80);
  CHECK(g_r13 == 0x7FFF0000ull);
  CHECK(g_base == mem.value().base());
  CHECK_FALSE(pthread_equal(g_thread, pthread_self()));
  CHECK(g_ss.ss_sp != nullptr);  // install_thread_altstack ran on the guest thread
}

TEST_CASE("run_guest reports an unimplemented import and returns 3", "[unit]") {
  auto mem = GuestMemory::reserve(0x1'0000'0000ull);
  REQUIRE(mem.ok());
  int rc = -1;
  std::string err = capture_stderr([&] { rc = run_guest(mem.value(), 0x82000100, entry_unimpl); });
  CHECK(rc == 3);
  CHECK(err.find("Unimplemented import: NtFoo") != std::string::npos);
  CHECK(err.find("Called from: 0x82001234") != std::string::npos);
  CHECK(err.find("Prev") != std::string::npos);
}

TEST_CASE("run_guest turns a stray exception into exit code 4", "[unit]") {
  auto mem = GuestMemory::reserve(0x1'0000'0000ull);
  REQUIRE(mem.ok());
  int rc = -1;
  std::string err = capture_stderr([&] { rc = run_guest(mem.value(), 0x82000100, entry_throws); });
  CHECK(rc == 4);
  CHECK_FALSE(err.empty());
}
