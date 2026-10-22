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

  explicit Fixture(const std::string& summary = "translated 3 of 3", int exit_code = 0,
                   const std::string& cache_text = "extern const size_t lr_shader_blob_uncompressed_size = 5;",
                   const std::string& extra_lines = "") {
    opts.root = root;
    cfg.xenosrecomp_commit = "aaaa+patches:1111";
    cfg.dxc_version = [](const fs::path&) { return std::string("dxc 1.8"); };
    write(root / "work" / "image" / "default_tu1.xex", "image-bytes");
    write(res / "tools" / "xenos_shader_common.h", "// prelude");
    const fs::path tool = res / "tools" / "XenosRecomp";
    write(tool, "#!/bin/sh\n"
                "{ for a in \"$@\"; do echo \"$a\"; done; pwd; } > \"" + (tmp.path / "argv.txt").string() + "\"\n"
                "echo \"container 0x1 microcode 0x2 pixel literal-loops 0\"\n"
                "echo \"container 0x3 microcode 0x4 vertex literal-loops 2\"\n" +
                extra_lines +
                "echo \"" + summary + "\"\n"
                "echo \"container 0x5 microcode 0x6 pixel literal-loops 1\"\n"
                "printf '%s\\n' \"" + cache_text + "\" > \"$2\"\n"
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

  const fs::path gen = ctx.paths.work / "shaders" / "shader_cache.cpp";
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
    CHECK_FALSE(fs::exists(ctx.paths.work / "shaders" / "shader_cache.cpp"));
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

TEST_CASE("a truncated or empty cache file fails the stage", "[unit]") {
  for (const char* text : {"", "extern const uint8_t lr_shader_blob[] = {1,2,3,"}) {
    Fixture f("translated 3 of 3", 0, text);
    auto ctx = f.ctx();
    auto r = make_shaders_stage(f.cfg)->execute(ctx);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::ToolFailed);
    CHECK(r.error().message.find("truncated") != std::string::npos);
    CHECK_FALSE(fs::exists(ctx.paths.work / "shaders" / "shader_cache.cpp"));
  }
}

TEST_CASE("literal-loops lines around the summary do not confuse the stage", "[unit]") {
  Fixture f("translated 3 of 3", 0, "extern const size_t lr_shader_blob_uncompressed_size = 5;",
            "echo \"container 0x7 microcode 0x8 vertex literal-loops 12\"\n");
  auto ctx = f.ctx();
  REQUIRE(make_shaders_stage(f.cfg)->execute(ctx));
  CHECK(f.sink.messages.size() == 1);
  CHECK(f.sink.messages.back() == "translated 3 of 3");
  const std::string log = slurp(ctx.paths.logs / "shaders.log");
  CHECK(log.find("literal-loops 12") != std::string::npos);
  CHECK(log.find("literal-loops 1\n") != std::string::npos);
}

TEST_CASE("query_dxc_version reads stdout or stderr", "[unit]") {
  lr::testing::TempDir tmp{"lr_fake_dxc"};
  const fs::path err = tmp.path / "dxc_err", out = tmp.path / "dxc_out", bad = tmp.path / "dxc_bad";
  write(err, "#!/bin/sh\necho \"dxcompiler.so: 1.8 (stderr)\" >&2\nexit 0\n");
  write(out, "#!/bin/sh\necho \"1.7 (stdout)\"\nexit 0\n");
  write(bad, "#!/bin/sh\nexit 3\n");
  for (const auto& p : {err, out, bad}) ::chmod(p.c_str(), 0755);
  CHECK(query_dxc_version(err) == "dxcompiler.so: 1.8 (stderr)");
  CHECK(query_dxc_version(out) == "1.7 (stdout)");
  CHECK(query_dxc_version(bad) == "unknown");
  CHECK(query_dxc_version(tmp.path / "missing") == "unknown");
}

TEST_CASE("shaders stage is independent of the stages before it", "[unit]") {
  Fixture f;
  CHECK(make_shaders_stage(f.cfg)->independent());
}
