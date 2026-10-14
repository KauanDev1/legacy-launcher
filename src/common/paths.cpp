// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/paths.h"

#include <system_error>

namespace lr {
namespace fs = std::filesystem;

DataPaths DataPaths::from_root(fs::path root) {
  DataPaths p;
  p.root = std::move(root);
  p.game = p.root / "game";
  p.work = p.root / "work";
  p.logs = p.work / "logs";
  p.bin = p.root / "bin";
  p.input = p.work / "input";
  p.image = p.work / "image";
  p.analysis = p.work / "analysis";
  p.generated = p.work / "generated";
  p.build = p.work / "build";
  p.state_file = p.work / "state.toml";
  return p;
}

fs::path default_data_root(const char* xdg_data_home, const char* home) {
  if (xdg_data_home && *xdg_data_home) return fs::path(xdg_data_home) / "legacyrecomp";
  return fs::path(home ? home : "") / ".local" / "share" / "legacyrecomp";
}

Result<fs::path> resources_dir(const fs::path& exe_path, const char* env_override) {
  std::error_code ec;
  if (env_override && *env_override) {
    fs::path p(env_override);
    if (fs::is_directory(p, ec)) return p;
    return Error{ErrorCode::NotFound, "resources directory not found: " + p.string(),
                 "set LEGACYRECOMP_RESOURCES to an existing directory"};
  }
  fs::path p = exe_path.parent_path() / ".." / "share" / "legacyrecomp";
  if (fs::is_directory(p, ec)) return p;
  return Error{ErrorCode::NotFound, "resources directory not found: " + p.string(),
               "set LEGACYRECOMP_RESOURCES"};
}

}  // namespace lr
