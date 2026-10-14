// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "common/paths.h"

namespace fs = std::filesystem;

TEST_CASE("default_data_root", "[unit]") {
  CHECK(lr::default_data_root("/x", "/h") == fs::path("/x/legacyrecomp"));
  CHECK(lr::default_data_root(nullptr, "/h") == fs::path("/h/.local/share/legacyrecomp"));
  CHECK(lr::default_data_root("", "/h") == fs::path("/h/.local/share/legacyrecomp"));
}

TEST_CASE("DataPaths::from_root", "[unit]") {
  auto p = lr::DataPaths::from_root("/r");
  CHECK(p.root == fs::path("/r"));
  CHECK(p.game == fs::path("/r/game"));
  CHECK(p.work == fs::path("/r/work"));
  CHECK(p.logs == fs::path("/r/work/logs"));
  CHECK(p.bin == fs::path("/r/bin"));
  CHECK(p.input == fs::path("/r/work/input"));
  CHECK(p.image == fs::path("/r/work/image"));
  CHECK(p.analysis == fs::path("/r/work/analysis"));
  CHECK(p.generated == fs::path("/r/work/generated"));
  CHECK(p.build == fs::path("/r/work/build"));
  CHECK(p.state_file == fs::path("/r/work/state.toml"));
}

TEST_CASE("resources_dir", "[unit]") {
  auto dir = fs::temp_directory_path() / "lr_test_res";
  fs::remove_all(dir);
  fs::create_directories(dir / "share" / "legacyrecomp");
  fs::create_directories(dir / "bin");
  auto env = dir / "envres";
  fs::create_directories(env);

  auto a = lr::resources_dir(dir / "bin" / "lr", env.c_str());
  REQUIRE(a.ok());
  CHECK(a.value() == env);

  auto b = lr::resources_dir(dir / "bin" / "lr", nullptr);
  REQUIRE(b.ok());
  CHECK(fs::equivalent(b.value(), dir / "share" / "legacyrecomp"));

  auto c = lr::resources_dir(dir / "bin" / "lr", (dir / "nope").c_str());
  REQUIRE(!c.ok());
  CHECK(c.error().code == lr::ErrorCode::NotFound);
  CHECK(c.error().hint.find("LEGACYRECOMP_RESOURCES") != std::string::npos);

  auto d = lr::resources_dir(dir / "x" / "bin" / "lr", nullptr);
  REQUIRE(!d.ok());
  CHECK(d.error().code == lr::ErrorCode::NotFound);
  fs::remove_all(dir);
}

TEST_CASE("data root precedence is flag, env, XDG default", "[unit]") {
  using std::filesystem::path;
  CHECK(lr::resolve_data_root(path("/flag"), "/env", "/xdg", "/home/u") == path("/flag"));
  CHECK(lr::resolve_data_root(std::nullopt, "/env", "/xdg", "/home/u") == path("/env"));
  CHECK(lr::resolve_data_root(std::nullopt, "", "/xdg", "/home/u") == path("/xdg/legacyrecomp"));
  CHECK(lr::resolve_data_root(std::nullopt, nullptr, nullptr, "/home/u") == path("/home/u/.local/share/legacyrecomp"));
}

