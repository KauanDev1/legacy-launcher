// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "common/result.h"
#include "pipeline/stage.h"

namespace lr::pipeline {

// "translated N of M" (the last line XenosRecomp prints) -> {N, M}; nullopt for any other line.
std::optional<std::pair<unsigned, unsigned>> parse_translated_line(std::string_view line);

// Fingerprint of the shaders stage: sha(image) | XenosRecomp commit (with the patches hash) | DXC version.
std::string shaders_fingerprint(std::string_view image_sha256, std::string_view xenosrecomp_commit,
                                std::string_view dxc_version);

// work/shaders/shader_cache.cpp: the generated cache the build stage compiles into the game.
fs::path shader_cache_path(const DataPaths& paths);

// First line of `<dxc> --version` (or "unknown" when it cannot be run). Never fails.
std::string query_dxc_version(const fs::path& dxc);

struct ShadersStageConfig {
  // XenosRecomp submodule commit plus "+patches:<hash>" (resources.cmake); empty = the build's value.
  std::string xenosrecomp_commit;
  // DXC version for the fingerprint; empty function = query the toolchain's dxc (bundled or on PATH).
  std::function<std::string(const fs::path& resources)> dxc_version;
};

// Runs tools/XenosRecomp on work/image/default_tu1.xex (copied to work/shaders/in/) and writes
// work/generated/shader_cache.cpp; the build stage compiles it into the game.
std::unique_ptr<Stage> make_shaders_stage(ShadersStageConfig config = {});

}  // namespace lr::pipeline
