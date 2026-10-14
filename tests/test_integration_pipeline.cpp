// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "common/paths.h"
#include "pipeline/doctor.h"
#include "pipeline/game_config.h"
#include "pipeline/runner.h"
#include "pipeline/stages/stages.h"
#include "pipeline/state.h"

using namespace lr;
using namespace lr::pipeline;
namespace fs = std::filesystem;

namespace {

struct PrintSink : ProgressSink {
  int last_percent = -1;
  StageId last_stage = StageId::Build;
  void on_progress(const Progress& p) override {
    int pct = static_cast<int>(p.fraction * 100.0f);
    if (p.stage != last_stage || pct / 10 != last_percent / 10 || p.fraction >= 1.0f) {
      std::fprintf(stderr, "[%s] %3d%% %s\n", std::string(stage_name(p.stage)).c_str(), pct, p.message.c_str());
      last_stage = p.stage;
      last_percent = pct;
    }
  }
  void on_log(std::string_view line) override {
    std::fprintf(stderr, "  %.*s\n", static_cast<int>(line.size()), line.data());
  }
  bool cancelled() const override { return false; }
};

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream os;
  os << f.rdbuf();
  return os.str();
}

}  // namespace

// Env: LEGACYRECOMP_ISO (required), LEGACYRECOMP_TEST_ROOT (default: fresh temp dir, removed on success),
// LR_STOP_AFTER (stage name, default "recompile"). Uses the resources staged in the build tree.
TEST_CASE("real ISO: pipeline doctor..recompile", "[integration]") {
  const char* iso = std::getenv("LEGACYRECOMP_ISO");
  if (!iso) SKIP("LEGACYRECOMP_ISO not set");

  const char* stop_env = std::getenv("LR_STOP_AFTER");
  auto stop = parse_stage(stop_env && *stop_env ? stop_env : "recompile");
  REQUIRE(stop.has_value());
  REQUIRE(*stop <= StageId::Recompile);

  const char* root_env = std::getenv("LEGACYRECOMP_TEST_ROOT");
  const bool temp_root = !(root_env && *root_env);
  const fs::path root = temp_root ? fs::temp_directory_path() / "lr_integration_pipeline" : fs::path(root_env);
  if (temp_root) fs::remove_all(root);
  fs::create_directories(root);

  const fs::path resources = LR_RESOURCES_DIR;
  auto game = load_game_config(resources / "config" / "minecraft-tu1");
  REQUIRE(game);

  SystemDoctorProbe probe;
  std::vector<std::unique_ptr<Stage>> owned;
  owned.push_back(make_doctor_stage(probe));
  owned.push_back(make_extract_stage());
  owned.push_back(make_verify_stage());
  owned.push_back(make_patch_stage());
  owned.push_back(make_analyse_stage());
  owned.push_back(make_recompile_stage());
  std::vector<Stage*> stages;
  for (auto& s : owned) {
    stages.push_back(s.get());
    if (s->id() == *stop) break;
  }

  Options opts;
  opts.iso = iso;
  opts.root = root;
  PrintSink sink;
  StageContext ctx{opts, DataPaths::from_root(root), resources, game.value(), sink};
  auto state = StateStore::load(ctx.paths.state_file);
  REQUIRE(state);
  auto r = run_stages(stages, ctx, state.value());
  if (!r) {
    std::fprintf(stderr, "pipeline failed: [%s] %s\nhint: %s\n", std::string(to_string(r.error().code)).c_str(),
                 r.error().message.c_str(), r.error().hint.c_str());
  }
  REQUIRE(r);

  if (*stop >= StageId::Patch) CHECK(fs::exists(ctx.paths.image / "default_tu1.xex"));
  if (*stop >= StageId::Analyse) CHECK(fs::exists(ctx.paths.analysis / "switch_tables.toml"));
  if (*stop == StageId::Recompile) {
    CHECK(fs::exists(ctx.paths.generated / "ppc_func_mapping.cpp"));
    CHECK(fs::exists(ctx.paths.generated / "ppc_recomp_shared.h"));
    CHECK(slurp(ctx.paths.generated / "ppc_config.h").find("#define PPC_IMAGE_BASE") != std::string::npos);
  }
  if (temp_root) fs::remove_all(root);
}
