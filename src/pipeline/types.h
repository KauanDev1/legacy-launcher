// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace lr::pipeline {

namespace fs = std::filesystem;

enum class StageId { Doctor, Extract, Verify, Patch, Analyse, Recompile, Shaders, Build };

std::string_view stage_name(StageId id);
std::optional<StageId> parse_stage(std::string_view name);

struct Progress {
  StageId stage;
  float fraction;
  std::string message;
};

struct ProgressSink {
  virtual ~ProgressSink();
  virtual void on_progress(const Progress& progress) = 0;
  virtual void on_log(std::string_view line) = 0;
  virtual bool cancelled() const = 0;
};

struct Options {
  fs::path iso;
  fs::path root;
  std::set<StageId> force;
  unsigned jobs = 0;  // 0 = hardware concurrency
};

}  // namespace lr::pipeline
