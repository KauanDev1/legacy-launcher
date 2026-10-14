// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

#include "launcher/cli/args.h"

using namespace lr;
using namespace lr::cli;
using lr::pipeline::StageId;

namespace {
Result<CliArgs> parse(std::vector<std::string_view> v) { return parse_args(v); }
}  // namespace

TEST_CASE("install parses every flag and keeps the path intact", "[unit]") {
  auto r = parse({"install", "--iso", "a b (c).iso", "--force", "recompile", "--jobs", "4"});
  REQUIRE(r);
  CHECK(r.value().command == "install");
  REQUIRE(r.value().iso);
  CHECK(r.value().iso->string() == "a b (c).iso");
  CHECK(r.value().force == std::set<StageId>{StageId::Recompile});
  CHECK(r.value().jobs == 4);
  CHECK_FALSE(r.value().root);
}

TEST_CASE("install without --iso is a usage error", "[unit]") {
  auto r = parse({"install"});
  REQUIRE_FALSE(r);
  CHECK(r.error().message.find("--iso") != std::string::npos);
}

TEST_CASE("unknown --force stage lists the valid names", "[unit]") {
  auto r = parse({"install", "--iso", "x", "--force", "bogus"});
  REQUIRE_FALSE(r);
  CHECK(r.error().code == ErrorCode::InvalidFormat);
  CHECK((r.error().message + r.error().hint).find("extract") != std::string::npos);
}

TEST_CASE("force accepts a comma list and repeats", "[unit]") {
  auto r = parse({"install", "--iso", "x", "--force", "patch,analyse", "--force", "build"});
  REQUIRE(r);
  CHECK(r.value().force == std::set<StageId>{StageId::Patch, StageId::Analyse, StageId::Build});
}

TEST_CASE("run forwards everything after -- to the game", "[unit]") {
  auto r = parse({"run", "--", "--headless"});
  REQUIRE(r);
  CHECK(r.value().command == "run");
  CHECK(r.value().passthrough == std::vector<std::string>{"--headless"});
}

TEST_CASE("build does not need --iso and forces recompile", "[unit]") {
  auto r = parse({"build"});
  REQUIRE(r);
  CHECK(r.value().command == "build");
  CHECK_FALSE(r.value().iso);
  CHECK(r.value().force.count(StageId::Recompile) == 1);
}

TEST_CASE("doctor and inspect parse; unknown command, flags and bad jobs fail", "[unit]") {
  CHECK(parse({"doctor", "--root", "/tmp/r"}));
  auto ins = parse({"inspect", "--iso", "g.iso"});
  REQUIRE(ins);
  CHECK(ins.value().passthrough == std::vector<std::string>{"--iso", "g.iso"});
  CHECK_FALSE(parse({"frobnicate"}));
  CHECK_FALSE(parse({"install", "--iso", "x", "--nope"}));
  CHECK_FALSE(parse({"install", "--iso", "x", "--jobs", "abc"}));
  CHECK_FALSE(parse({"install", "--iso"}));
  CHECK_FALSE(parse({}));
}

TEST_CASE("data root precedence is flag, env, XDG default", "[unit]") {
  using std::filesystem::path;
  CHECK(resolve_root(path("/flag"), "/env", "/xdg", "/home/u") == path("/flag"));
  CHECK(resolve_root(std::nullopt, "/env", "/xdg", "/home/u") == path("/env"));
  CHECK(resolve_root(std::nullopt, "", "/xdg", "/home/u") == path("/xdg/legacyrecomp"));
  CHECK(resolve_root(std::nullopt, nullptr, nullptr, "/home/u") == path("/home/u/.local/share/legacyrecomp"));
}

TEST_CASE("build --force extract or doctor without --iso is a usage error", "[unit]") {
  CHECK_FALSE(parse({"build", "--force", "extract"}));
  CHECK_FALSE(parse({"build", "--force", "doctor"}));
  CHECK(parse({"build", "--force", "extract", "--iso", "g.iso"}));
  CHECK(parse({"build", "--force", "patch"}));
}
