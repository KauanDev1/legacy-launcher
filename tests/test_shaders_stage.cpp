// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "pipeline/pipeline.h"
#include "pipeline/stages/shaders_stage.h"
#include "support/temp_dir.h"

using namespace lr;
using namespace lr::pipeline;
namespace fs = std::filesystem;

namespace {

struct Sink : ProgressSink {
  std::vector<std::string> messages;
  void on_progress(const Progress& p) override { messages.push_back(p.message); }
  void on_log(std::string_view) override {}
  bool cancelled() const override { return false; }
};

void write(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << text;
}

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream os;
  os << f.rdbuf();
  return os.str();
}

// A data root with a fake image and a fake XenosRecomp that records its argv/cwd, prints the given summary line
// and, like the real tool, writes the cache file (its second argument; arguments: <in> <out> <common> ...).
struct Fixture {
  lr::testing::TempDir tmp{"lr_shaders_stage"};
  fs::path res = tmp.path / "res", root = tmp.path / "root";
  Options opts;
  GameConfig game{};
  Sink sink;
  ShadersStageConfig cfg;

  explicit Fixture(const std::string& summary = "translated 3 of 3", int exit_code = 0) {
    opts.root = root;
    cfg.xenosrecomp_commit = "aaaa+patches:1111";
    cfg.dxc_version = [](const fs::path&) { return std::string("dxc 1.8"); };
    write(root / "work" / "image" / "default_tu1.xex", "image-bytes");
    write(res / "tools" / "xenos_shader_common.h", "// prelude");
    const fs::path tool = res / "tools" / "XenosRecomp";
    write(tool, "#!/bin/sh\n"
                "{ for a in \"$@\"; do echo \"$a\"; done; pwd; } > \"" + (tmp.path / "argv.txt").string() + "\"\n"
                "echo \"container 0x1 microcode 0x2 pixel literal-loops 0\"\n"
                "echo \"" + summary + "\"\n"
                "echo cache > \"$2\"\n"
                "exit " + std::to_string(exit_code) + "\n");
    ::chmod(tool.c_str(), 0755);
  }
  StageContext ctx() { return StageContext{opts, DataPaths::from_root(root), res, game, sink}; }
};

}  // namespace

TEST_CASE("shaders stage runs the tool and records the count", "[unit]") {
  Fixture f;
  auto ctx = f.ctx();
  auto stage = make_shaders_stage(f.cfg);
  REQUIRE(stage->id() == StageId::Shaders);
  auto r = stage->execute(ctx);
  REQUIRE(r);

  const fs::path gen = ctx.paths.generated / "shader_cache.cpp";
  CHECK(fs::exists(gen));
  CHECK(stage->outputs(ctx) == std::vector<fs::path>{gen});
  CHECK(slurp(ctx.paths.work / "shaders" / "in" / "default_tu1.xex") == "image-bytes");
  CHECK(f.sink.messages.back() == "translated 3 of 3");
  CHECK(slurp(ctx.paths.logs / "shaders.log").find("translated 3 of 3") != std::string::npos);

  // argv vector: <in> <out> <prelude> --hlsl-dir <dir>, then pwd (cwd = work/shaders).
  std::istringstream argv(slurp(f.tmp.path / "argv.txt"));
  std::vector<std::string> lines;
  for (std::string l; std::getline(argv, l);) lines.push_back(l);
  REQUIRE(lines.size() == 6);
  CHECK(lines[0] == (ctx.paths.work / "shaders" / "in").string());
  CHECK(lines[1] == gen.string());
  CHECK(lines[2] == (f.res / "tools" / "xenos_shader_common.h").string());
  CHECK(lines[3] == "--hlsl-dir");
  CHECK(lines[5] == (ctx.paths.work / "shaders").string());
}

