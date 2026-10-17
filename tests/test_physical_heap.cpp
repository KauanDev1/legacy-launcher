// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include "runtime/memory/physical_heap.h"
#include "runtime/memory_map.h"

using namespace lr;
using namespace lr::runtime;

namespace {
constexpr uint32_t kRw = 0x04;
constexpr uint32_t kLarge = kRw | PhysicalHeap::kMemLargePages;
constexpr uint32_t kHuge = kRw | PhysicalHeap::kMem16MbPages;
constexpr uint32_t kAll = 0xFFFFFFFF;
}  // namespace

TEST_CASE("physical heap allocates top-down in range", "[unit]") {
  PhysicalHeap h;
  auto a = h.allocate(0x10000, kLarge, 0, kAll, 0);
  REQUIRE(a);
  CHECK(a.value() == kPhysicalViewA0 + 0x20000000 - 0x10000);
  auto b = h.allocate(0x10000, kLarge, 0, kAll, 0);
  REQUIRE(b);
  CHECK(b.value() == a.value() - 0x10000);
  auto c = h.allocate(0x10000, kLarge, 0x100000, 0x1FFFFF, 0);  // inclusive max
  REQUIRE(c);
  CHECK(c.value() == kPhysicalViewA0 + 0x1F0000);
  CHECK(h.allocate(0x10000, kLarge, 0x20000000, kAll, 0).error().code == ErrorCode::OutOfRange);
  CHECK(h.allocate(0, kLarge, 0, kAll, 0).error().code == ErrorCode::InvalidFormat);
}

TEST_CASE("physical heap honours alignment", "[unit]") {
  PhysicalHeap h;
  auto a = h.allocate(0x1000, kRw, 0, 0x1FFFFF, 0x100000);
  REQUIRE(a);
  CHECK(PhysicalHeap::physical_address(a.value()) == 0x100000);
  CHECK(h.allocate(0x1000, kRw, 0, kAll, 3).error().code == ErrorCode::InvalidFormat);
  auto b = h.allocate(0x1000, kLarge, 0, kAll, 0x1000);  // alignment raised to the page size
  REQUIRE(b);
  CHECK((PhysicalHeap::physical_address(b.value()) & 0xFFFF) == 0);
}

TEST_CASE("physical heap view follows page size", "[unit]") {
  PhysicalHeap h;
  auto a = h.allocate(0x1000, kLarge, 0, kAll, 0);
  auto c = h.allocate(0x1000, kHuge, 0, kAll, 0);
  auto e = h.allocate(0x1000, kRw, 0, kAll, 0);
  REQUIRE(a);
  REQUIRE(c);
  REQUIRE(e);
  CHECK(a.value() >> 29 == 0x5);
  CHECK(c.value() >> 29 == 0x6);
  CHECK(e.value() >> 29 == 0x7);
  CHECK(h.allocation_size(a.value()).value() == 0x10000);
  CHECK(h.allocation_size(c.value() + 0xFFFFFF).value() == 0x1000000);
  CHECK(h.allocation_size(e.value()).value() == 0x1000);
  CHECK(h.allocation_size(e.value() + 0x1000).error().code == ErrorCode::NotFound);
  // Physical pages never overlap between views.
  CHECK(PhysicalHeap::physical_address(a.value()) == 0x20000000 - 0x10000);
  CHECK(PhysicalHeap::physical_address(c.value()) == 0x1000000 * 30);
}

TEST_CASE("physical heap physical_address round trip", "[unit]") {
  CHECK(PhysicalHeap::physical_address(0xA0012345) == 0x12345);
  CHECK(PhysicalHeap::physical_address(0xC1000000) == 0x1000000);
  CHECK(PhysicalHeap::physical_address(0xE0000000) == 0x1000);
  CHECK(PhysicalHeap::physical_address(0xE0005000) == 0x6000);
  PhysicalHeap h;
  auto e = h.allocate(0x2000, kRw, 0x40000, 0x41FFF, 0);
  REQUIRE(e);
  CHECK(e.value() == 0xE0000000 + 0x40000 - 0x1000);
  CHECK(PhysicalHeap::physical_address(e.value()) == 0x40000);
  // Physical page 0 is never handed out through E0.
  CHECK(h.allocate(0x1000, kRw, 0, 0xFFF, 0).error().code == ErrorCode::OutOfRange);
  auto lo2 = h.allocate(0x1000, kRw, 0, 0x1FFF, 0);
  REQUIRE(lo2);
  CHECK(lo2.value() == 0xE0000000);
  CHECK(PhysicalHeap::physical_address(lo2.value()) == 0x1000);
  auto top = h.allocate(0x1000, kRw, 0, kAll, 0);
  REQUIRE(top);
  CHECK(top.value() == 0xE0000000 + 0x1FFFF000 - 0x1000);
}

TEST_CASE("physical heap free then reuse", "[unit]") {
  PhysicalHeap h;
  auto a = h.allocate(0x10000, kLarge, 0, kAll, 0);
  auto b = h.allocate(0x10000, kLarge, 0, kAll, 0);
  REQUIRE(a);
  REQUIRE(b);
  REQUIRE(h.free(a.value()));
  CHECK(h.free(a.value()).error().code == ErrorCode::NotFound);
  CHECK(h.free(b.value() + 0x1000).error().code == ErrorCode::NotFound);
  auto c = h.allocate(0x10000, kLarge, 0, kAll, 0);
  REQUIRE(c);
  CHECK(c.value() == a.value());
  // A freed range can be taken by a different view.
  REQUIRE(h.free(c.value()));
  auto d = h.allocate(0x1000, kRw, 0x1FFF0000, 0x1FFFFFFF, 0);
  REQUIRE(d);
  CHECK(d.value() >> 29 == 0x7);
}

TEST_CASE("physical heap out of physical memory", "[unit]") {
  PhysicalHeap h;
  auto a = h.allocate(0x1F000000, kHuge, 0, kAll, 0);  // rounds up to 496 MiB
  REQUIRE(a);
  CHECK(h.allocation_size(a.value()).value() == 0x1F000000);
  CHECK(h.allocate(0x2000000, kHuge, 0, kAll, 0).error().code == ErrorCode::OutOfRange);
  CHECK(h.allocate(0x1000000, kHuge, 0, kAll, 0));
  CHECK(h.allocate(0x10000, kLarge, 0, kAll, 0).error().code == ErrorCode::OutOfRange);
  CHECK(h.allocate(0x40000000, kRw, 0, kAll, 0).error().code == ErrorCode::OutOfRange);
}
