// SPDX-License-Identifier: MIT
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "runtime/fault.h"
#include "runtime/guest_memory.h"
#include "runtime/write_watch.h"

using namespace lr::runtime;

TEST_CASE("parse_watch_spec reads <addr>:<len> in hex or decimal", "[unit]") {
  auto a = parse_watch_spec("0x1D1E8000:0x8000");
  REQUIRE(a.ok());
  CHECK(a.value().addr == 0x1D1E8000u);
  CHECK(a.value().len == 0x8000u);
  auto b = parse_watch_spec("4096:16");
  REQUIRE(b.ok());
  CHECK(b.value().addr == 4096u);
  CHECK(b.value().len == 16u);
  CHECK(parse_watch_spec("0xFFFFFFF0:0x10").ok());  // ends exactly at 2^32
  for (const char* bad : {"", "0x10", ":4", "0x10:", "0x10:0", "0xFFFFFFFF:2", "zz:1", "0x10:-1", "0x:4", "1:2:3"})
    CHECK_FALSE(parse_watch_spec(bad).ok());
}

TEST_CASE("HostCodeMap maps host code to the guest function that contains it", "[unit]") {
  const HostCodeMap map({{0x3000, 0x82000200}, {0x1000, 0x82000000}, {0x2000, 0x82000100}});  // any order
  CHECK_FALSE(map.guest_function(0x0FFF).has_value());
  CHECK(map.guest_function(0x1000) == 0x82000000u);
  CHECK(map.guest_function(0x1FFF) == 0x82000000u);
  CHECK(map.guest_function(0x2500) == 0x82000100u);
  CHECK(map.guest_function(0x3FFF) == 0x82000200u);
  CHECK_FALSE(map.guest_function(0x4000).has_value());  // past the last function's short tail: host code
  CHECK_FALSE(HostCodeMap().guest_function(0x1000).has_value());
}

namespace {

// The "recompiled function" of the forked test: its host address is mapped to guest 0x82001000. One 4-byte store
// (memcpy may store a small block twice, overlapping, and the watch counts every faulting store).
[[gnu::noinline]] void guest_store(uint8_t* p, uint32_t v) { *reinterpret_cast<volatile uint32_t*>(p) = v; }

// Runs `child` in a forked process with stderr on a pipe; returns (exit status, everything it wrote).
template <class F>
std::pair<int, std::string> run_forked(F child) {
  int fds[2];
  REQUIRE(::pipe(fds) == 0);
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::dup2(fds[1], 2);
    ::close(fds[0]);
    ::_exit(child());
  }
  ::close(fds[1]);
  std::string out;
  char buf[512];
  for (ssize_t n; (n = ::read(fds[0], buf, sizeof buf)) > 0;) out.append(buf, static_cast<size_t>(n));
  ::close(fds[0]);
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  REQUIRE(WIFEXITED(status));
  return {WEXITSTATUS(status), out};
}

}  // namespace

TEST_CASE("the write watch logs writes into a watched range and lets them through (forked child)", "[unit]") {
  auto r = GuestMemory::reserve(0x100000);
  REQUIRE(r.ok());
  GuestMemory& mem = r.value();
  const auto [status, out] = run_forked([&]() -> int {
    if (!install_fault_handlers(mem).ok()) return 90;
    const WatchRange ranges[] = {{0x10010, 0x20}, {0x12FF0, 0x20}};  // the second spans pages 0x12000 and 0x13000
    HostCodeMap code({{reinterpret_cast<uintptr_t>(&guest_store), 0x82001000}});
    if (!install_write_watch(mem, ranges, std::move(code)).ok()) return 91;
    uint8_t* const b = mem.base();
    guest_store(b + 0x10014, 0x11223344);  // inside range 0
    guest_store(b + 0x10014, 0x55667788);  // again: counted, same call site (not logged twice)
    guest_store(b + 0x10100, 0xAABBCCDD);  // same page, outside the range: let through, not counted
    guest_store(b + 0x30000, 0x01020304);  // unwatched page
    uint8_t line[32];
    for (uint8_t i = 0; i < 32; ++i) line[i] = static_cast<uint8_t>(i + 1);
    std::memcpy(b + 0x12FF0, line, sizeof line);  // one copy across two watched pages
    uint32_t v = 0;
    std::memcpy(&v, b + 0x10014, 4);
    if (v != 0x55667788) return 92;
    std::memcpy(&v, b + 0x10100, 4);
    if (v != 0xAABBCCDD) return 93;
    std::memcpy(&v, b + 0x30000, 4);
    if (v != 0x01020304) return 94;
    if (std::memcmp(b + 0x12FF0, line, sizeof line) != 0) return 95;
    if (write_watch_count(0) != 2) {
      std::fprintf(stderr, "count0=%llu\n", static_cast<unsigned long long>(write_watch_count(0)));
      return 96;
    }
    if (write_watch_count(1) == 0) return 97;
    if (!write_watch_overlaps(0x12000, 1) || write_watch_overlaps(0x14000, 0x1000)) return 98;
    // Host-side zeroing never faults (madvise), so GuestMemory::zero reports it; the page stays watched.
    if (!mem.zero(0x10000, 0x1000).ok()) return 99;
    if (write_watch_count(0) != 3) return 100;
    guest_store(b + 0x10020, 1);
    if (write_watch_count(0) != 4) return 101;
    // Any other fault is still a real one: exit 4 from the fault handler.
    ::mprotect(b + 0x40000, 0x1000, PROT_NONE);
    guest_store(b + 0x40000, 1);
    return 0;  // not reached
  });
  INFO(out);
  CHECK(status == 4);
  CHECK(out.find("[watch] armed range 0 0x00010010+0x20\n") != std::string::npos);
  CHECK(out.find("[watch] armed range 1 0x00012FF0+0x20\n") != std::string::npos);
  CHECK(out.find("[watch] write 0x00010014 range 0 n=1 tid=") != std::string::npos);
  CHECK(out.find(" fn 0x82001000 pc=0x") != std::string::npos);
  CHECK(out.find("n=2") == std::string::npos);  // the second write of the same site is only counted
  CHECK(out.find("[watch] write 0x00010100") == std::string::npos);
  CHECK(out.find("[watch] host zero 0x00010000+0x1000 range 0 n=3") != std::string::npos);
  CHECK(out.find("range 1 n=1") != std::string::npos);
  CHECK(out.find("SIGSEGV at guest 0x00040000") != std::string::npos);
}
