// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/result.h"

namespace lr::pipeline {

struct ToolInvocation {
  std::filesystem::path program;  // absolute path or bare name (resolved via PATH)
  std::vector<std::string> args;  // passed verbatim, no shell involved
  std::filesystem::path cwd;      // empty = inherit
  std::filesystem::path log_file; // empty = no file logging; appended, parent dirs created
  std::function<bool()> cancelled;
  std::function<void(std::string_view)> on_line;
};

// Runs the tool with stdout+stderr merged; stdin is /dev/null. Cancellation: SIGTERM, SIGKILL after 3 s.
// Each tool runs in its own process group: cancel signals the whole group (SIGTERM, SIGKILL after 3 s, then
// we stop reading after a 500 ms grace and reap). Tools therefore do NOT receive the terminal's SIGINT; the
// caller's SIGINT handler must set the `cancelled` flag. Children get default SIGPIPE and an empty signal mask.
// SIGPIPE is set to SIG_IGN process-wide on first use (writes to a dead pipe return EPIPE instead).
Result<void> run_tool(const ToolInvocation& inv);

// Captures stdout only (stderr and stdin go to /dev/null). Non-zero exit -> ToolFailed.
Result<std::string> capture_output(const std::filesystem::path& program, const std::vector<std::string>& args);

std::optional<std::filesystem::path> find_program(std::string_view name);

}  // namespace lr::pipeline
