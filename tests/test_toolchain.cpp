// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "pipeline/doctor.h"
#include "pipeline/toolchain.h"
#include "support/temp_dir.h"

using namespace lr;
using namespace lr::pipeline;
namespace fs = std::filesystem;

namespace {

struct TempDir : lr::testing::TempDir {
  TempDir() : lr::testing::TempDir("lr_toolchain") {}
};

void write_exe(const fs::path& p) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << "#!/bin/sh\nexit 0\n";
  fs::permissions(p, fs::perms::owner_all);
}

// The layout packaging/appimage/build.sh produces under share/legacyrecomp/toolchain/.
void make_bundled_tree(const fs::path& resources) {
  const fs::path tc = resources / "toolchain";
  for (const char* f : {"bin/cc", "bin/c++", "bin/ninja", "cmake/bin/cmake", "zig/zig"}) write_exe(tc / f);
  fs::create_directories(tc / "sdl2" / "lib" / "cmake" / "SDL2");
  std::ofstream(tc / "sdl2" / "lib" / "cmake" / "SDL2" / "SDL2Config.cmake") << "# fake\n";
}

// Sets PATH for the lifetime of the object.
struct ScopedPath {
  std::optional<std::string> old;
  explicit ScopedPath(const std::string& value) {
    if (const char* p = std::getenv("PATH")) old = p;
    ::setenv("PATH", value.c_str(), 1);
  }
  ~ScopedPath() {
    if (old) ::setenv("PATH", old->c_str(), 1);
    else ::unsetenv("PATH");
  }
};

struct HealthyProbe : DoctorProbe {
  std::optional<fs::path> find(std::string_view) override { return std::nullopt; }  // nothing on the system PATH
  Result<std::string> version_output(const fs::path&) override {
    return Error{ErrorCode::ToolMissing, "unexpected", ""};
  }
  Result<uint64_t> free_bytes(const fs::path&) override { return 100ull << 30; }
  uint64_t total_ram() override { return 16ull << 30; }
  bool pkg_config_exists(std::string_view) override { return false; }
  bool cpu_supports_avx() override { return true; }
};

}  // namespace

TEST_CASE("resolve_toolchain uses the bundled toolchain when resources/toolchain exists", "[unit]") {
  TempDir t;
  make_bundled_tree(t.path);
  auto r = resolve_toolchain(t.path);
  REQUIRE(r);
  const Toolchain& tc = r.value();
  const fs::path base = t.path / "toolchain";
  CHECK(tc.bundled);
  CHECK(tc.cc == base / "bin" / "cc");
  CHECK(tc.cxx == base / "bin" / "c++");
  CHECK(tc.cmake == base / "cmake" / "bin" / "cmake");
  CHECK(tc.ninja == base / "bin" / "ninja");
  REQUIRE(tc.sdl2_prefix.has_value());
  CHECK(*tc.sdl2_prefix == base / "sdl2");
}

TEST_CASE("resolve_toolchain falls back to clang/cmake/ninja on PATH", "[unit]") {
  TempDir t;
  const fs::path bin = t.path / "fakebin";
  for (const char* f : {"clang", "clang++", "cmake", "ninja"}) write_exe(bin / f);
  ScopedPath path(bin.string());
  auto r = resolve_toolchain(t.path / "resources-without-toolchain");
  REQUIRE(r);
  const Toolchain& tc = r.value();
  CHECK_FALSE(tc.bundled);
  CHECK(tc.cc == bin / "clang");
  CHECK(tc.cxx == bin / "clang++");
  CHECK(tc.cmake == bin / "cmake");
  CHECK(tc.ninja == bin / "ninja");
  CHECK_FALSE(tc.sdl2_prefix.has_value());
}

TEST_CASE("resolve_toolchain reports a system tool missing from PATH with the install hint", "[unit]") {
  TempDir t;
  const fs::path bin = t.path / "fakebin";
  for (const char* f : {"clang", "clang++", "ninja"}) write_exe(bin / f);
  ScopedPath path(bin.string());
  auto r = resolve_toolchain(t.path);
  REQUIRE_FALSE(r);
  CHECK(r.error().code == ErrorCode::ToolMissing);
  CHECK(r.error().message.find("cmake") != std::string::npos);
  CHECK(r.error().hint.find("pacman") != std::string::npos);
}

TEST_CASE("resolve_toolchain reports a missing bundled file as a corrupted AppImage", "[unit]") {
  TempDir t;
  make_bundled_tree(t.path);
  fs::remove(t.path / "toolchain" / "zig" / "zig");
  auto r = resolve_toolchain(t.path);
  REQUIRE_FALSE(r);
  CHECK(r.error().code == ErrorCode::ToolMissing);
  CHECK(r.error().message.find("zig") != std::string::npos);
  CHECK(r.error().hint.find("AppImage") != std::string::npos);
  CHECK(r.error().hint.find("corrupted") != std::string::npos);
}

TEST_CASE("doctor in bundled mode checks the bundled files, not the system PATH or pkg-config", "[unit]") {
  TempDir t;
  make_bundled_tree(t.path);
  HealthyProbe probe;
  auto ok = run_doctor(probe, t.path / "root", t.path);
  REQUIRE(ok);

  fs::remove(t.path / "toolchain" / "sdl2" / "lib" / "cmake" / "SDL2" / "SDL2Config.cmake");
  auto r = run_doctor(probe, t.path / "root", t.path);
  REQUIRE_FALSE(r);
  CHECK(r.error().code == ErrorCode::ToolMissing);
  CHECK(r.error().hint.find("corrupted") != std::string::npos);
  CHECK(r.error().hint.find("pacman") == std::string::npos);
}

TEST_CASE("doctor in bundled mode still refuses a CPU without AVX", "[unit]") {
  TempDir t;
  make_bundled_tree(t.path);
  struct NoAvx : HealthyProbe {
    bool cpu_supports_avx() override { return false; }
  } probe;
  auto r = run_doctor(probe, t.path / "root", t.path);
  REQUIRE_FALSE(r);
  CHECK(r.error().code == ErrorCode::InsufficientResources);
}
