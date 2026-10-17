// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
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