TEST_CASE("fewer translated than found fails the stage", "[unit]") {
  SECTION("tool exits 0 but reports 2 of 3") {
    Fixture f("translated 2 of 3");
    auto ctx = f.ctx();
    auto r = make_shaders_stage(f.cfg)->execute(ctx);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::ToolFailed);
    CHECK(r.error().message.find("2 of 3") != std::string::npos);
    CHECK(r.error().hint.find("shaders.log") != std::string::npos);
    CHECK_FALSE(fs::exists(ctx.paths.generated / "shader_cache.cpp"));
  }
  SECTION("tool exits non-zero") {
    Fixture f("translated 2 of 3", 1);
    auto ctx = f.ctx();
    auto r = make_shaders_stage(f.cfg)->execute(ctx);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::ToolFailed);
  }
  SECTION("nothing found (0 of 0) is a failure too") {
    Fixture f("translated 0 of 0");
    auto ctx = f.ctx();
    auto r = make_shaders_stage(f.cfg)->execute(ctx);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::ToolFailed);
  }
  SECTION("no summary line at all") {
    Fixture f("done");
    auto ctx = f.ctx();
    auto r = make_shaders_stage(f.cfg)->execute(ctx);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::ToolFailed);
  }
}

TEST_CASE("missing XenosRecomp is a ToolMissing that points at the doctor", "[unit]") {
  Fixture f;
  fs::remove(f.res / "tools" / "XenosRecomp");
  auto ctx = f.ctx();
  auto r = make_shaders_stage(f.cfg)->execute(ctx);
  REQUIRE_FALSE(r);
  CHECK(r.error().code == ErrorCode::ToolMissing);
  CHECK(r.error().message.find("XenosRecomp") != std::string::npos);
  CHECK(r.error().hint.find("doctor") != std::string::npos);
}

TEST_CASE("fingerprint changes with the XenosRecomp commit", "[unit]") {
  Fixture f;
  auto ctx = f.ctx();
  auto base = make_shaders_stage(f.cfg)->fingerprint(ctx);
  REQUIRE(base);
  CHECK(make_shaders_stage(f.cfg)->fingerprint(ctx).value() == base.value());

  ShadersStageConfig other_commit = f.cfg;
  other_commit.xenosrecomp_commit = "bbbb+patches:1111";
  CHECK(make_shaders_stage(other_commit)->fingerprint(ctx).value() != base.value());

  ShadersStageConfig other_patches = f.cfg;
  other_patches.xenosrecomp_commit = "aaaa+patches:2222";
  CHECK(make_shaders_stage(other_patches)->fingerprint(ctx).value() != base.value());

  ShadersStageConfig other_dxc = f.cfg;
  other_dxc.dxc_version = [](const fs::path&) { return std::string("dxc 1.9"); };
  CHECK(make_shaders_stage(other_dxc)->fingerprint(ctx).value() != base.value());

  write(ctx.paths.image / "default_tu1.xex", "other-image");
  CHECK(make_shaders_stage(f.cfg)->fingerprint(ctx).value() != base.value());
}

TEST_CASE("stage order includes shaders between recompile and build", "[unit]") {
  SystemDoctorProbe probe;
  auto stages = default_stages(probe);
  std::vector<StageId> ids;
  for (auto& s : stages) ids.push_back(s->id());
  const std::vector<StageId> expected{StageId::Doctor,    StageId::Extract,   StageId::Verify, StageId::Patch,
                                      StageId::Analyse,   StageId::Recompile, StageId::Shaders, StageId::Build};
  CHECK(ids == expected);
  CHECK(stage_name(StageId::Shaders) == "shaders");
  CHECK(parse_stage("shaders") == StageId::Shaders);
}

TEST_CASE("parse_translated_line reads the tool summary", "[unit]") {
  CHECK(parse_translated_line("translated 12 of 34") == std::make_pair(12u, 34u));
  CHECK(parse_translated_line("translated 0 of 0\r") == std::make_pair(0u, 0u));
  CHECK_FALSE(parse_translated_line("translated 1 of").has_value());
  CHECK_FALSE(parse_translated_line("translated x of 2").has_value());
  CHECK_FALSE(parse_translated_line("container 0x1 translated 1 of 2").has_value());
}
