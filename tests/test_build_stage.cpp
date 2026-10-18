// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "pipeline/pipeline.h"
#include "pipeline/stages/build_stage.h"
#include "support/temp_dir.h"

using namespace lr;
using namespace lr::pipeline;
namespace fs = std::filesystem;

namespace {

struct TempDir : lr::testing::TempDir {
  TempDir() : lr::testing::TempDir("lr_build_stage") {}
};

void write(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << text;
}

}  // namespace

TEST_CASE("parse_ninja_progress reads the [n/N] status prefix", "[unit]") {
  CHECK(parse_ninja_progress("[3/12] Building CXX object x.o") == 0.25f);
  CHECK(parse_ninja_progress("[12/12] Linking CXX executable legacyrecomp-game") == 1.0f);
  CHECK_FALSE(parse_ninja_progress("-- Configuring done").has_value());
  CHECK_FALSE(parse_ninja_progress("[a/b] x").has_value());
  CHECK_FALSE(parse_ninja_progress("[1/0] x").has_value());
  CHECK_FALSE(parse_ninja_progress("[1 of 2] x").has_value());
  CHECK_FALSE(parse_ninja_progress("").has_value());
}

TEST_CASE("build_jobs honours --jobs, else min(threads, RAM / 2 GiB)", "[unit]") {
  constexpr uint64_t GiB = 1ull << 30;
  CHECK(build_jobs(4, 12, 19 * GiB) == 4);
  CHECK(build_jobs(0, 12, 19 * GiB) == 9);
  CHECK(build_jobs(0, 4, 64 * GiB) == 4);
  CHECK(build_jobs(0, 0, 1 * GiB) == 1);
}

TEST_CASE("build_fingerprint tracks generated listing and runtime contents, not import_stubs.cpp", "[unit]") {
  TempDir t;
  const fs::path gen = t.path / "generated", rt = t.path / "runtime";
  write(gen / "ppc_recomp.0.cpp", "int a;");
  write(gen / "ppc_config.h", "#pragma once");
  write(rt / "lib" / "liblr_runtime_core.a", "lib-v1");
  write(rt / "src" / "main.cpp", "int main() {}");

  auto base = build_fingerprint(gen, rt);
  REQUIRE(base);

  write(gen / "import_stubs.cpp", "// stubs");  // written by the build stage itself
  write(gen / "import_stubs.cpp.tmp", "// partial");
  CHECK(build_fingerprint(gen, rt).value() == base.value());

  write(gen / "ppc_recomp.0.cpp", "int ab;");  // size change
  auto grown = build_fingerprint(gen, rt);
  REQUIRE(grown);
  CHECK(grown.value() != base.value());

  write(rt / "lib" / "liblr_runtime_core.a", "lib-v2");  // same size, new contents
  auto relib = build_fingerprint(gen, rt);
  REQUIRE(relib);
  CHECK(relib.value() != grown.value());
}

TEST_CASE("build_fingerprint fails without generated sources or runtime", "[unit]") {
  TempDir t;
  auto r = build_fingerprint(t.path / "missing", t.path);
  REQUIRE_FALSE(r);
  CHECK(r.error().code == ErrorCode::NotFound);

  fs::create_directories(t.path / "generated");
  fs::create_directories(t.path / "runtime");
  auto e = build_fingerprint(t.path / "generated", t.path / "runtime");
  REQUIRE_FALSE(e);
  CHECK(e.error().code == ErrorCode::NotFound);
}

TEST_CASE("reset_stale_build_dir wipes a build dir configured from another template", "[unit]") {
  TempDir t;
  const fs::path tmpl = t.path / "share" / "runtime" / "template", build = t.path / "work" / "build";
  fs::create_directories(tmpl);

  auto none = reset_stale_build_dir(build, tmpl);  // never configured
  REQUIRE(none);
  CHECK_FALSE(none.value());

  const std::string good_cache = "# cache\nCMAKE_CACHEFILE_DIR:INTERNAL=" + build.string() +
                                 "\nCMAKE_HOME_DIRECTORY:INTERNAL=" + tmpl.string() + "\nFOO:BOOL=ON\n";
  write(build / "CMakeCache.txt", good_cache);
  write(build / "build.ninja", "x");
  auto same = reset_stale_build_dir(build, tmpl / ".");
  REQUIRE(same);
  CHECK_FALSE(same.value());
  CHECK(fs::exists(build / "build.ninja"));

  // The data root moved: same template, but the cache was written for another build dir.
  write(build / "CMakeCache.txt", "CMAKE_CACHEFILE_DIR:INTERNAL=/old/root/work/build\nCMAKE_HOME_DIRECTORY:INTERNAL=" +
                                      tmpl.string() + "\n");
  auto root_moved = reset_stale_build_dir(build, tmpl);
  REQUIRE(root_moved);
  CHECK(root_moved.value());
  CHECK_FALSE(fs::exists(build));

  write(build / "CMakeCache.txt", "CMAKE_HOME_DIRECTORY:INTERNAL=" + tmpl.string() + "\n");  // no CACHEFILE_DIR
  auto no_cachefile_dir = reset_stale_build_dir(build, tmpl);
  REQUIRE(no_cachefile_dir);
  CHECK(no_cachefile_dir.value());
  CHECK_FALSE(fs::exists(build));

  write(build / "CMakeCache.txt", "CMAKE_HOME_DIRECTORY:INTERNAL=/old/prefix/share/legacyrecomp/runtime/template\n");
  auto moved = reset_stale_build_dir(build, tmpl);
  REQUIRE(moved);
  CHECK(moved.value());
  CHECK_FALSE(fs::exists(build));

  write(build / "CMakeCache.txt", "garbage without the key\n");
  auto corrupt = reset_stale_build_dir(build, tmpl);
  REQUIRE(corrupt);
  CHECK(corrupt.value());
  CHECK_FALSE(fs::exists(build));
}

