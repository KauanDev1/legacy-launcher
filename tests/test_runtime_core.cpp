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
    install_fault_handlers(r.value());
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
