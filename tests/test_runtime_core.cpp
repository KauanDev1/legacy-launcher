// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>

#include "runtime/be.h"

using lr::runtime::be;

TEST_CASE("be<uint32_t> stores big-endian bytes", "[unit]") {
  be<uint32_t> v;
  v = 0x11223344u;
  uint8_t b[4];
  std::memcpy(b, &v, 4);
  CHECK(b[0] == 0x11);
  CHECK(b[1] == 0x22);
  CHECK(b[2] == 0x33);
  CHECK(b[3] == 0x44);
  CHECK(static_cast<uint32_t>(v) == 0x11223344u);
}

TEST_CASE("be<float>/be<double>/be<uint16_t> round-trip", "[unit]") {
  be<float> f;
  f = 1.5f;
  CHECK(static_cast<float>(f) == 1.5f);
  uint8_t fb[4];
  std::memcpy(fb, &f, 4);
  CHECK(fb[0] == 0x3F);  // 1.5f == 0x3FC00000
  CHECK(fb[1] == 0xC0);
  CHECK(fb[2] == 0x00);
  CHECK(fb[3] == 0x00);
  be<double> d;
  d = -2.25;
  CHECK(static_cast<double>(d) == -2.25);
  be<uint16_t> s(0xABCD);
  uint8_t b[2];
  std::memcpy(b, &s, 2);
  CHECK(b[0] == 0xAB);
  CHECK(static_cast<uint16_t>(s) == 0xABCD);
  be<int32_t> n(-2);
  CHECK(static_cast<int32_t>(n) == -2);
  uint8_t nb[4];
  std::memcpy(nb, &n, 4);
  CHECK(nb[0] == 0xFF);
  CHECK(nb[3] == 0xFE);
  be<uint64_t> q(0x0102030405060708ull);
  uint8_t qb[8];
  std::memcpy(qb, &q, 8);
  for (int k = 0; k < 8; ++k) CHECK(qb[k] == k + 1);
  CHECK(static_cast<uint64_t>(q) == 0x0102030405060708ull);
  static_assert(sizeof(be<uint32_t>) == 4);
  static_assert(sizeof(be<double>) == 8);
}

#include "runtime/guest_memory.h"

using lr::runtime::GuestMemory;

TEST_CASE("GuestMemory reserves 4 GiB and translates guest addresses", "[unit]") {
  auto r = GuestMemory::reserve(0x1'0000'0000ull);
  REQUIRE(r.ok());
  GuestMemory mem = std::move(r.value());
  CHECK(mem.size() == 0x1'0000'0000ull);
  REQUIRE(mem.base() != nullptr);
  *mem.translate<uint32_t>(0x82000000) = 7;
  CHECK(*mem.translate<uint32_t>(0x82000000) == 7);
  CHECK(mem.translate<uint8_t>(0x82000000) == mem.base() + 0x82000000);
  GuestMemory moved = std::move(mem);
  CHECK(mem.base() == nullptr);  // NOLINT: moved-from is defined as empty
  CHECK(*moved.translate<uint32_t>(0x82000000) == 7);
}

TEST_CASE("GuestMemory::reserve rejects zero size", "[unit]") {
  CHECK_FALSE(GuestMemory::reserve(0).ok());
}

#include <sys/mman.h>

#include <csignal>
#include <string>

#include "runtime/fault.h"

using namespace lr::runtime;

TEST_CASE("guest_address_of maps host pointers into guest space", "[unit]") {
  alignas(16) static uint8_t fake[16];  // never dereferenced beyond the array
  const uint8_t* base = fake;
  CHECK(guest_address_of(base, 16, base + 4) == std::optional<uint32_t>(4));
  CHECK(guest_address_of(base, 16, base) == std::optional<uint32_t>(0));
  CHECK_FALSE(guest_address_of(base, 16, base + 16).has_value());
  CHECK_FALSE(guest_address_of(base, 16, nullptr).has_value());
}

TEST_CASE("describe_fault formats guest and host faults", "[unit]") {
  const uint8_t* base = reinterpret_cast<const uint8_t*>(uintptr_t{0x7000'0000'0000});
  const uint64_t size = 0x1'0000'0000ull;
  CHECK(describe_fault(SIGSEGV, base, size, base + 0x82001234) == "SIGSEGV at guest 0x82001234");
  CHECK(describe_fault(SIGBUS, base, size, base + 0x10) == "SIGBUS at guest 0x00000010");
  CHECK(describe_fault(SIGILL, base, size, base + 1) == "SIGILL at guest 0x00000001");
  auto out = describe_fault(SIGSEGV, base, size, reinterpret_cast<const void*>(uintptr_t{0x1234}));
  CHECK(out == "SIGSEGV at host 0x1234 (outside guest memory)");
  CHECK(describe_fault(99, base, size, nullptr).find("signal 99") == 0);
}

TEST_CASE("format_fault truncates safely", "[unit]") {
  const uint8_t* base = reinterpret_cast<const uint8_t*>(uintptr_t{0x7000'0000'0000});
  char buf[8];
  size_t n = format_fault(buf, sizeof buf, SIGSEGV, base, 0x1'0000'0000ull, base + 0x10);
  CHECK(n == 7);
  CHECK(std::string(buf) == "SIGSEGV");
  CHECK(format_fault(buf, 0, SIGSEGV, base, 1, base) == 0);
}

#include <sys/wait.h>
#include <unistd.h>

