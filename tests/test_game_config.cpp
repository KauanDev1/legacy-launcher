// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "pipeline/game_config.h"

using namespace lr;
using namespace lr::pipeline;

TEST_CASE("load_game_config reads the repo config", "[unit]") {
  const std::filesystem::path dir = std::filesystem::path(LR_SOURCE_DIR) / "config/minecraft-tu1";
  auto cfg = load_game_config(dir);
  REQUIRE(cfg);
  CHECK(cfg.value().title_id == 0x584111F7u);
  CHECK(cfg.value().package_xex == "default.xex");
  CHECK(cfg.value().tu_xexp == "default.xexp");
  CHECK(cfg.value().hash_package_xex.size() == 64);
  CHECK(cfg.value().hash_patched_image.size() == 64);
  CHECK(cfg.value().recomp_toml == dir / "recomp.toml");
}

TEST_CASE("load_game_config error cases", "[unit]") {
  auto missing = load_game_config("/nonexistent/dir/for/lr");
  REQUIRE_FALSE(missing);
  CHECK(missing.error().code == ErrorCode::NotFound);

  const auto tmp = std::filesystem::temp_directory_path() / "lr_gc_test";
  std::filesystem::create_directories(tmp);
  { std::ofstream(tmp / "game.toml") << "title_id = = 3"; }
  auto bad = load_game_config(tmp);
  REQUIRE_FALSE(bad);
  CHECK(bad.error().code == ErrorCode::InvalidFormat);

  { std::ofstream(tmp / "game.toml") << "title_id = 1\n[sources]\npackage = \"a\"\n"; }
  auto partial = load_game_config(tmp);
  REQUIRE_FALSE(partial);
  CHECK(partial.error().code == ErrorCode::InvalidFormat);
  CHECK(partial.error().message.find("title_update") != std::string::npos);
  std::filesystem::remove_all(tmp);
}
