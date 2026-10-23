// SPDX-License-Identifier: MIT
// M3a on the real game (spec section 14): installed through pipeline::run, the recompiled game runs headless on the
// emulated GPU (NullBackend, nothing drawn) and drives it: it reaches 60 processed swaps within 300 s and
// --exit-after-swaps 60 ends it with exit code 0, without a message box on the way.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "pipeline/game_exit.h"
#include "pipeline/pipeline.h"
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
