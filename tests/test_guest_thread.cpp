// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>

#include "runtime/be.h"
#include "runtime/guest_thread.h"
#include "runtime/kernel/registry.h"

using namespace lr::runtime;

namespace {
uint8_t* g_base;
pthread_t g_thread;
stack_t g_ss;
void* g_user;

void body_ok(uint8_t* base, void* user) {
  g_base = base;
  g_user = user;
  g_thread = pthread_self();
  sigaltstack(nullptr, &g_ss);
}

void body_unimpl(uint8_t*, void*) {
  lr::kernel::trace("Prev");
  lr::kernel::unimplemented_import("NtFoo", 0x82001234);
}

void body_throws(uint8_t*, void*) { throw 7; }

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

TEST_CASE("run_guest runs the body on another thread with an altstack", "[unit]") {
  auto mem = GuestMemory::reserve(0x1'0000'0000ull);
  REQUIRE(mem.ok());
  g_ss = {};
  int token = 0;
  CHECK(run_guest(mem.value(), body_ok, &token) == 0);
  CHECK(g_user == &token);
  CHECK(g_base == mem.value().base());
  CHECK_FALSE(pthread_equal(g_thread, pthread_self()));
  CHECK(g_ss.ss_sp != nullptr);  // install_thread_altstack ran on the guest thread
}

TEST_CASE("run_guest reports an unimplemented import and returns 3", "[unit]") {
  auto mem = GuestMemory::reserve(0x1'0000'0000ull);
  REQUIRE(mem.ok());
  int rc = -1;
  std::string err = capture_stderr([&] { rc = run_guest(mem.value(), body_unimpl, nullptr); });
  CHECK(rc == 3);
  CHECK(err.find("Unimplemented import: NtFoo") != std::string::npos);
  CHECK(err.find("Called from: 0x82001234") != std::string::npos);
  CHECK(err.find("Prev") != std::string::npos);
}

TEST_CASE("run_guest turns a stray exception into exit code 4", "[unit]") {
  auto mem = GuestMemory::reserve(0x1'0000'0000ull);
  REQUIRE(mem.ok());
  int rc = -1;
  std::string err = capture_stderr([&] { rc = run_guest(mem.value(), body_throws, nullptr); });
  CHECK(rc == 4);
  CHECK_FALSE(err.empty());
}

TEST_CASE("init_guest_pcr writes the KPCR fields big-endian", "[unit]") {
  auto mem = GuestMemory::reserve(0x1'0000'0000ull);
  REQUIRE(mem.ok());
  init_guest_pcr(mem.value());
  auto rd = [&](uint32_t off) { return uint32_t(*mem.value().translate<lr::runtime::be<uint32_t>>(0x7FFF0000 + off)); };
  CHECK(rd(0x00) == 0x7FFE0000u);
  CHECK(rd(0x30) == 0x7FFF0000u);
  CHECK(rd(0x70) == 0x70100000u);
  CHECK(rd(0x74) == 0x70000000u);
  CHECK(rd(0x100) == 0x7FFD0000u);
  CHECK(*mem.value().translate<uint8_t>(0x7FFF0000 + 0x10C) == 0);
  // raw byte order: tls_ptr 0x7FFE0000 -> 7F FE 00 00
  CHECK(mem.value().translate<uint8_t>(0x7FFF0000)[0] == 0x7F);
  CHECK(mem.value().translate<uint8_t>(0x7FFF0000)[1] == 0xFE);
}
