// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <string>

// What a legacyrecomp-game exit code (as returned by lr::pipeline::launch) means, worded for the launchers: the CLI
// prints it in English, the GUI (Portuguese-only) in Portuguese. The codes are the game's own (src/runtime/
// guest_thread.h): 0 ok, 2 usage/load, 3 missing import, 4 fault, 5 guest fatal, 6 deadlock; 128 + N is a signal.

namespace lr::pipeline {

enum class ExitTextLanguage { English, Portuguese };

// <root>/work/logs/game-trace.log: launch always passes it as --trace-file; the game writes it only with
// --trace-imports.
std::filesystem::path game_trace_file(const std::filesystem::path& root);

// <root>/work/logs/gpu-trace.log: launch always passes it as --gpu-trace-file; the game writes it only with --trace-gpu.
std::filesystem::path gpu_trace_file(const std::filesystem::path& root);

// <root>/work/shaders/runtime: launch always passes it as --shader-cache-dir, the game's disk cache of translated
// shaders (game-derived SPIR-V; never committed or distributed).
std::filesystem::path runtime_shader_cache_dir(const std::filesystem::path& root);

// One message for `code`; empty for 0 (normal exit). The import trace hint (rerun with `run -- --trace-imports`,
// read trace_file) accompanies the codes where the calls before the stop help: 3, 4, 5 and 6.
std::string describe_game_exit(int code, const std::filesystem::path& trace_file, ExitTextLanguage language);

}  // namespace lr::pipeline
