// SPDX-License-Identifier: GPL-3.0-or-later
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
Result<std::string> build_fingerprint(const fs::path& generated, const fs::path& runtime_dir);

// Parallel compile jobs for the game: `requested` if non-zero, else min(hardware threads, RAM / 2 GiB), at least 1.
// Generated translation units are 1-3 MB of C++ each; one clang -O2 process peaks at ~0.3-1 GiB on them.
unsigned build_jobs(unsigned requested, unsigned hardware_threads, uint64_t total_ram_bytes);

}  // namespace lr::pipeline
