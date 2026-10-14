// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "common/sha256.h"
#include "pipeline/stages/extract_stage.h"
#include "pipeline/stages/verify_stage.h"
#include "support/stfs_builder.h"
#include "support/xdvdfs_builder.h"

using namespace lr;
using namespace lr::pipeline;
namespace fs = std::filesystem;

namespace {

std::vector<std::byte> bytes(const std::string& s) {
  std::vector<std::byte> v;
  for (char c : s) v.push_back(std::byte(c));
  return v;
}
std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}

struct Sink : ProgressSink {
  std::vector<float> fractions;
  int cancel_after_calls = -1;
  int calls = 0;
  void on_progress(const Progress& p) override { fractions.push_back(p.fraction); }
  void on_log(std::string_view) override {}
  bool cancelled() const override { return cancel_after_calls >= 0 && ++const_cast<Sink*>(this)->calls > cancel_after_calls; }
};

struct Fixture {
  fs::path dir = fs::temp_directory_path() / "lr_extract_test";
  fs::path iso = dir / "game.iso";
  Options opts;
  GameConfig game{};
  Sink sink;
  Fixture() {
    fs::remove_all(dir);
    fs::create_directories(dir / "cfg");
    std::ofstream(dir / "cfg" / "game.toml") << "title_id = 1\n";
    game.package = "Content/pkg";
    game.title_update = "tu/tu1";
    game.package_xex = "default.xex";
    game.tu_xexp = "default.xexp";
    game.recomp_toml = dir / "cfg" / "recomp.toml";
    opts.iso = iso;
    opts.root = dir / "data";
  }
  ~Fixture() { fs::remove_all(dir); }
  void write_iso(const std::vector<std::byte>& pkg, const std::vector<std::byte>& tu) {
    auto img = testing::XdvdfsBuilder().add("Content/pkg", pkg).add("tu/tu1", tu).build();
    std::ofstream f(iso, std::ios::binary);
    std::vector<std::byte> head(img.partition_offset);
    f.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
    f.write(reinterpret_cast<const char*>(img.data.data()), static_cast<std::streamsize>(img.data.size()));
  }
  StageContext ctx() { return StageContext{opts, DataPaths::from_root(opts.root), dir, game, sink}; }
};

}  // namespace

TEST_CASE("extract writes xex, game files and overlays TU", "[unit]") {
  Fixture f;
  testing::StfsBuilder pkg, tu;
  pkg.add_file("default.xex", bytes("XEXDATA"));
  pkg.add_dir("data");
  pkg.add_file("x.bin", bytes("PKGX"), 1);
  pkg.add_file("keep.txt", bytes("keep"));
  tu.add_file("default.xexp", bytes("PATCH"));
  tu.add_file("extra.txt", bytes("extra"));
  tu.add_file("keep.txt", bytes("overlaid"));
  f.write_iso(pkg.build(), tu.build());

  auto stage = make_extract_stage();
  auto c = f.ctx();
  REQUIRE(stage->fingerprint(c));
  auto r = stage->execute(c);
  REQUIRE(r);
  CHECK(slurp(c.paths.input / "package.xex") == "XEXDATA");
  CHECK(slurp(c.paths.input / "tu.xexp") == "PATCH");
  CHECK(slurp(c.paths.game / "data" / "x.bin") == "PKGX");
  CHECK(slurp(c.paths.game / "extra.txt") == "extra");
  CHECK(slurp(c.paths.game / "keep.txt") == "overlaid");
  CHECK(fs::exists(c.paths.game / ".extracted"));
  CHECK_FALSE(fs::exists(c.paths.game / "default.xex"));
  CHECK(f.sink.fractions.back() == 1.0f);
  for (auto p : stage->outputs(c)) CHECK(fs::exists(p));
}

TEST_CASE("extract fingerprint changes with the iso", "[unit]") {
  Fixture f;
  testing::StfsBuilder pkg, tu;
  pkg.add_file("default.xex", bytes("A"));
  tu.add_file("default.xexp", bytes("B"));
  f.write_iso(pkg.build(), tu.build());
  auto stage = make_extract_stage();
  auto c = f.ctx();
  auto a = stage->fingerprint(c);
  REQUIRE(a);
  CHECK(stage->fingerprint(c).value() == a.value());
  pkg.add_file("more", bytes("zzz"));
  f.write_iso(pkg.build(), tu.build());
  CHECK(stage->fingerprint(c).value() != a.value());
}

TEST_CASE("extract rejects paths escaping game/", "[unit]") {
  Fixture f;
  testing::StfsBuilder pkg, tu;
  pkg.add_file("default.xex", bytes("A"));
  pkg.add_file("..", bytes("evil"));
  tu.add_file("default.xexp", bytes("B"));
  f.write_iso(pkg.build(), tu.build());
  auto c = f.ctx();
  auto r = make_extract_stage()->execute(c);
  REQUIRE_FALSE(r);
  CHECK(r.error().code == ErrorCode::InvalidFormat);
}

