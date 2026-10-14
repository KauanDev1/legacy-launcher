// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/result.h"
#include "pipeline/types.h"

namespace lr::cli {

struct CliArgs {
  std::string command;  // doctor | inspect | install | build | run
  std::optional<std::filesystem::path> iso, root;
  std::set<pipeline::StageId> force;
  unsigned jobs = 0;
  // run: everything after `--`. inspect: its own arguments, uninterpreted.
  std::vector<std::string> passthrough;
};

// `args` excludes argv[0]. Errors are InvalidFormat (the caller prints usage and exits 2).
Result<CliArgs> parse_args(std::span<const std::string_view> args);

std::string_view usage_text();

// Data root precedence: --root, then $LEGACYRECOMP_ROOT (empty counts as unset), then the XDG default.
// Pure: callers pass getenv() results.
std::filesystem::path resolve_root(const std::optional<std::filesystem::path>& flag, const char* env_root,
                                   const char* xdg_data_home, const char* home);

}  // namespace lr::cli