TEST_CASE("default_stages lists every stage in order", "[unit]") {
  SystemDoctorProbe probe;
  auto stages = default_stages(probe);
  REQUIRE(stages.size() == 7);
  for (size_t i = 0; i < stages.size(); ++i) CHECK(static_cast<size_t>(stages[i]->id()) == i);
}

TEST_CASE("launch reports a missing game build", "[unit]") {
  TempDir t;
  Options opts;
  opts.root = t.path;
  auto r = launch(opts, {});
  REQUIRE_FALSE(r);
  CHECK(r.error().code == ErrorCode::NotFound);
  CHECK(r.error().message.find("legacyrecomp-game") != std::string::npos);
}

TEST_CASE("launch forwards Ctrl+C to the game, survives it and returns 128 + SIGINT", "[unit]") {
  TempDir t;
  write(t.path / "work" / "image" / "default_tu1.xex", "image");
  // Stand-in game: interrupts its parent (the launcher must ignore that) and then itself.
  const fs::path game = t.path / "bin" / "legacyrecomp-game";
  write(game, "#!/bin/sh\nkill -INT $PPID\nkill -INT $$\nsleep 5\nexit 0\n");
  fs::permissions(game, fs::perms::owner_all);
  Options opts;
  opts.root = t.path;
  auto r = launch(opts, {});
  REQUIRE(r);
  CHECK(r.value() == 130);
}

TEST_CASE("launch passes --trace-file <root>/work/logs/game-trace.log to the game", "[unit][trace]") {
  TempDir t;
  write(t.path / "work" / "image" / "default_tu1.xex", "image");
  // Stand-in game: records its arguments, one per line.
  const fs::path game = t.path / "bin" / "legacyrecomp-game";
  const fs::path args = t.path / "args.txt";
  write(game, "#!/bin/sh\nfor a in \"$@\"; do echo \"$a\"; done > '" + args.string() + "'\nexit 0\n");
  fs::permissions(game, fs::perms::owner_all);
  Options opts;
  opts.root = t.path;
  const std::vector<std::string> extra = {"--trace-imports"};
  auto r = launch(opts, extra);
  REQUIRE(r);
  CHECK(r.value() == 0);
  std::ifstream f(args);
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string expect_file = (t.path / "work" / "logs" / "game-trace.log").string();
  CHECK(ss.str().find("--trace-file\n" + expect_file + "\n") != std::string::npos);
  CHECK(ss.str().find("--trace-imports\n") != std::string::npos);  // extra arguments still reach the game
}

TEST_CASE("launch passes --data <root>/game and --user <root>/user and creates user/", "[unit][vfs]") {
  TempDir t;
  write(t.path / "work" / "image" / "default_tu1.xex", "image");
  const fs::path game = t.path / "bin" / "legacyrecomp-game";
  const fs::path args = t.path / "args.txt";
  write(game, "#!/bin/sh\nfor a in \"$@\"; do echo \"$a\"; done > '" + args.string() + "'\nexit 0\n");
  fs::permissions(game, fs::perms::owner_all);
  Options opts;
  opts.root = t.path;
  REQUIRE(launch(opts, {}));
  std::ifstream f(args);
  std::stringstream ss;
  ss << f.rdbuf();
  CHECK(ss.str().find("--data\n" + (t.path / "game").string() + "\n") != std::string::npos);
  CHECK(ss.str().find("--user\n" + (t.path / "user").string() + "\n") != std::string::npos);
  CHECK(fs::is_directory(t.path / "user"));
}
