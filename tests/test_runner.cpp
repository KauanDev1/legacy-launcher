// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <array>
#include <vector>

#include "pipeline/runner.h"
#include "pipeline/stage.h"
#include "pipeline/state.h"
#include "support/temp_dir.h"

using namespace lr;
using namespace lr::pipeline;
namespace fs = std::filesystem;

namespace {

struct RecSink : ProgressSink {
  std::vector<Progress> events;
  std::optional<StageId> cancel_after;  // cancelled() turns true once this stage reported "done"
  void on_progress(const Progress& p) override {
    events.push_back(p);
    if (cancel_after && p.stage == *cancel_after && p.message == "done") cancelled_now = true;
  }
  void on_log(std::string_view) override {}
  bool cancelled() const override { return cancelled_now; }
  bool cancelled_now = false;
  int count(const std::string& msg) const {
    int n = 0;
    for (auto& e : events) n += e.message == msg;
    return n;
  }
};

struct FakeStage : Stage {
  FakeStage(StageId i, fs::path out) : id_(i), out_(std::move(out)) {}
  StageId id() const override { return id_; }
  Result<std::string> fingerprint(StageContext&) override { return fp; }
  std::vector<fs::path> outputs(StageContext&) const override { return {out_}; }
  Result<void> execute(StageContext&) override {
    ++runs;
    if (fail) return Error{ErrorCode::ToolFailed, "boom", ""};
    std::ofstream(out_) << "x";
    return {};
  }
  StageId id_;
  fs::path out_;
  std::string fp = "a";
  bool fail = false;
  int runs = 0;
};

struct Fixture {
  lr::testing::TempDir tmp{"lr_runner_test"};
  fs::path dir = tmp.path;
  Options opts;
  GameConfig game{};
  RecSink sink;
  FakeStage s1{StageId::Doctor, dir / "o1"}, s2{StageId::Extract, dir / "o2"},
      s3{StageId::Verify, dir / "o3"};
  Stage* stages[3] = {&s1, &s2, &s3};
  Fixture() { opts.root = dir; }
  Result<void> run() {
    auto st = StateStore::load(dir / "state.toml");
    REQUIRE(st);
    StageContext ctx{opts, DataPaths::from_root(dir), dir, game, sink};
    return run_stages(stages, ctx, st.value());
  }
  std::array<int, 3> runs() const { return {s1.runs, s2.runs, s3.runs}; }
};

}  // namespace

TEST_CASE("stage names round trip", "[unit]") {
  CHECK(stage_name(StageId::Doctor) == "doctor");
  CHECK(parse_stage("recompile") == StageId::Recompile);
  CHECK_FALSE(parse_stage("nope"));
}

TEST_CASE("runner caches completed stages", "[unit]") {
  Fixture f;
  REQUIRE(f.run());
  CHECK(f.runs() == std::array<int, 3>{1, 1, 1});
  CHECK(f.sink.count("start") == 3);
  CHECK(f.sink.count("done") == 3);
  f.sink.events.clear();
  REQUIRE(f.run());
  CHECK(f.runs() == std::array<int, 3>{1, 1, 1});
  CHECK(f.sink.count("cached") == 3);
  CHECK(f.sink.count("start") == 0);
  CHECK(f.sink.events.size() == 3);
}

TEST_CASE("changed fingerprint re-executes later stages", "[unit]") {
  Fixture f;
  REQUIRE(f.run());
  f.s2.fp = "b";
  REQUIRE(f.run());
  CHECK(f.runs() == std::array<int, 3>{1, 2, 2});
}

TEST_CASE("missing output re-executes everything after", "[unit]") {
  Fixture f;
  REQUIRE(f.run());
  fs::remove(f.dir / "o1");
  REQUIRE(f.run());
  CHECK(f.runs() == std::array<int, 3>{2, 2, 2});
}

TEST_CASE("force re-executes the stage and later ones", "[unit]") {
  Fixture f;
  REQUIRE(f.run());
  f.opts.force = {StageId::Extract};
  REQUIRE(f.run());
  CHECK(f.runs() == std::array<int, 3>{1, 2, 2});
}

TEST_CASE("failed or cancelled stage is not marked complete", "[unit]") {
  SECTION("failure after previous completion") {
    Fixture f;
    REQUIRE(f.run());
    f.opts.force = {StageId::Extract};
    f.s2.fail = true;
    auto r = f.run();
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::ToolFailed);
    CHECK(r.error().message == "boom");
    CHECK(f.runs() == std::array<int, 3>{1, 2, 1});
    auto st = StateStore::load(f.dir / "state.toml");
    REQUIRE(st);
    CHECK_FALSE(st.value().is_complete(StageId::Extract, "a"));
    CHECK(st.value().is_complete(StageId::Doctor, "a"));
    f.opts.force.clear();
    f.s2.fail = false;
    REQUIRE(f.run());
    CHECK(f.runs() == std::array<int, 3>{1, 3, 2});
  }
  SECTION("cancel mid-pipeline") {
    Fixture f;
    f.sink.cancel_after = StageId::Doctor;
    auto r = f.run();
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::Cancelled);
    CHECK(f.runs() == std::array<int, 3>{1, 0, 0});
    auto st = StateStore::load(f.dir / "state.toml");
    REQUIRE(st);
    CHECK(st.value().is_complete(StageId::Doctor, "a"));
    CHECK_FALSE(st.value().is_complete(StageId::Extract, "a"));
    f.sink.cancel_after.reset();
    f.sink.cancelled_now = false;
    REQUIRE(f.run());
    CHECK(f.runs() == std::array<int, 3>{1, 1, 1});
  }
}

TEST_CASE("state survives reload", "[unit]") {
  lr::testing::TempDir tmp{"lr_state_test"};
  const auto p = tmp.path / "state.toml";
  {
    auto st = StateStore::load(p);
    REQUIRE(st);
    CHECK_FALSE(st.value().is_complete(StageId::Patch, "fp"));
    st.value().mark_started(StageId::Patch);
    CHECK_FALSE(st.value().is_complete(StageId::Patch, "fp"));
    st.value().mark_complete(StageId::Patch, "fp");
    REQUIRE(st.value().save());
  }
  auto st = StateStore::load(p);
  REQUIRE(st);
  CHECK(st.value().is_complete(StageId::Patch, "fp"));
  CHECK_FALSE(st.value().is_complete(StageId::Patch, "other"));
  { std::ofstream(p) << "[stages\nbroken"; }
  auto bad = StateStore::load(p);
  REQUIRE_FALSE(bad);
  CHECK(bad.error().code == ErrorCode::InvalidFormat);
}

TEST_CASE("state with a stale shaders entry still loads", "[unit]") {
  // An install made before plan B (ADR 0005 M8) wrote a [stages.shaders] table; the stage no longer exists.
  lr::testing::TempDir tmp{"lr_state_stale_test"};
  const auto p = tmp.path / "state.toml";
  {
    std::ofstream(p) << "[stages.patch]\ncomplete = true\nfingerprint = \"fp\"\n\n"
                        "[stages.shaders]\ncomplete = true\nfingerprint = \"old\"\n";
  }
  auto st = StateStore::load(p);
  REQUIRE(st);
  CHECK(st.value().is_complete(StageId::Patch, "fp"));
  CHECK_FALSE(parse_stage("shaders"));
  CHECK(st.value().save());  // and it can be written back
  CHECK(StateStore::load(p));
}
