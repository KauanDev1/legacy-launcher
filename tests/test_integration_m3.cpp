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
