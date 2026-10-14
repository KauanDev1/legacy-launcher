// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

#include "common/result.h"
#include "pipeline/types.h"

namespace lr::pipeline {

struct GameConfig {
  uint32_t title_id = 0;
  std::string package, title_update, package_xex, tu_xexp;
  std::string hash_package_xex, hash_tu_xexp, hash_patched_image;
  fs::path recomp_toml;
};

// Reads <dir>/game.toml. Never throws.
Result<GameConfig> load_game_config(const fs::path& dir);

}  // namespace lr::pipeline
