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
