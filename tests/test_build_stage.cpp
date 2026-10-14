// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <stdlib.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "pipeline/pipeline.h"
#include "pipeline/stages/build_stage.h"

using namespace lr;
using namespace lr::pipeline;
namespace fs = std::filesystem;

namespace {

struct TempDir {
  fs::path path;
  TempDir() {
    std::string t = (fs::temp_directory_path() / "lr_build_stage_XXXXXX").string();
    REQUIRE(mkdtemp(t.data()) != nullptr);
    path = t;
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
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
