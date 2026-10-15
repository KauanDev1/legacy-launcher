// SPDX-License-Identifier: MIT
// End to end on the real game: pipeline::run (doctor..build) into a data root, then launch the recompiled game
// headless. M1 acceptance: the guest runs until the first unimplemented kernel import (exit 3), and a second run
// executes no stage at all.
#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "pipeline/pipeline.h"
#include "support/temp_dir.h"

using namespace lr;
using namespace lr::pipeline;
namespace fs = std::filesystem;

namespace {

// Prints stage changes and every 10% step; records every progress event.
struct RecordingSink : ProgressSink {
  std::vector<Progress> events;
  int last_percent = -1;
  StageId last_stage = StageId::Build;
  void on_progress(const Progress& p) override {
    events.push_back(p);
    const int pct = static_cast<int>(p.fraction * 100.0f);
    if (p.stage != last_stage || pct / 10 != last_percent / 10 || p.fraction >= 1.0f || p.fraction == 0.0f) {
      std::fprintf(stderr, "[%s] %3d%% %s\n", std::string(stage_name(p.stage)).c_str(), pct, p.message.c_str());
      last_stage = p.stage;
      last_percent = pct;
    }
  }
  void on_log(std::string_view) override {}
  bool cancelled() const override { return false; }
  size_t starts() const {
    size_t n = 0;
    for (const auto& e : events) n += e.message == "start";
    return n;
  }
};

void report(const Result<void>& r) {
  if (!r)
    std::fprintf(stderr, "pipeline failed: [%s] %s\nhint: %s\n", std::string(to_string(r.error().code)).c_str(),
                 r.error().message.c_str(), r.error().hint.c_str());
}

// Runs launch() with the process's stdout+stderr redirected to `log` (the game inherits them).
Result<int> launch_captured(const Options& opts, const std::vector<std::string>& extra, const fs::path& log) {
  std::fflush(nullptr);
  const int saved_out = ::dup(1), saved_err = ::dup(2);
  const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  REQUIRE(saved_out >= 0);
  REQUIRE(saved_err >= 0);
  REQUIRE(fd >= 0);
  ::dup2(fd, 1);
  ::dup2(fd, 2);
  ::close(fd);
  auto r = launch(opts, extra);
  std::fflush(nullptr);
  ::dup2(saved_out, 1);
  ::dup2(saved_err, 2);
  ::close(saved_out);
  ::close(saved_err);
  return r;
}

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream os;
  os << f.rdbuf();
  return os.str();
}

}  // namespace

// Env: LEGACYRECOMP_ISO (required), LEGACYRECOMP_TEST_ROOT (default: fresh temp dir, removed on success).
// Resources come from the build tree (<build>/share/legacyrecomp, found next to lr_tests).
TEST_CASE("real ISO: install, launch until the first unimplemented import, cached rerun", "[integration]") {
  const char* iso = std::getenv("LEGACYRECOMP_ISO");
  if (!iso) SKIP("LEGACYRECOMP_ISO not set");

  const char* root_env = std::getenv("LEGACYRECOMP_TEST_ROOT");
  const bool temp_root = !(root_env && *root_env);
  fs::path root;
  if (temp_root) {
    root = lr::testing::make_temp_dir("lr_integration_e2e");
  } else {
    root = root_env;
    fs::create_directories(root);
  }

  Options opts;
  opts.iso = iso;
  opts.root = root;

  RecordingSink first;
  auto r = run(opts, first);
  report(r);
  REQUIRE(r);
  CHECK(fs::exists(root / "bin" / "legacyrecomp-game"));

  const fs::path log = root / "work" / "logs" / "e2e-launch.log";
  auto code = launch_captured(opts, {"--headless"}, log);
  REQUIRE(code);
  const std::string out = slurp(log);
  std::fprintf(stderr, "legacyrecomp-game exited with %d:\n%s\n", code.value(), out.c_str());
  CHECK(code.value() == 3);
  CHECK(out.find("Unimplemented import:") != std::string::npos);

  RecordingSink second;
  auto again = run(opts, second);
  report(again);
  REQUIRE(again);
  CHECK(second.starts() == 0);
  CHECK(second.events.size() == 7);  // one "cached" event per stage

  if (temp_root) fs::remove_all(root);
}
