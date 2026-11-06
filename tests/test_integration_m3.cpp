// SPDX-License-Identifier: MIT
// M3a on the real game (spec section 14): installed through pipeline::run, the recompiled game runs headless on the
// emulated GPU (NullBackend, nothing drawn) and drives it: it reaches 60 processed swaps within 300 s and
// --exit-after-swaps 60 ends it with exit code 0, without a message box on the way. Below: every loaded shader
// translates (Task 15.11), and the frame of swap 600 is the title screen by objective measures (Task 22).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "pipeline/game_exit.h"
#include "pipeline/pipeline.h"
#include "pipeline/process.h"
#include "support/game_launch.h"
#include "support/temp_dir.h"

using namespace lr;
using namespace lr::pipeline;
namespace fs = std::filesystem;

namespace {

struct QuietSink : ProgressSink {
  void on_progress(const Progress& p) override {
    if (p.message == "start") std::fprintf(stderr, "[%s] start\n", std::string(stage_name(p.stage)).c_str());
  }
  void on_log(std::string_view) override {}
  bool cancelled() const override { return false; }
};

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream os;
  os << f.rdbuf();
  return os.str();
}

}  // namespace

// Env: LEGACYRECOMP_ISO (required), LEGACYRECOMP_TEST_ROOT (default: fresh temp dir, removed on success; an existing
// installed root makes the pipeline a cached no-op).
TEST_CASE("real ISO: the game drives the GPU for 60 swaps and exits cleanly", "[integration]") {
  const char* iso = std::getenv("LEGACYRECOMP_ISO");
  if (!iso) SKIP("LEGACYRECOMP_ISO not set");

  const char* root_env = std::getenv("LEGACYRECOMP_TEST_ROOT");
  const bool temp_root = !(root_env && *root_env);
  const fs::path root = temp_root ? lr::testing::make_temp_dir("lr_integration_m3") : fs::path(root_env);
  fs::create_directories(root);

  Options opts;
  opts.iso = iso;
  opts.root = root;
  QuietSink sink;
  auto r = run(opts, sink);
  if (!r) std::fprintf(stderr, "pipeline failed: %s\nhint: %s\n", r.error().message.c_str(), r.error().hint.c_str());
  REQUIRE(r);

  const fs::path log = root / "work" / "logs" / "m3-launch.log";
  const auto started = std::chrono::steady_clock::now();
  Result<int> code = Error{ErrorCode::IoError, "not launched", ""};
  bool timed_out = false;
  {
    lr::testing::Watchdog watchdog(std::chrono::seconds(300));
    code = lr::testing::launch_captured(opts, {"--headless", "--exit-after-swaps", "60"}, log);
    timed_out = watchdog.fired();
  }
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started);
  REQUIRE(code);
  const std::string out = slurp(log);
  std::fprintf(stderr, "legacyrecomp-game exited with %d after %lld s:\n%s\n", code.value(),
               static_cast<long long>(seconds.count()), out.c_str());
  CHECK_FALSE(timed_out);
  CHECK(code.value() == 0);
  CHECK(out.find("[gpu] exit after 60 swaps") != std::string::npos);
  CHECK(out.find("[game] message box:") == std::string::npos);

  if (temp_root) fs::remove_all(root);
}

// Translator gate (Task 15.11, ADR 0005 M9): every shader the game loads during its first 300 swaps translates. The
// trace (--trace-gpu) names each key once with "translated", "disk" or "failed: <reason>"; the exit summary counts
// them. Only counts and the failing lines' reasons are printed, never shader data.
TEST_CASE("the game's loaded shaders all translate", "[integration]") {
  const char* iso = std::getenv("LEGACYRECOMP_ISO");
  if (!iso) SKIP("LEGACYRECOMP_ISO not set");

  const char* root_env = std::getenv("LEGACYRECOMP_TEST_ROOT");
  const bool temp_root = !(root_env && *root_env);
  const fs::path root = temp_root ? lr::testing::make_temp_dir("lr_integration_m3_shaders") : fs::path(root_env);
  fs::create_directories(root);

  Options opts;
  opts.iso = iso;
  opts.root = root;
  QuietSink sink;
  auto r = run(opts, sink);
  if (!r) std::fprintf(stderr, "pipeline failed: %s\nhint: %s\n", r.error().message.c_str(), r.error().hint.c_str());
  REQUIRE(r);

  const fs::path log = root / "work" / "logs" / "m3-shaders.log";
  Result<int> code = Error{ErrorCode::IoError, "not launched", ""};
  bool timed_out = false;
  {
    lr::testing::Watchdog watchdog(std::chrono::seconds(900));
    code = lr::testing::launch_captured(opts, {"--headless", "--trace-gpu", "--exit-after-swaps", "300"}, log);
    timed_out = watchdog.fired();
  }
  REQUIRE(code);
  const std::string out = slurp(log);

  std::size_t keys = 0, failed = 0;
  {
    std::ifstream trace(gpu_trace_file(root));
    std::string line;
    while (std::getline(trace, line)) {
      if (line.rfind("shader vs ", 0) != 0 && line.rfind("shader ps ", 0) != 0) continue;
      if (line.find(" mod=0x") == std::string::npos) continue;  // "shader load ..." lines carry no key
      ++keys;
      if (line.find(" failed: ") != std::string::npos) {
        ++failed;
        std::fprintf(stderr, "%s\n", line.c_str());
      }
    }
  }
  const auto summary_at = out.find("[gpu] shaders: ");
  const std::string summary =
      summary_at == std::string::npos ? "" : out.substr(summary_at, out.find('\n', summary_at) - summary_at);
  std::fprintf(stderr, "legacyrecomp-game exited with %d; %zu shader keys in the trace, %zu failed\n%s\n", code.value(),
               keys, failed, summary.c_str());
  CHECK_FALSE(timed_out);
  CHECK(code.value() == 0);
  CHECK(out.find("[gpu] exit after 300 swaps") != std::string::npos);
  CHECK(keys > 0);
  CHECK(failed == 0);
  CHECK(summary.find(" failed=0 ") != std::string::npos);

  if (temp_root) fs::remove_all(root);
}

