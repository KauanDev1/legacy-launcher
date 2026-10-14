// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <optional>

#include "common/result.h"

namespace lr {

struct DataPaths {
  std::filesystem::path root, game, work, logs, bin, input, image, analysis, generated, build,
      state_file;
  static DataPaths from_root(std::filesystem::path root);
};

// Pure: callers pass getenv("XDG_DATA_HOME") and getenv("HOME"). Empty XDG is treated as unset.
std::filesystem::path default_data_root(const char* xdg_data_home, const char* home);

// Data root precedence: explicit flag, then $LEGACYRECOMP_ROOT (empty counts as unset), then default_data_root.
// Pure: callers pass getenv() results. Shared by the CLI and GUI launchers.
std::filesystem::path resolve_data_root(const std::optional<std::filesystem::path>& flag, const char* env_root,
                                        const char* xdg_data_home, const char* home);

// env_override if set and existing; else <exe dir>/../share/legacyrecomp if it exists; else NotFound.
Result<std::filesystem::path> resources_dir(const std::filesystem::path& exe_path,
                                            const char* env_override);

}  // namespace lr
