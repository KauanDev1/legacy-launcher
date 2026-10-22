// SPDX-License-Identifier: MIT
// End to end on the real game: pipeline::run (doctor..build) into a data root, and a second run executes no stage at
// all. Running the game is tests/test_integration_m3.cpp (until M3 this test also expected the startup to stop at the
// first GPU-creating Vd import, exit 3; the GPU imports exist now).
#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

}  // namespace

// Env: LEGACYRECOMP_ISO (required), LEGACYRECOMP_TEST_ROOT (default: fresh temp dir, removed on success).
// Resources come from the build tree (<build>/share/legacyrecomp, found next to lr_tests).
TEST_CASE("real ISO: install, cached rerun", "[integration]") {
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

  RecordingSink second;
  auto again = run(opts, second);
  report(again);
  REQUIRE(again);
  CHECK(second.starts() == 0);
  CHECK(second.events.size() == 7);  // one "cached" event per stage

  if (temp_root) fs::remove_all(root);
}