TEST_CASE("extract honours cancellation and leaves no marker", "[unit]") {
  Fixture f;
  testing::StfsBuilder pkg, tu;
  pkg.add_file("default.xex", bytes("A"));
  pkg.add_file("b", bytes("B"));
  tu.add_file("default.xexp", bytes("B"));
  f.write_iso(pkg.build(), tu.build());
  f.sink.cancel_after_calls = 2;
  auto c = f.ctx();
  auto r = make_extract_stage()->execute(c);
  REQUIRE_FALSE(r);
  CHECK(r.error().code == ErrorCode::Cancelled);
  CHECK_FALSE(fs::exists(c.paths.game / ".extracted"));
}

TEST_CASE("extract fails when the xex is missing", "[unit]") {
  Fixture f;
  testing::StfsBuilder pkg, tu;
  pkg.add_file("other", bytes("A"));
  tu.add_file("default.xexp", bytes("B"));
  f.write_iso(pkg.build(), tu.build());
  auto c = f.ctx();
  auto r = make_extract_stage()->execute(c);
  REQUIRE_FALSE(r);
  CHECK(r.error().code == ErrorCode::NotFound);
}

namespace {
struct VerifyFixture {
  fs::path dir = fs::temp_directory_path() / "lr_verify_test";
  Options opts;
  GameConfig game{};
  Sink sink;
  VerifyFixture() {
    fs::remove_all(dir);
    fs::create_directories(dir / "work" / "input");
    std::ofstream(dir / "work" / "input" / "package.xex") << "xex";
    std::ofstream(dir / "work" / "input" / "tu.xexp") << "xexp";
    opts.root = dir;
  }
  ~VerifyFixture() { fs::remove_all(dir); }
  StageContext ctx() { return StageContext{opts, DataPaths::from_root(dir), dir, game, sink}; }
};
}  // namespace

TEST_CASE("verify accepts matching hashes", "[unit]") {
  VerifyFixture f;
  auto c = f.ctx();
  f.game.hash_package_xex = sha256_file(c.paths.input / "package.xex").value();
  f.game.hash_tu_xexp = sha256_file(c.paths.input / "tu.xexp").value();
  auto stage = make_verify_stage();
  CHECK(stage->fingerprint(c));
  CHECK(stage->execute(c));
}

TEST_CASE("verify rejects mismatched hash", "[unit]") {
  VerifyFixture f;
  auto c = f.ctx();
  f.game.hash_package_xex = std::string(64, '0');
  f.game.hash_tu_xexp = sha256_file(c.paths.input / "tu.xexp").value();
  auto found = sha256_file(c.paths.input / "package.xex").value();
  auto r = make_verify_stage()->execute(c);
  REQUIRE_FALSE(r);
  CHECK(r.error().code == ErrorCode::UnsupportedVersion);
  CHECK(r.error().message.find(found) != std::string::npos);
  CHECK(r.error().message.find("package.xex") != std::string::npos);
  CHECK(r.error().hint.find("584111F7") != std::string::npos);
}

TEST_CASE("verify with no configured hash is an internal error", "[unit]") {
  VerifyFixture f;
  auto c = f.ctx();
  auto r = make_verify_stage()->execute(c);
  REQUIRE_FALSE(r);
  CHECK(r.error().code == ErrorCode::Internal);
}

TEST_CASE("verify fingerprint depends on expected hashes", "[unit]") {
  VerifyFixture f;
  auto c = f.ctx();
  f.game.hash_package_xex = "aa";
  f.game.hash_tu_xexp = "bb";
  auto stage = make_verify_stage();
  auto a = stage->fingerprint(c).value();
  f.game.hash_tu_xexp = "cc";
  CHECK(stage->fingerprint(c).value() != a);
}

TEST_CASE("extract clears stale game/ contents", "[unit]") {
  Fixture f;
  testing::StfsBuilder pkg, tu;
  pkg.add_file("default.xex", bytes("A"));
  tu.add_file("default.xexp", bytes("B"));
  f.write_iso(pkg.build(), tu.build());
  auto c = f.ctx();
  fs::create_directories(c.paths.game / "old");
  std::ofstream(c.paths.game / "stale.txt") << "x";
  std::ofstream(c.paths.game / "old" / "s2") << "x";
  REQUIRE(make_extract_stage()->execute(c));
  CHECK_FALSE(fs::exists(c.paths.game / "stale.txt"));
  CHECK_FALSE(fs::exists(c.paths.game / "old"));
  CHECK(fs::exists(c.paths.game / ".extracted"));
}

TEST_CASE("extract validates all paths before writing anything", "[unit]") {
  Fixture f;
  testing::StfsBuilder pkg, tu;
  pkg.add_file("default.xex", bytes("A"));
  pkg.add_file("good.txt", bytes("g"));
  pkg.add_file("..", bytes("evil"));
  tu.add_file("default.xexp", bytes("B"));
  f.write_iso(pkg.build(), tu.build());
  auto c = f.ctx();
  auto r = make_extract_stage()->execute(c);
  REQUIRE_FALSE(r);
  CHECK(r.error().code == ErrorCode::InvalidFormat);
  CHECK_FALSE(fs::exists(c.paths.game / "good.txt"));
  CHECK_FALSE(fs::exists(c.paths.input / "package.xex"));
}
