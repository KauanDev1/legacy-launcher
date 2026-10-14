// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "launcher/gui/queue_sink.h"

using lr::gui::QueueSink;

TEST_CASE("QueueSink keeps per-producer order across concurrent posts and drains", "[unit]") {
  QueueSink sink;
  constexpr int kPerThread = 1000;
  auto produce = [&](char tag) {
    for (int i = 0; i < kPerThread; ++i) sink.on_log(std::string(1, tag) + std::to_string(i));
  };
  std::thread a(produce, 'a'), b(produce, 'b');

  std::vector<QueueSink::Event> all;
  // A lost event must fail the test, not hang it.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (all.size() < 2 * kPerThread && std::chrono::steady_clock::now() < deadline) {
    auto part = sink.drain();
    all.insert(all.end(), part.begin(), part.end());
    std::this_thread::yield();
  }
  a.join();
  b.join();
  auto rest = sink.drain();
  all.insert(all.end(), rest.begin(), rest.end());

  REQUIRE(all.size() == 2 * kPerThread);
  std::array<int, 2> next{0, 0};
  for (const auto& e : all) {
    CHECK(e.kind == QueueSink::Event::Log);
    int t = e.text[0] == 'a' ? 0 : 1;
    CHECK(e.text.substr(1) == std::to_string(next[t]));
    ++next[t];
  }
  CHECK(next[0] == kPerThread);
  CHECK(next[1] == kPerThread);
  CHECK(sink.drain().empty());
}

TEST_CASE("QueueSink carries progress and finished events", "[unit]") {
  QueueSink sink;
  sink.on_progress({lr::pipeline::StageId::Extract, 0.5f, "half"});
  sink.post_finished(lr::Error{lr::ErrorCode::Cancelled, "stopped", "retry"});
  auto ev = sink.drain();
  REQUIRE(ev.size() == 2);
  CHECK(ev[0].kind == QueueSink::Event::Progress);
  CHECK(ev[0].progress.stage == lr::pipeline::StageId::Extract);
  CHECK(ev[0].progress.message == "half");
  CHECK(ev[1].kind == QueueSink::Event::Finished);
  REQUIRE(ev[1].error);
  CHECK(ev[1].error->hint == "retry");
}

TEST_CASE("QueueSink cancel sets cancelled", "[unit]") {
  QueueSink sink;
  CHECK_FALSE(sink.cancelled());
  sink.cancel();
  CHECK(sink.cancelled());
}
