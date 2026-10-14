// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include "common/error.h"
#include "common/result.h"

using namespace lr;

namespace {
Result<void> failing() { return Error{ErrorCode::IoError, "disk", "retry"}; }
Result<void> succeeding() { return {}; }

Result<int> uses_try(bool fail, bool& reached) {
  LR_TRY(fail ? failing() : succeeding());
  reached = true;
  return 7;
}

Result<int> uses_try_assign(Result<int> in) {
  LR_TRY_ASSIGN(v, in);
  return v + 1;
}
}  // namespace

TEST_CASE("Result holds a value", "[unit]") {
  Result<int> ok = 5;
  REQUIRE(ok.ok());
  REQUIRE(static_cast<bool>(ok));
  REQUIRE(ok.value() == 5);
}

TEST_CASE("Result holds an error", "[unit]") {
  Result<int> bad = Error{ErrorCode::NotFound, "x", "y"};
  REQUIRE_FALSE(bad);
  REQUIRE(bad.error().code == ErrorCode::NotFound);
  REQUIRE(bad.error().message == "x");
  REQUIRE(bad.error().hint == "y");
}

TEST_CASE("LR_TRY propagates failure and continues on success", "[unit]") {
  bool reached = false;
  auto r = uses_try(true, reached);
  REQUIRE_FALSE(r);
  REQUIRE_FALSE(reached);
  REQUIRE(r.error().code == ErrorCode::IoError);
  REQUIRE(r.error().message == "disk");
  REQUIRE(r.error().hint == "retry");

  auto s = uses_try(false, reached);
  REQUIRE(s.ok());
  REQUIRE(reached);
  REQUIRE(s.value() == 7);
}

TEST_CASE("LR_TRY_ASSIGN unwraps or propagates", "[unit]") {
  REQUIRE(uses_try_assign(41).value() == 42);
  auto bad = uses_try_assign(Error{ErrorCode::Cancelled, "c", ""});
  REQUIRE_FALSE(bad);
  REQUIRE(bad.error().code == ErrorCode::Cancelled);
}

TEST_CASE("to_string names every code", "[unit]") {
  REQUIRE(to_string(ErrorCode::UnsupportedVersion) == "UnsupportedVersion");
  REQUIRE(to_string(ErrorCode::UnimplementedImport) == "UnimplementedImport");
  REQUIRE(to_string(ErrorCode::Internal) == "Internal");
}
