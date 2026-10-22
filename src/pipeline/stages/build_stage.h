// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "common/result.h"
#include "pipeline/stages/stages.h"

namespace lr::pipeline {

// Parses Ninja's "[n/N] ..." status prefix into n/N in [0,1]; nullopt for any other line.
std::optional<float> parse_ninja_progress(std::string_view line);

// Fingerprint of the build inputs: the generated directory listing (file names + sizes, sorted; import_stubs.cpp,
// written by the build stage itself, is left out) and the SHA-256 of every file under <resources>/runtime
// (libraries, headers, main.cpp, template). Missing generated dir -> NotFound.
// `shader_cache` (work/shaders/shader_cache.cpp, compiled into the game) is hashed too; empty = not part of the build.
Result<std::string> build_fingerprint(const fs::path& generated, const fs::path& runtime_dir,
                                      const fs::path& shader_cache = {});

// Parallel compile jobs for the game: `requested` if non-zero, else min(hardware threads, RAM / 2 GiB), at least 1.
// Generated translation units are 1-3 MB of C++ each; one clang -O2 process peaks at ~0.3-1 GiB on them.
unsigned build_jobs(unsigned requested, unsigned hardware_threads, uint64_t total_ram_bytes);

// CMake refuses to reuse a build dir configured from another source dir ("does not match the source used to generate
// cache") or whose cache was written in another build dir ("CMakeCache.txt directory ... is different"), and never
// recovers. If <build_dir>/CMakeCache.txt exists and its CMAKE_HOME_DIRECTORY is not `template_dir` (e.g. the
// launcher moved or was installed elsewhere) or its CMAKE_CACHEFILE_DIR is not `build_dir` (the data root moved), or
// the cache is unreadable / lacks either entry, removes `build_dir` and returns true. No cache -> false, nothing
// touched. IoError if removal fails.
Result<bool> reset_stale_build_dir(const fs::path& build_dir, const fs::path& template_dir);

}  // namespace lr::pipeline
