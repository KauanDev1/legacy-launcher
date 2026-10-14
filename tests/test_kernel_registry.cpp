// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <string>

#include "fake_generated/ppc_config.h"
#include "ppc_context.h"
#include "runtime/kernel/registry.h"

using namespace lr::kernel;

// The single real implementation in the test target (global scope, as the macro requires): proves
// LR_KERNEL_EXPORT defines the strong symbol and registers the name.
LR_KERNEL_EXPORT(LrTestExported) {
  (void)base;
  ctx.r3.u32 = 42;
}

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

TEST_CASE("register_implemented / is_implemented / LR_KERNEL_EXPORT", "[unit]") {
  CHECK_FALSE(is_implemented("Q"));
  register_implemented("Q");
  CHECK(is_implemented("Q"));
  CHECK(is_implemented("LrTestExported"));
  PPCContext ctx{};
  __imp__LrTestExported(ctx, nullptr);
  CHECK(ctx.r3.u32 == 42);
}