TEST_CASE("install_fault_handlers reports a guest fault (forked child)", "[unit]") {
  auto r = GuestMemory::reserve(0x1'0000'0000ull);
  REQUIRE(r.ok());
  int fds[2];
  REQUIRE(::pipe(fds) == 0);
  pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::dup2(fds[1], 2);
    ::close(fds[0]);
    if (!install_fault_handlers(r.value()).ok()) ::_exit(99);
    // Make the page inaccessible, then touch it: SIGSEGV at a known guest address.
    ::mprotect(r.value().base() + 0x82000000ull, 4096, PROT_NONE);
    *r.value().translate<volatile uint32_t>(0x82000010) = 1;
    ::_exit(0);  // not reached
  }
  ::close(fds[1]);
  char buf[256] = {};
  ssize_t n = ::read(fds[0], buf, sizeof buf - 1);
  ::close(fds[0]);
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 4);
  REQUIRE(n > 0);
  CHECK(std::string(buf).find("SIGSEGV at guest 0x82000010") == 0);
}

#include "runtime/func_table.h"

static void dummy_func() {}

TEST_CASE("install_function_table writes 8-byte slots per 4-byte instruction", "[unit]") {
  auto r = GuestMemory::reserve(0x1'0000'0000ull);
  REQUIRE(r.ok());
  const uint64_t off = 0x82000000ull + 0x32D0000ull;
  void* f = reinterpret_cast<void*>(&dummy_func);
  const FuncMapping maps[] = {{0x82000010, f}, {0x82000014, f}};
  REQUIRE(install_function_table(r.value(), off, 0x82000000, 0x1000, maps).ok());
  CHECK(*reinterpret_cast<void**>(r.value().base() + off + 0x20) == f);
  CHECK(*reinterpret_cast<void**>(r.value().base() + off + 0x28) == f);
  CHECK(*reinterpret_cast<void**>(r.value().base() + off + 0x30) == nullptr);
}

TEST_CASE("install_function_table rejects bad mappings", "[unit]") {
  auto r = GuestMemory::reserve(0x1'0000'0000ull);
  REQUIRE(r.ok());
  void* f = reinterpret_cast<void*>(&dummy_func);
  const uint64_t off = 0x852D0000ull;
  auto try_one = [&](uint32_t g) {
    const FuncMapping m[] = {{g, f}};
    return install_function_table(r.value(), off, 0x82200000, 0x1000, m);
  };
  auto below = try_one(0x821FFFFC);
  REQUIRE_FALSE(below.ok());
  CHECK(below.error().code == lr::ErrorCode::InvalidFormat);
  CHECK(below.error().message.find("0x821FFFFC") != std::string::npos);
  CHECK_FALSE(try_one(0x82201000).ok());  // == code_end
  auto mis = try_one(0x82200002);
  REQUIRE_FALSE(mis.ok());
  CHECK(mis.error().message.find("aligned") != std::string::npos);
  // slot beyond guest memory
  const FuncMapping m[] = {{0x82200000, f}};
  CHECK_FALSE(install_function_table(r.value(), 0x1'0000'0000ull - 4, 0x82200000, 0x1000, m).ok());
}

TEST_CASE("install_function_table with real game constants", "[unit]") {
  constexpr uint32_t kImageBase = 0x82000000, kImageSize = 0x32D0000, kCodeBase = 0x82200000, kCodeSize = 0x96428C;
  auto r = GuestMemory::reserve(0x1'0000'0000ull + kCodeSize * 2ull);
  REQUIRE(r.ok());
  void* a = reinterpret_cast<void*>(&dummy_func);
  void* b = reinterpret_cast<char*>(a) + 1;
  const uint64_t off = uint64_t{kImageBase} + kImageSize;
  const FuncMapping maps[] = {{kCodeBase, a}, {kCodeBase + kCodeSize - 4, b}};
  REQUIRE(install_function_table(r.value(), off, kCodeBase, kCodeSize, maps).ok());
  CHECK(*reinterpret_cast<void**>(r.value().base() + 0x852D0000ull) == a);
  CHECK(*reinterpret_cast<void**>(r.value().base() + off + (kCodeSize - 4) * 2ull) == b);
  const FuncMapping past[] = {{kCodeBase + kCodeSize, a}};
  CHECK_FALSE(install_function_table(r.value(), off, kCodeBase, kCodeSize, past).ok());
}

#include <pthread.h>

namespace {
volatile int g_limit = 1 << 30;  // opaque to the optimizer; effectively unbounded
[[gnu::noinline]] int recurse(volatile char* p, int depth) {
  volatile char pad[512];
  pad[0] = static_cast<char>(depth);
  if (p != nullptr) pad[1] = p[0];
  if (depth >= g_limit) return 0;
  return recurse(pad, depth + 1) + pad[0];
}
void* overflow_thread(void*) {
  if (!install_thread_altstack().ok() || !install_thread_altstack().ok()) ::_exit(99);  // idempotent
  recurse(nullptr, 0);
  return nullptr;
}
}  // namespace

TEST_CASE("fault in a second thread with its own altstack is reported (forked child)", "[unit]") {
  auto r = GuestMemory::reserve(0x1'0000'0000ull);
  REQUIRE(r.ok());
  int fds[2];
  REQUIRE(::pipe(fds) == 0);
  pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::dup2(fds[1], 2);
    ::close(fds[0]);
    if (!install_fault_handlers(r.value()).ok()) ::_exit(99);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_t t;
    if (pthread_create(&t, &attr, overflow_thread, nullptr) != 0) ::_exit(98);
    pthread_join(t, nullptr);
    ::_exit(0);
  }
  ::close(fds[1]);
  char buf[256] = {};
  ssize_t n = ::read(fds[0], buf, sizeof buf - 1);
  ::close(fds[0]);
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 4);
  REQUIRE(n > 0);
  CHECK(std::string(buf).find("SIGSEGV at host 0x") == 0);
}