// The title screen (Task 22, Step 3): on Vulkan (lavapipe in the container) with the validation layer, the game
// reaches 600 swaps, and the frame of swap 600 passes objective thresholds measured by tools/frame_stats.py: at
// least 64 distinct colours, a mean luminance between 0.05 and 0.95, at least 2% edge pixels. Nobody views the frame:
// only the three numbers are printed, and the screenshot (game data) is deleted.
TEST_CASE("real ISO: the game reaches its title screen", "[integration]") {
  const char* iso = std::getenv("LEGACYRECOMP_ISO");
  if (!iso) SKIP("LEGACYRECOMP_ISO not set");
  const auto python = find_program("python3");
  if (!python) FAIL("python3 is needed for tools/frame_stats.py");

  const char* root_env = std::getenv("LEGACYRECOMP_TEST_ROOT");
  const bool temp_root = !(root_env && *root_env);
  const fs::path root = temp_root ? lr::testing::make_temp_dir("lr_integration_m3_title") : fs::path(root_env);
  fs::create_directories(root);

  Options opts;
  opts.iso = iso;
  opts.root = root;
  QuietSink sink;
  auto r = run(opts, sink);
  if (!r) std::fprintf(stderr, "pipeline failed: %s\nhint: %s\n", r.error().message.c_str(), r.error().hint.c_str());
  REQUIRE(r);

  const fs::path log = root / "work" / "logs" / "m3-title.log";
  const fs::path shot = root / "work" / "logs" / "m3-title.png";
  fs::remove(shot);
  const auto started = std::chrono::steady_clock::now();
  Result<int> code = Error{ErrorCode::IoError, "not launched", ""};
  bool timed_out = false;
  ::setenv("LR_VULKAN_VALIDATION", "1", 1);
  {
    lr::testing::Watchdog watchdog(std::chrono::seconds(3300));
    const std::vector<std::string> args = {"--headless", "--gpu", "vulkan", "--screenshot-after", "600", shot.string()};
    code = lr::testing::launch_captured(opts, args, log);
    timed_out = watchdog.fired();
  }
  ::unsetenv("LR_VULKAN_VALIDATION");
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started);
  REQUIRE(code);
  const std::string out = slurp(log);
  const auto summary_at = out.find("[gpu] shaders: ");
  const std::string summary =
      summary_at == std::string::npos ? "" : out.substr(summary_at, out.find('\n', summary_at) - summary_at);
  std::fprintf(stderr, "legacyrecomp-game exited with %d after %lld s\n%s\n", code.value(),
               static_cast<long long>(seconds.count()), summary.c_str());
  CHECK_FALSE(timed_out);
  CHECK(code.value() == 0);
  CHECK(out.find("[gpu] screenshot after 600 swaps") != std::string::npos);
  CHECK(out.find("[vulkan]") == std::string::npos);  // no validation message
  CHECK(out.find("[game] message box:") == std::string::npos);
  REQUIRE(fs::exists(shot));

  auto stats = capture_output(*python, {"-I", std::string(LR_SOURCE_DIR) + "/tools/frame_stats.py", shot.string()});
  fs::remove(shot);
  REQUIRE(stats);
  std::fprintf(stderr, "frame_stats: %s", stats.value().c_str());
  unsigned long colors = 0;
  double luminance = -1, edges = -1;
  REQUIRE(std::sscanf(stats.value().c_str(), "colors=%lu mean_luminance=%lf edge_fraction=%lf", &colors, &luminance,
                      &edges) == 3);
  CHECK(colors >= 64);
  CHECK(luminance >= 0.05);
  CHECK(luminance <= 0.95);
  CHECK(edges >= 0.02);

  if (temp_root) fs::remove_all(root);
}
