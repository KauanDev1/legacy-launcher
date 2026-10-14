// SPDX-License-Identifier: MIT
#include "pipeline/types.h"

#include <array>

namespace lr::pipeline {

namespace {
constexpr std::array<std::pair<StageId, std::string_view>, 7> kNames{{
    {StageId::Doctor, "doctor"},
    {StageId::Extract, "extract"},
    {StageId::Verify, "verify"},
    {StageId::Patch, "patch"},
    {StageId::Analyse, "analyse"},
    {StageId::Recompile, "recompile"},
    {StageId::Build, "build"},
}};
}  // namespace

std::string_view stage_name(StageId id) {
  for (auto& [i, n] : kNames)
    if (i == id) return n;
  return "unknown";
}

std::optional<StageId> parse_stage(std::string_view name) {
  for (auto& [i, n] : kNames)
    if (n == name) return i;
  return std::nullopt;
}

ProgressSink::~ProgressSink() = default;

}  // namespace lr::pipeline
