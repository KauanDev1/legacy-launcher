// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "runtime/memory/page_heap.h"
#include "runtime/memory_map.h"

using namespace lr;
using namespace lr::runtime;

namespace {
PageHeap make4k() { return PageHeap(kHeap4kBase, kHeap4kEnd, 0x1000); }
}  // namespace

TEST_CASE("reserve bottom-up and top-down", "[unit]") {
  PageHeap h = make4k();
  auto a = h.reserve(0x2000, 0, false, std::nullopt);
  REQUIRE(a);
  CHECK(a.value() == kHeap4kBase);
  auto b = h.reserve(0x1800, 0, false, std::nullopt);  // rounds up to 0x2000
  REQUIRE(b);
  CHECK(b.value() == kHeap4kBase + 0x2000);
  CHECK(h.allocation_size(b.value()).value() == 0x2000);
  auto t = h.reserve(0x3000, 0, true, std::nullopt);
  REQUIRE(t);
  CHECK(t.value() == kHeap4kEnd - 0x3000);
  auto t2 = h.reserve(0x1000, 0, true, std::nullopt);
  REQUIRE(t2);
  CHECK(t2.value() == kHeap4kEnd - 0x4000);
}

TEST_CASE("alignment is honoured", "[unit]") {
  PageHeap h = make4k();
  REQUIRE(h.reserve(0x1000, 0, false, std::nullopt));
  auto a = h.reserve(0x1000, 0x10000, false, std::nullopt);
  REQUIRE(a);
  CHECK(a.value() == 0x00020000);  // 0x10000 is taken, next 64K boundary
  auto t = h.reserve(0x1000, 0x100000, true, std::nullopt);
  REQUIRE(t);
  CHECK(t.value() % 0x100000 == 0);
  CHECK(t.value() == kHeap4kEnd - 0x100000);
  CHECK(h.reserve(0x1000, 0x3000, false, std::nullopt).error().code == ErrorCode::InvalidFormat);
}

TEST_CASE("fixed base conflicts with existing region", "[unit]") {
  PageHeap h = make4k();
  REQUIRE(h.reserve(0x4000, 0, false, 0x20000));
  auto c = h.reserve(0x1000, 0, false, 0x22000);
  REQUIRE_FALSE(c);
  CHECK(c.error().code == ErrorCode::OutOfRange);
  CHECK(h.reserve(0x2000, 0, false, 0x1F000).error().code == ErrorCode::OutOfRange);  // overlaps tail
  CHECK(h.reserve(0x1000, 0, false, 0x24000));
  CHECK(h.reserve(0x1000, 0, false, 0x1000).error().code == ErrorCode::OutOfRange);  // below heap base
}

TEST_CASE("commit inside reserved region calls hook", "[unit]") {
  PageHeap h = make4k();
  struct Call {
    uint32_t a, s;
    bool c;
  };
  std::vector<Call> calls;
  h.set_commit_hook([&](uint32_t a, uint32_t s, bool c) {
    // Re-entrancy: the hook may query without deadlocking.
    CHECK(h.query(a));
    calls.push_back({a, s, c});
  });
  uint32_t base = h.reserve(0x8000, 0, false, std::nullopt).value();
  REQUIRE(h.commit(base + 0x1000, 0x2000, kPageReadWrite));
  REQUIRE(calls.size() == 1);
  CHECK(calls[0].a == base + 0x1000);
  CHECK(calls[0].s == 0x2000);
  CHECK(calls[0].c);
  // Overlapping commit only reports the new page.
  REQUIRE(h.commit(base + 0x2000, 0x2000, kPageReadOnly));
  REQUIRE(calls.size() == 2);
  CHECK(calls[1].a == base + 0x3000);
  CHECK(calls[1].s == 0x1000);
  REQUIRE(h.decommit(base + 0x1000, 0x3000));
  REQUIRE(calls.size() == 3);
  CHECK(calls[2].a == base + 0x1000);
  CHECK(calls[2].s == 0x3000);
  CHECK_FALSE(calls[2].c);
  CHECK(h.commit(base + 0x8000, 0x1000, kPageReadWrite).error().code == ErrorCode::NotFound);
  CHECK(h.commit(base, 0x1000, 0x3).error().code == ErrorCode::InvalidFormat);
}

