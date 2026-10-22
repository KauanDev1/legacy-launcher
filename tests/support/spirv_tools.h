// SPDX-License-Identifier: MIT
// Test helper: SPIRV-Tools' spirv-val and spirv-dis (Arch package spirv-tools, installed in the dev container only;
// test-only, never shipped). Both are launched with posix_spawn and an argv vector (lr::pipeline::run_tool), on a
// temporary copy of the module. Tests that need them SKIP when the tools are absent:
//
//   if (!lr::testing::spirv_tools_available()) SKIP("spirv-val/spirv-dis not installed");
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>

#include "pipeline/process.h"
#include "support/temp_dir.h"

namespace lr::testing {

inline bool spirv_tools_available() {
  return lr::pipeline::find_program("spirv-val").has_value() && lr::pipeline::find_program("spirv-dis").has_value();
}

namespace detail {

struct SpirvToolRun {
  bool ok = false;
  std::string output;  // stdout and stderr, merged
};

inline SpirvToolRun run_spirv_tool(const char* tool, std::initializer_list<const char*> args,
                                   std::span<const uint32_t> module) {
  TempDir dir("lr_spirv");
  const std::filesystem::path file = dir.path / "module.spv";
  {
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(module.data()), static_cast<std::streamsize>(module.size_bytes()));
  }
  lr::pipeline::ToolInvocation inv;
  inv.program = tool;
  for (const char* a : args) inv.args.emplace_back(a);
  inv.args.push_back(file.string());
  SpirvToolRun run;
  inv.on_line = [&](std::string_view line) {
    run.output.append(line);
    run.output.push_back('\n');
  };
  const auto r = lr::pipeline::run_tool(inv);
  run.ok = r.ok();
  if (!r.ok() && run.output.empty()) run.output = r.error().message;
  return run;
}

}  // namespace detail

// nullopt when the module is valid for Vulkan 1.2; otherwise spirv-val's diagnostics.
inline std::optional<std::string> spirv_val(std::span<const uint32_t> module) {
  auto run = detail::run_spirv_tool("spirv-val", {"--target-env", "vulkan1.2"}, module);
  if (run.ok) return std::nullopt;
  return run.output.empty() ? std::string("spirv-val failed") : run.output;
}

// The disassembly (spirv-dis, no header comment, raw ids), or the error output when it fails.
inline std::string spirv_dis(std::span<const uint32_t> module) {
  return detail::run_spirv_tool("spirv-dis", {"--no-header", "--raw-id", "--no-color"}, module).output;
}

}  // namespace lr::testing
