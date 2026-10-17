// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

#include "cases.h"
#include "ppc_recomp_shared.h"
#include "runtime/guest_memory.h"

namespace {

using lr::test::recomp_cases;

// Generated code addresses guest memory as base + 32-bit guest address, so the test needs the full 4 GiB
// reservation (lazily committed) instead of a std::vector.
lr::runtime::GuestMemory& guest_memory() {
  static auto mem = [] {
    auto r = lr::runtime::GuestMemory::reserve(0x1'0000'0000ull);
    REQUIRE(r.ok());
    return std::move(r.value());
  }();
  return mem;
}

PPCFunc* find_function(uint32_t address) {
  for (const PPCFuncMapping* m = PPCFuncMappings; m->host; ++m)
    if (m->guest == address) return m->host;
  return nullptr;
}

// Runs the recompiled function at `address` with `in` as the initial context and returns the final context.
PPCContext run_case(uint32_t address, PPCContext in, lr::runtime::GuestMemory& mem) {
  PPCFunc* fn = find_function(address);
  REQUIRE(fn != nullptr);
  in.fpscr.loadFromHost();  // valid FP environment, as XenonRecomp's own harness does
  fn(in, mem.base());
  return in;
}

uint32_t case_address(const char* name) {
  for (const auto& c : recomp_cases())
    if (std::strcmp(c.name, name) == 0) return c.address;
  FAIL("unknown case " << name);
  return 0;
}

}  // namespace

TEST_CASE("addi baseline", "[unit][recomp]") {
  PPCContext in{};
  in.r3.u64 = 7;
  const PPCContext out = run_case(case_address("addi baseline"), in, guest_memory());
  CHECK(out.r3.u64 == 12);
}

namespace {

// Branch fixtures return r3 = 1 on fall-through and r3 = 2 when the branch is taken.
uint64_t run_branch(const char* name, const PPCContext& in) {
  return run_case(case_address(name), in, guest_memory()).r3.u64;
}

}  // namespace

TEST_CASE("bdzf takes branch when ctr hits zero and bit clear", "[unit][recomp]") {
  PPCContext in{};
  in.ctr.u64 = 1;  // decrements to 0
  CHECK(run_branch("bdzf cr0.lt", in) == 2);
  CHECK(run_branch("bdzf cr0.gt", in) == 2);
  CHECK(run_branch("bdzf cr0.eq", in) == 2);
  CHECK(run_branch("bdzf cr0.so", in) == 2);
  CHECK(run_branch("bdzf cr2.gt", in) == 2);
}

TEST_CASE("bdzf falls through when bit set", "[unit][recomp]") {
  PPCContext in{};
  in.ctr.u64 = 1;
  PPCContext a = in;
  a.cr0.lt = 1;
  CHECK(run_branch("bdzf cr0.lt", a) == 1);
  PPCContext b = in;
  b.cr0.gt = 1;
  CHECK(run_branch("bdzf cr0.gt", b) == 1);
  PPCContext c = in;
  c.cr0.eq = 1;
  CHECK(run_branch("bdzf cr0.eq", c) == 1);
  PPCContext d = in;
  d.cr0.so = 1;
  CHECK(run_branch("bdzf cr0.so", d) == 1);
  PPCContext e = in;
  e.cr2.gt = 1;
  CHECK(run_branch("bdzf cr2.gt", e) == 1);
}

TEST_CASE("bdzf falls through when ctr is not zero after decrement", "[unit][recomp]") {
  PPCContext in{};
  in.ctr.u64 = 2;
  CHECK(run_branch("bdzf cr0.lt", in) == 1);
}

TEST_CASE("bdnzf uses the requested bit (gt)", "[unit][recomp]") {
  PPCContext in{};
  in.ctr.u64 = 2;  // stays non-zero
  CHECK(run_branch("bdnzf cr0.gt", in) == 2);
  PPCContext gt = in;
  gt.cr0.gt = 1;  // gt set, eq clear: an eq-only shortcut would wrongly branch
  CHECK(run_branch("bdnzf cr0.gt", gt) == 1);
  PPCContext eq = in;
  eq.cr0.eq = 1;  // eq set but gt clear: must still branch
  CHECK(run_branch("bdnzf cr0.gt", eq) == 2);
  CHECK(run_branch("bdnzf cr0.eq", eq) == 1);
  PPCContext zero{};
  zero.ctr.u64 = 1;  // decrements to 0: no branch
  CHECK(run_branch("bdnzf cr0.gt", zero) == 1);
}

TEST_CASE("bso branches on the summary-overflow bit", "[unit][recomp]") {
  PPCContext in{};
  CHECK(run_branch("bso cr0", in) == 1);
  PPCContext so = in;
  so.cr0.so = 1;
  CHECK(run_branch("bso cr0", so) == 2);
  CHECK(run_branch("bso cr1", so) == 1);
  PPCContext so1 = in;
  so1.cr1.so = 1;
  CHECK(run_branch("bso cr1", so1) == 2);
}

