// SPDX-License-Identifier: MIT
// Memory imports of the kernel HLE (src/runtime/kernel/exports/memory.cpp): virtual memory (Nt*VirtualMemory),
// physical memory (Mm*), pool (ExAllocatePool*, Xam*), the null guard and --strict-memory.
#include <catch2/catch_test_macros.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "runtime/be.h"
#include "runtime/fault.h"
#include "runtime/guest_thread.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/status.h"
#include "runtime/memory_map.h"
#include "support/kernel_fixture.h"

using lr::runtime::be;
using lr::testing::KernelFixture;
using namespace lr::kernel;

namespace {

constexpr uint32_t kMemCommit = 0x1000, kMemReserve = 0x2000, kMemDecommit = 0x4000, kMemRelease = 0x8000;
constexpr uint32_t kMemFree = 0x10000, kMemPrivate = 0x20000, kMemTopDown = 0x100000;
constexpr uint32_t kMemNoZero = 0x800000, kMemLargePages = 0x20000000, kMem16MbPages = 0x80000000;
constexpr uint32_t kRW = 0x04, kRO = 0x02, kExecRW = 0x40;

uint32_t rd(KernelFixture& f, uint32_t a) { return *f.mem.translate<be<uint32_t>>(a); }
void wr(KernelFixture& f, uint32_t a, uint32_t v) { *f.mem.translate<be<uint32_t>>(a) = v; }
uint32_t status(uint64_t r3) { return static_cast<uint32_t>(r3); }

// Allocates through NtAllocateVirtualMemory; returns the status, fills base/size.
uint32_t nt_alloc(KernelFixture& f, uint32_t* base, uint32_t* size, uint32_t type, uint32_t protect) {
  const uint32_t bp = f.alloc(4), sp = f.alloc(4);
  wr(f, bp, *base);
  wr(f, sp, *size);
  const uint32_t st = status(f.call("NtAllocateVirtualMemory", bp, sp, type, protect, 0u));
  *base = rd(f, bp);
  *size = rd(f, sp);
  return st;
}

uint32_t nt_free(KernelFixture& f, uint32_t* base, uint32_t* size, uint32_t type) {
  const uint32_t bp = f.alloc(4), sp = f.alloc(4);
  wr(f, bp, *base);
  wr(f, sp, *size);
  const uint32_t st = status(f.call("NtFreeVirtualMemory", bp, sp, type, 0u));
  *base = rd(f, bp);
  *size = rd(f, sp);
  return st;
}

struct Mbi {
  uint32_t base, allocation_base, allocation_protect, size, state, protect, type;
};

Mbi query(KernelFixture& f, uint32_t addr, uint32_t* st) {
  const uint32_t p = f.alloc(28);
  *st = status(f.call("NtQueryVirtualMemory", addr, p));
  return {rd(f, p), rd(f, p + 4), rd(f, p + 8), rd(f, p + 12), rd(f, p + 16), rd(f, p + 20), rd(f, p + 24)};
}

bool all_zero(KernelFixture& f, uint32_t addr, uint32_t size) {
  const uint8_t* p = f.mem.translate<uint8_t>(addr);
  for (uint32_t i = 0; i < size; ++i)
    if (p[i] != 0) return false;
  return true;
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

TEST_CASE("memory imports are registered (whole-archive link of exports/memory.cpp)", "[unit]") {
  for (const char* n : {"NtAllocateVirtualMemory", "NtFreeVirtualMemory", "NtProtectVirtualMemory",
                        "NtQueryVirtualMemory", "MmAllocatePhysicalMemoryEx", "MmFreePhysicalMemory",
                        "MmGetPhysicalAddress", "MmQueryAddressProtect", "MmSetAddressProtect", "MmQueryAllocationSize",
                        "MmQueryStatistics", "ExAllocatePool", "ExAllocatePoolTypeWithTag", "ExFreePool", "XamAlloc",
                        "XamAllocEx", "XamFree"}) {
    INFO(n);
    CHECK(find_import(n).state == ImportState::Implemented);
  }
  CHECK(find_import("MmMapIoSpace").state == ImportState::Implemented);  // M3 (identity mapping)
}

TEST_CASE("NtAllocateVirtualMemory reserve+commit writes base and size back", "[unit]") {
  KernelFixture f;
  uint32_t base = 0, size = 0x1800;
  REQUIRE(nt_alloc(f, &base, &size, kMemReserve | kMemCommit, kRW) == STATUS_SUCCESS);
  CHECK(base >= lr::runtime::kHeap4kBase);
  CHECK(base < lr::runtime::kHeap4kEnd);
  CHECK(base % 0x1000 == 0);
  CHECK(size == 0x2000);
  // Never inside the fixture's own data/stack block.
  CHECK((base + size <= KernelFixture::kHeapBase || base >= KernelFixture::kReservedEnd));
  CHECK(all_zero(f, base, size));
  std::memset(f.mem.translate<uint8_t>(base), 0xAB, size);

  SECTION("commit zeroes recycled pages unless MEM_NOZERO") {
    uint32_t b = base, s = 0;
    REQUIRE(nt_free(f, &b, &s, kMemRelease) == STATUS_SUCCESS);
    uint32_t again = base, sz = 0x2000;
    REQUIRE(nt_alloc(f, &again, &sz, kMemReserve | kMemCommit, kRW) == STATUS_SUCCESS);
    CHECK(again == base);
    CHECK(all_zero(f, base, sz));
    std::memset(f.mem.translate<uint8_t>(base), 0xCD, sz);
    REQUIRE(nt_free(f, &b, &s, kMemRelease) == STATUS_SUCCESS);
    uint32_t nz = base, nzs = 0x2000;
    REQUIRE(nt_alloc(f, &nz, &nzs, kMemReserve | kMemCommit | kMemNoZero, kRW) == STATUS_SUCCESS);
    CHECK(*f.mem.translate<uint8_t>(base) == 0xCD);
  }
  SECTION("a fixed base that overlaps an allocation conflicts") {
    uint32_t b = base + 0x1000, s = 0x1000;
    CHECK(nt_alloc(f, &b, &s, kMemReserve, kRW) == STATUS_CONFLICTING_ADDRESSES);
  }
  SECTION("MEM_TOP_DOWN allocates from the top of the 4K heap") {
    uint32_t b = 0, s = 0x1000;
    REQUIRE(nt_alloc(f, &b, &s, kMemReserve | kMemTopDown, kRW) == STATUS_SUCCESS);
    CHECK(b == lr::runtime::kHeap4kEnd - 0x1000);
  }
}

TEST_CASE("NtAllocateVirtualMemory argument handling", "[unit]") {
  KernelFixture f;
  SECTION("null pointers and size 0 are invalid") {
    const uint32_t sp = f.alloc(4);
    wr(f, sp, 0x1000);
    CHECK(status(f.call("NtAllocateVirtualMemory", 0u, sp, kMemReserve, kRW, 0u)) == STATUS_INVALID_PARAMETER);
    uint32_t b = 0, s = 0;
    CHECK(nt_alloc(f, &b, &s, kMemReserve, kRW) == STATUS_INVALID_PARAMETER);
  }
  SECTION("neither MEM_COMMIT nor MEM_RESERVE is invalid") {
    uint32_t b = 0, s = 0x1000;
    CHECK(nt_alloc(f, &b, &s, kMemTopDown, kRW) == STATUS_INVALID_PARAMETER);
  }
  SECTION("a fixed base outside the virtual heaps is invalid") {
    uint32_t b = 0x82000000, s = 0x1000;
    CHECK(nt_alloc(f, &b, &s, kMemReserve, kRW) == STATUS_INVALID_PARAMETER);
  }
  SECTION("MEM_COMMIT alone on free pages reserves and commits") {
    uint32_t b = 0x10000000, s = 0x1000;
    REQUIRE(nt_alloc(f, &b, &s, kMemCommit, kRW) == STATUS_SUCCESS);
    CHECK(b == 0x10000000);
    uint32_t st = 0;
    const Mbi m = query(f, b, &st);
    CHECK(m.state == kMemCommit);
    CHECK(m.allocation_base == b);
  }
  SECTION("MEM_COMMIT on a reserved range commits it, unaligned base rounds down") {
    uint32_t b = 0, s = 0x4000;
    REQUIRE(nt_alloc(f, &b, &s, kMemReserve, kRW) == STATUS_SUCCESS);
    const uint32_t r = b;
    uint32_t cb = r + 0x1010, cs = 0x10;
    REQUIRE(nt_alloc(f, &cb, &cs, kMemCommit, kRO) == STATUS_SUCCESS);
    CHECK(cb == r + 0x1000);
    CHECK(cs == 0x1000);
    CHECK(status(f.call("MmQueryAddressProtect", r + 0x1000)) == kRO);
    CHECK(status(f.call("MmQueryAddressProtect", r)) == 0u);
  }
  SECTION("a negative size is negated") {
    uint32_t b = 0, s = static_cast<uint32_t>(-0x2000);
    REQUIRE(nt_alloc(f, &b, &s, kMemReserve, kRW) == STATUS_SUCCESS);
    CHECK(s == 0x2000);
  }
}

TEST_CASE("MEM_LARGE_PAGES goes to the 64K heap", "[unit]") {
  KernelFixture f;
  uint32_t base = 0, size = 0x1000;
  REQUIRE(nt_alloc(f, &base, &size, kMemReserve | kMemCommit | kMemLargePages, kRW) == STATUS_SUCCESS);
  CHECK(base >= lr::runtime::kHeap64kBase);
  CHECK(base < lr::runtime::kHeap64kEnd);
  CHECK(base % 0x10000 == 0);
  CHECK(size == 0x10000);
  CHECK(status(f.call("MmQueryAllocationSize", base + 0x20)) == 0x10000u);
}

TEST_CASE("NtFreeVirtualMemory MEM_RELEASE", "[unit]") {
  KernelFixture f;
  uint32_t base = 0, size = 0x3000;
  REQUIRE(nt_alloc(f, &base, &size, kMemReserve | kMemCommit, kRW) == STATUS_SUCCESS);

  SECTION("release writes back the whole allocation and frees it") {
    uint32_t b = base, s = 0;
    REQUIRE(nt_free(f, &b, &s, kMemRelease) == STATUS_SUCCESS);
    CHECK(b == base);
    CHECK(s == 0x3000);
    uint32_t st = 0;
    CHECK(query(f, base, &st).state == kMemFree);
    CHECK(st == STATUS_SUCCESS);
    uint32_t b2 = base, s2 = 0;
    CHECK(nt_free(f, &b2, &s2, kMemRelease) == STATUS_UNSUCCESSFUL);
    CHECK(status(f.call("MmQueryAllocationSize", base)) == 0u);
  }
  SECTION("decommit keeps the reservation") {
    uint32_t b = base + 0x1000, s = 0x1000;
    REQUIRE(nt_free(f, &b, &s, kMemDecommit) == STATUS_SUCCESS);
    CHECK(b == base + 0x1000);
    CHECK(s == 0x1000);
    uint32_t st = 0;
    const Mbi m = query(f, base + 0x1000, &st);
    CHECK(m.state == kMemReserve);
    CHECK(m.allocation_base == base);
  }
  SECTION("base 0 is not allocated") {
    uint32_t b = 0, s = 0;
    CHECK(nt_free(f, &b, &s, kMemRelease) == STATUS_MEMORY_NOT_ALLOCATED);
  }
}

TEST_CASE("NtQueryVirtualMemory fills MEMORY_BASIC_INFORMATION", "[unit]") {
  KernelFixture f;
  uint32_t base = 0, size = 0x3000;
  REQUIRE(nt_alloc(f, &base, &size, kMemReserve, kRW) == STATUS_SUCCESS);
  uint32_t cb = base, cs = 0x1000;
  REQUIRE(nt_alloc(f, &cb, &cs, kMemCommit, kRW) == STATUS_SUCCESS);

  uint32_t st = 1;
  const Mbi c = query(f, base + 0x10, &st);
  CHECK(st == STATUS_SUCCESS);
  CHECK(c.base == base);
  CHECK(c.allocation_base == base);
  CHECK(c.allocation_protect == kRW);
  CHECK(c.size == 0x1000);
  CHECK(c.state == kMemCommit);
  CHECK(c.protect == kRW);
  CHECK(c.type == kMemPrivate);

  const Mbi r = query(f, base + 0x1000, &st);
  CHECK(r.base == base + 0x1000);
  CHECK(r.size == 0x2000);
  CHECK(r.state == kMemReserve);
  CHECK(r.protect == 0u);

  CHECK(status(f.call("NtQueryVirtualMemory", 0x82000000u, f.alloc(28))) == STATUS_INVALID_PARAMETER);
}

TEST_CASE("NtProtectVirtualMemory and MmSetAddressProtect", "[unit]") {
  KernelFixture f;
  uint32_t base = 0, size = 0x2000;
  REQUIRE(nt_alloc(f, &base, &size, kMemReserve | kMemCommit, kRW) == STATUS_SUCCESS);
  const uint32_t bp = f.alloc(4), sp = f.alloc(4), op = f.alloc(4);
  wr(f, bp, base + 4);
  wr(f, sp, 0x10);
  REQUIRE(status(f.call("NtProtectVirtualMemory", bp, sp, kRO, op, 0u)) == STATUS_SUCCESS);
  CHECK(rd(f, bp) == base);
  CHECK(rd(f, sp) == 0x1000);
  CHECK(rd(f, op) == kRW);
  CHECK(status(f.call("MmQueryAddressProtect", base)) == kRO);
  CHECK(status(f.call("MmQueryAddressProtect", base + 0x1000)) == kRW);
  CHECK(status(f.call("NtProtectVirtualMemory", bp, sp, kExecRW, op, 0u)) == STATUS_ACCESS_DENIED);

  f.call("MmSetAddressProtect", base, 0x2000u, kRW);
  CHECK(status(f.call("MmQueryAddressProtect", base)) == kRW);
  CHECK(status(f.call("MmQueryAddressProtect", 0x00100000u)) == 0u);  // free page
}

TEST_CASE("MmAllocatePhysicalMemoryEx returns a C0 view address for 16 MB pages", "[unit]") {
  KernelFixture f;
  const uint32_t a = status(f.call("MmAllocatePhysicalMemoryEx", 0u, 0x100000u, kMem16MbPages | kRW, 0u,
                                   0xFFFFFFFFu, 0u));
  REQUIRE(a != 0u);
  CHECK(a >= lr::runtime::kPhysicalViewC0);
  CHECK(a < lr::runtime::kPhysicalViewE0);
  CHECK(a % 0x1000000 == 0);
  CHECK(status(f.call("MmQueryAllocationSize", a + 0x100)) == 0x1000000u);
  CHECK(status(f.call("MmGetPhysicalAddress", a + 0x10)) == ((a + 0x10) & 0x1FFFFFFFu));
  CHECK(status(f.call("MmQueryAddressProtect", a)) == kRW);
  CHECK(all_zero(f, a, 0x1000));
  *f.mem.translate<uint8_t>(a) = 1;
  f.call("MmFreePhysicalMemory", 0u, a);
  CHECK(status(f.call("MmQueryAllocationSize", a)) == 0u);

  SECTION("E0 view adds 0x1000 to the physical address") {
    const uint32_t e = status(f.call("MmAllocatePhysicalMemoryEx", 0u, 0x1000u, kRW, 0u, 0xFFFFFFFFu, 0u));
    REQUIRE(e >= lr::runtime::kPhysicalViewE0);
    CHECK(status(f.call("MmGetPhysicalAddress", e)) == (e & 0x1FFFFFFFu) + 0x1000u);
  }
  SECTION("no readable protection fails with 0") {
    CHECK(status(f.call("MmAllocatePhysicalMemoryEx", 0u, 0x1000u, 0x01u, 0u, 0xFFFFFFFFu, 0u)) == 0u);
  }
}

TEST_CASE("MmQueryStatistics checks the size field", "[unit]") {
  KernelFixture f;
  const uint32_t p = f.alloc(104);
  wr(f, p, 100);
  CHECK(status(f.call("MmQueryStatistics", p)) == STATUS_BUFFER_TOO_SMALL);
  wr(f, p, 104);
  REQUIRE(status(f.call("MmQueryStatistics", p)) == STATUS_SUCCESS);
  CHECK(rd(f, p + 4) == 0x20000u);       // total_physical_pages
  CHECK(rd(f, p + 0x64) == 0x1FFFFu);    // highest_physical_page
  CHECK(rd(f, p + 0x0C) <= 0x20000u);    // title.available_pages
  CHECK(status(f.call("MmQueryStatistics", 0u)) == STATUS_INVALID_PARAMETER);
}

TEST_CASE("ExAllocatePoolTypeWithTag zeroed", "[unit]") {
  KernelFixture f;
  const uint32_t a = status(f.call("ExAllocatePoolTypeWithTag", 0x20u, 0x4C524543u, 0u));
  REQUIRE(a != 0u);
  CHECK(a >= lr::runtime::kHeap4kBase);
  CHECK(a < lr::runtime::kHeap4kEnd);
  CHECK(all_zero(f, a, 0x1000));
  std::memset(f.mem.translate<uint8_t>(a), 0xEE, 0x1000);
  f.call("ExFreePool", a);
  CHECK(status(f.call("MmQueryAllocationSize", a)) == 0u);
  const uint32_t b = status(f.call("ExAllocatePool", 0x1000u));
  REQUIRE(b != 0u);
  CHECK(all_zero(f, b, 0x1000));
  f.call("ExFreePool", b);

  SECTION("XamAlloc writes the block to the out pointer, XamFree frees it") {
    const uint32_t out = f.alloc(4);
    REQUIRE(status(f.call("XamAlloc", 0u, 0x40u, out)) == 0u);
    const uint32_t x = rd(f, out);
    REQUIRE(x != 0u);
    CHECK(all_zero(f, x, 0x40));
    CHECK(status(f.call("XamFree", x)) == 0u);
    CHECK(status(f.call("MmQueryAllocationSize", x)) == 0u);
    REQUIRE(status(f.call("XamAllocEx", 0u, 0u, 0x40u, out)) == 0u);
    CHECK(rd(f, out) != 0u);
  }
}

TEST_CASE("null guard faults", "[unit]") {
  KernelFixture f;
  const ChildResult r = run_child([&] {
    if (!lr::runtime::install_fault_handlers(f.mem)) ::_exit(98);
    *f.mem.translate<volatile uint32_t>(0) = 1;
  });
  CHECK(r.code == lr::runtime::kExitFault);
  CHECK(r.err.find("SIGSEGV at guest 0x00000000") != std::string::npos);
}

TEST_CASE("strict memory leaves uncommitted pages inaccessible", "[unit]") {
  KernelConfig cfg;
  cfg.strict_memory = true;
  KernelFixture f(cfg);
  uint32_t base = 0, size = 0x2000;
  REQUIRE(nt_alloc(f, &base, &size, kMemReserve, kRW) == STATUS_SUCCESS);
  uint32_t cb = base, cs = 0x1000;
  REQUIRE(nt_alloc(f, &cb, &cs, kMemCommit, kRW) == STATUS_SUCCESS);
  *f.mem.translate<uint32_t>(base) = 7;  // committed: accessible
  const ChildResult r = run_child([&] {
    if (!lr::runtime::install_fault_handlers(f.mem)) ::_exit(98);
    *f.mem.translate<volatile uint32_t>(base + 0x1000) = 1;  // reserved only
  });
  CHECK(r.code == lr::runtime::kExitFault);
  char want[64];
  std::snprintf(want, sizeof want, "SIGSEGV at guest 0x%08x", base + 0x1000);
  CHECK(r.err.find(want) != std::string::npos);
}