TEST_CASE("query reports state and allocation base", "[unit]") {
  PageHeap h = make4k();
  uint32_t base = h.reserve(0x6000, 0, false, std::nullopt).value();
  REQUIRE(h.commit(base + 0x1000, 0x2000, kPageReadWrite | kPageGuard));
  auto q = h.query(base + 0x1800);
  REQUIRE(q);
  CHECK(q.value().state == RegionInfo::Committed);
  CHECK(q.value().base == base + 0x1000);
  CHECK(q.value().size == 0x2000);
  CHECK(q.value().allocation_base == base);
  CHECK(q.value().protect == (kPageReadWrite | kPageGuard));
  CHECK(q.value().allocation_protect == kPageReadWrite);
  auto r = h.query(base + 0x4000);
  CHECK(r.value().state == RegionInfo::Reserved);
  CHECK(r.value().base == base + 0x3000);
  CHECK(r.value().size == 0x3000);
  auto f = h.query(base + 0x6000);
  CHECK(f.value().state == RegionInfo::Free);
  CHECK(f.value().allocation_base == 0);
  CHECK(f.value().base == base + 0x6000);
  CHECK(f.value().size == kHeap4kEnd - (base + 0x6000));
  // Mixed protections split the region at the protect boundary.
  uint32_t old = 0;
  REQUIRE(h.protect(base + 0x2000, 0x1000, kPageReadOnly, &old));
  CHECK(old == (kPageReadWrite | kPageGuard));
  CHECK(h.query(base + 0x1000).value().size == 0x1000);
  CHECK(h.query(base + 0x2000).value().protect == kPageReadOnly);
  CHECK(h.protect(base + 0x4000, 0x1000, kPageReadOnly, nullptr).error().code == ErrorCode::OutOfRange);
  CHECK(h.query(kHeap4kEnd).error().code == ErrorCode::OutOfRange);
}

TEST_CASE("page heap edge cases", "[unit]") {
  PageHeap h = make4k();
  CHECK(h.reserve(0, 0, false, std::nullopt).error().code == ErrorCode::InvalidFormat);
  CHECK(h.reserve(kHeap4kEnd, 0, false, std::nullopt).error().code == ErrorCode::OutOfRange);
  // Unaligned fixed base is rounded down to the page.
  auto a = h.reserve(0x1000, 0, false, 0x20800);
  REQUIRE(a);
  CHECK(a.value() == 0x20000);
  // Double reserve of the same range.
  CHECK(h.reserve(0x1000, 0, false, 0x20000).error().code == ErrorCode::OutOfRange);
  // Release with non-base address.
  uint32_t b = h.reserve(0x3000, 0, false, 0x30000).value();
  CHECK(h.release(b + 0x1000).error().code == ErrorCode::NotFound);
  CHECK(h.allocation_size(b + 0x1000).error().code == ErrorCode::NotFound);
  // Release reports committed pages and frees the range for reuse.
  uint32_t released = 0;
  h.set_commit_hook([&](uint32_t, uint32_t s, bool c) {
    if (!c) released += s;
  });
  REQUIRE(h.commit(b, 0x2000, kPageReadWrite));
  REQUIRE(h.release(b));
  CHECK(released == 0x2000);
  CHECK(h.query(b).value().state == RegionInfo::Free);
  CHECK(h.reserve(0x3000, 0, false, 0x30000));
  // 64K heap rounds to its page size and ends at 0x70000000.
  PageHeap big(kHeap64kBase, kHeap64kEnd, 0x10000);
  CHECK(big.reserve(1, 0, true, std::nullopt).value() == kHeap64kEnd - 0x10000);
}