TEST_CASE("bso after fcmpu with NaN branches", "[unit][recomp]") {
  PPCContext in{};
  in.f1.f64 = std::numeric_limits<double>::quiet_NaN();
  in.f2.f64 = 1.0;
  CHECK(run_branch("fcmpu then bso", in) == 2);
}

TEST_CASE("bso after fcmpu of 1.0,2.0 falls through", "[unit][recomp]") {
  PPCContext in{};
  in.f1.f64 = 1.0;
  in.f2.f64 = 2.0;
  CHECK(run_branch("fcmpu then bso", in) == 1);
}

namespace {

uint8_t* guest_ptr(uint32_t addr) { return guest_memory().base() + addr; }

void zero_guest(uint32_t addr, size_t n) { std::memset(guest_ptr(addr), 0, n); }

}  // namespace

TEST_CASE("lfsu loads a float and updates rA", "[unit][recomp]") {
  zero_guest(0x10000, 0x20);
  const uint8_t be[4] = {0x3F, 0xC0, 0x00, 0x00};  // 1.5f
  std::memcpy(guest_ptr(0x10008), be, 4);
  PPCContext in{};
  in.r3.u64 = 0x10000;
  const PPCContext out = run_case(case_address("lfsu"), in, guest_memory());
  CHECK(out.f1.f64 == 1.5);
  CHECK(out.r3.u32 == 0x10008);
}

TEST_CASE("lfsux loads a float and updates rA", "[unit][recomp]") {
  zero_guest(0x10000, 0x20);
  const uint8_t be[4] = {0xBF, 0xC0, 0x00, 0x00};  // -1.5f
  std::memcpy(guest_ptr(0x10010), be, 4);
  PPCContext in{};
  in.r3.u64 = 0x10000;
  in.r4.u64 = 0x10;
  const PPCContext out = run_case(case_address("lfsux"), in, guest_memory());
  CHECK(out.f1.f64 == -1.5);
  CHECK(out.r3.u32 == 0x10010);
  CHECK(out.r4.u32 == 0x10);
}

TEST_CASE("stfsu stores a float and updates rA", "[unit][recomp]") {
  zero_guest(0xFF0, 0x20);
  PPCContext in{};
  in.r1.u64 = 0x1000;
  in.f1.f64 = 1.5;
  const PPCContext out = run_case(case_address("stfsu"), in, guest_memory());
  CHECK(guest_ptr(0xFF8)[0] == 0x3F);
  CHECK(guest_ptr(0xFF8)[1] == 0xC0);
  CHECK(guest_ptr(0xFF8)[2] == 0x00);
  CHECK(guest_ptr(0xFF8)[3] == 0x00);
  CHECK(guest_ptr(0xFFC)[0] == 0x00);  // nothing written past the word
  CHECK(out.r1.u32 == 0xFF8);
}

TEST_CASE("stfsux stores a float and updates rA", "[unit][recomp]") {
  zero_guest(0x10000, 0x20);
  PPCContext in{};
  in.r3.u64 = 0x10000;
  in.r4.u64 = 0x8;
  in.f1.f64 = -1.5;
  const PPCContext out = run_case(case_address("stfsux"), in, guest_memory());
  CHECK(guest_ptr(0x10008)[0] == 0xBF);
  CHECK(guest_ptr(0x10008)[1] == 0xC0);
  CHECK(guest_ptr(0x10008)[2] == 0x00);
  CHECK(guest_ptr(0x10008)[3] == 0x00);
  CHECK(out.r3.u32 == 0x10008);
  CHECK(out.r4.u32 == 0x8);
}

TEST_CASE("lhzu zero-extends a halfword and updates rA", "[unit][recomp]") {
  zero_guest(0x10000, 0x20);
  const uint8_t be[2] = {0xFF, 0x80};
  std::memcpy(guest_ptr(0x10002), be, 2);
  PPCContext in{};
  in.r5.u64 = 0x10000;
  in.r4.u64 = 0xFFFFFFFFFFFFFFFFull;
  const PPCContext out = run_case(case_address("lhzu"), in, guest_memory());
  CHECK(out.r4.u64 == 0xFF80);
  CHECK(out.r5.u32 == 0x10002);
}

TEST_CASE("sthu stores the low halfword and updates rA", "[unit][recomp]") {
  zero_guest(0x10000, 0x20);
  PPCContext in{};
  in.r5.u64 = 0x10004;
  in.r4.u64 = 0x1234ABCD;
  const PPCContext out = run_case(case_address("sthu"), in, guest_memory());
  CHECK(guest_ptr(0x10002)[0] == 0xAB);
  CHECK(guest_ptr(0x10002)[1] == 0xCD);
  CHECK(guest_ptr(0x10004)[0] == 0x00);  // only two bytes written
  CHECK(out.r5.u32 == 0x10002);
}
