// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "common/result.h"
#include "pipeline/stages/stages.h"
#include "pipeline/switch_scan.h"

namespace lr::pipeline {

// Parses the repo recomp.toml template, sets [main] file_path / switch_table_file_path /
// out_directory_path to the given absolute paths and removes patch_file_path / patched_file_path
// (the patch stage already produced the patched image). Everything else is kept as-is.
// Relative paths -> Internal. XenonRecomp prefixes every path with the TOML's directory, so the
// recompile stage runs it with cwd = work/ and the bare argument "recomp.toml" (empty prefix).
// `extra_functions` (from the analyse stage) are appended to [main] functions, except those that
// overlap an entry already in the template (hand-written overrides win).
Result<std::string> render_recomp_config(const std::string& template_toml, const fs::path& image,
                                         const fs::path& switch_tables, const fs::path& out_dir,
                                         std::span<const FunctionRange> extra_functions = {});

// Reads `functions = [{ address, size }, ...]` (the analyse stage's functions.toml).
Result<std::vector<FunctionRange>> parse_function_overrides(const std::string& toml_text);

// Reads the hand-written `[main] functions` of a recomp.toml.
Result<std::vector<FunctionRange>> manual_functions(const std::string& recomp_toml_text);

// Parses XenonRecomp's "Recompiling functions... 12.5%" line into [0,1]; nullopt otherwise.
std::optional<float> parse_recomp_progress(std::string_view line);

// XenonRecomp output line kinds. Every "ERROR:" line is fatal for the stage: XenonRecomp exits 0
// even when it emitted wrong code (e.g. a switch case jumping outside its function becomes `return;`).
enum class RecompLine { Progress, Error, Other };
RecompLine classify_recomp_line(std::string_view line);

// Stage error for `count` ERROR lines; `first` holds the first few, quoted in message and hint.
Error recomp_errors(size_t count, std::span<const std::string> first, const fs::path& log);

}  // namespace lr::pipeline
