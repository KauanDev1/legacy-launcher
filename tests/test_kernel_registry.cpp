// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include <string>

#include "runtime/kernel/registry.h"

using namespace lr::kernel;

// Registered at global scope, as the macros require.
namespace lr::kernel::exports {
uint32_t LrTestImplemented(uint32_t a, uint32_t b) { return a + b; }
}  // namespace lr::kernel::exports
LR_KERNEL(LrTestImplemented);
LR_KERNEL_STUB(LrTestStub, -1);  // exercises the 32-bit sign extension of the stub's return value

TEST_CASE("unimplemented_import throws with the call trail", "[unit]") {
  trace("X");
  trace("Y");
  try {
    unimplemented_import("Z", 0x82001000);
    FAIL("expected throw");
  } catch (const UnimplementedImport& e) {
    CHECK(e.name == "Z");
    CHECK(e.caller == 0x82001000u);
    REQUIRE(e.trail.size() >= 2);
    CHECK(e.trail[e.trail.size() - 2] == "X");
    CHECK(e.trail.back() == "Y");
    const std::string rep = format_report(e);
    CHECK(rep.find("Unimplemented import: Z") != std::string::npos);
    CHECK(rep.find("Called from: 0x82001000") != std::string::npos);
    CHECK(rep.find("X -> Y") != std::string::npos);
  }
}

TEST_CASE("trace keeps only the last 32 entries, oldest first", "[unit]") {
  for (int i = 0; i < 100; ++i) trace(("call" + std::to_string(i)).c_str());
  try {
    unimplemented_import("W", 0);
    FAIL("expected throw");
  } catch (const UnimplementedImport& e) {
    REQUIRE(e.trail.size() == 32);
    CHECK(e.trail.front() == "call68");
    CHECK(e.trail.back() == "call99");
  }
}

TEST_CASE("unregistered import is missing", "[unit]") {
  const ImportEntry e = find_import("LrTestNeverRegistered");
  CHECK(e.state == ImportState::Missing);
  CHECK(e.fn == nullptr);
}

TEST_CASE("register_import records state and function", "[unit]") {
  register_import("LrTestDirect", ImportState::Implemented, [](GuestCall& c) { c.ret_gpr = 7; });
  const ImportEntry e = find_import("LrTestDirect");
  CHECK(e.state == ImportState::Implemented);
  REQUIRE(e.fn != nullptr);
  GuestCall c;
  e.fn(c);
  CHECK(c.ret_gpr == 7u);
}

TEST_CASE("LR_KERNEL registers a marshalled implementation", "[unit]") {
  const ImportEntry e = find_import("LrTestImplemented");
  CHECK(e.state == ImportState::Implemented);
  REQUIRE(e.fn != nullptr);
  GuestCall c;
  c.gpr[0] = 40;
  c.gpr[1] = 2;
  e.fn(c);
  CHECK(c.ret_gpr == 42u);
}

TEST_CASE("registered stub is reported as stub", "[unit]") {
  const ImportEntry e = find_import("LrTestStub");
  CHECK(e.state == ImportState::Stub);
  REQUIRE(e.fn != nullptr);
  GuestCall c;
  c.gpr[0] = 5;
  e.fn(c);
  CHECK(c.ret_gpr == 0xFFFF'FFFF'FFFF'FFFFull);
  CHECK_FALSE(c.has_fpr_return);
}
