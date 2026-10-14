// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <map>

#include "pipeline/doctor.h"
#include "pipeline/stages/stages.h"

using namespace lr;
using namespace lr::pipeline;
namespace fs = std::filesystem;

namespace {

constexpr uint64_t kGiB = 1024ull * 1024 * 1024;

struct FakeProbe : DoctorProbe {
  std::map<std::string, std::string> tools{{"clang++", "clang version 18.1.8"},
                                           {"cmake", "cmake version 3.28.3"},
                                           {"ninja", "1.11.1"}};
  uint64_t free = 100 * kGiB, ram = 16 * kGiB;
  bool sdl2 = true;
  fs::path asked;
  std::optional<fs::path> find(std::string_view n) override {
    auto it = tools.find(std::string(n));
    if (it == tools.end()) return std::nullopt;
    return fs::path("/usr/bin") / it->first;
  }
  Result<std::string> version_output(const fs::path& p) override { return tools.at(p.filename().string()); }
  Result<uint64_t> free_bytes(const fs::path& p) override {
    asked = p;
    return free;
  }
  uint64_t total_ram() override { return ram; }
  bool pkg_config_exists(std::string_view) override { return sdl2; }
  bool avx = true;
  bool cpu_supports_avx() override { return avx; }
};

}  // namespace

TEST_CASE("doctor passes with a healthy environment", "[unit]") {
  FakeProbe p;
  auto r = run_doctor(p, "/nonexistent_lr_root/a/b");
  REQUIRE(r);
  CHECK(r.value().warnings.empty());
  CHECK(fs::exists(p.asked));  // probed the nearest existing parent
}

TEST_CASE("doctor reports missing clang with pacman hint", "[unit]") {
  FakeProbe p;
  p.tools.erase("clang++");
  auto r = run_doctor(p, "/tmp");
  REQUIRE_FALSE(r);
  CHECK(r.error().code == ErrorCode::ToolMissing);
  CHECK(r.error().hint.find("pacman -S --needed clang") != std::string::npos);
  CHECK(r.error().hint.find("apt") != std::string::npos);
  CHECK(r.error().hint.find("dnf") != std::string::npos);
}

TEST_CASE("doctor rejects old clang and cmake", "[unit]") {
  FakeProbe p;
  p.tools["clang++"] = "clang version 17.0.6";
  auto r = run_doctor(p, "/tmp");
  REQUIRE_FALSE(r);
  CHECK(r.error().code == ErrorCode::ToolMissing);
  CHECK(r.error().message.find("18") != std::string::npos);
  p.tools["clang++"] = "clang version 18.0.0";
  p.tools["cmake"] = "cmake version 3.24.9";
  r = run_doctor(p, "/tmp");
  REQUIRE_FALSE(r);
  CHECK(r.error().message.find("3.25") != std::string::npos);
}

TEST_CASE("doctor requires ninja and sdl2", "[unit]") {
  FakeProbe p;
  p.tools.erase("ninja");
  CHECK(run_doctor(p, "/tmp").error().code == ErrorCode::ToolMissing);
  FakeProbe q;
  q.sdl2 = false;
  CHECK(run_doctor(q, "/tmp").error().code == ErrorCode::ToolMissing);
}

TEST_CASE("doctor fails below 8 GiB free", "[unit]") {
  FakeProbe p;
  p.free = 8 * kGiB - 1;
  auto r = run_doctor(p, "/tmp");
  REQUIRE_FALSE(r);
  CHECK(r.error().code == ErrorCode::InsufficientResources);
  p.free = 8 * kGiB;
  CHECK(run_doctor(p, "/tmp"));
}

TEST_CASE("doctor warns on low RAM", "[unit]") {
  FakeProbe p;
  p.ram = 4 * kGiB;
  auto r = run_doctor(p, "/tmp");
  REQUIRE(r);
  CHECK(r.value().warnings.size() == 1);
}

TEST_CASE("doctor survives absurd or foreign version strings", "[unit]") {
  FakeProbe p;
  p.tools["clang++"] = "clang version 99999999999999999999.1";
  auto r = run_doctor(p, "/tmp");
  REQUIRE_FALSE(r);
  CHECK(r.error().code == ErrorCode::ToolMissing);
  p.tools["clang++"] = "Apple clang version 15.0.0";
  r = run_doctor(p, "/tmp");
  REQUIRE_FALSE(r);
  CHECK(r.error().code == ErrorCode::ToolMissing);
}

TEST_CASE("doctor fails on a CPU without AVX", "[unit]") {
  FakeProbe p;
  p.avx = false;
  auto r = run_doctor(p, "/tmp");
  REQUIRE_FALSE(r);
  CHECK(r.error().code == ErrorCode::InsufficientResources);
  CHECK(r.error().message == "CPU without AVX support");
  CHECK(r.error().hint.find("-march=sandybridge") != std::string::npos);
}

TEST_CASE("doctor stage fingerprint is stable so a finished install stays cached", "[unit]") {
  struct NullSink : ProgressSink {
    void on_progress(const Progress&) override {}
    void on_log(std::string_view) override {}
    bool cancelled() const override { return false; }
  } sink;
  FakeProbe p;
  Options opts;
  GameConfig game;
  StageContext ctx{opts, DataPaths::from_root("/nonexistent"), "/nonexistent", game, sink};
  auto stage = make_doctor_stage(p);
  auto a = stage->fingerprint(ctx);
  auto b = stage->fingerprint(ctx);
  REQUIRE(a);
  REQUIRE(b);
  CHECK(a.value() == b.value());
  CHECK(stage->outputs(ctx).empty());
}
