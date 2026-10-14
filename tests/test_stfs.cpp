// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "formats/reader.h"
#include "formats/stfs.h"
#include "support/stfs_builder.h"

using namespace lr;
using namespace lr::formats;
using namespace lr::testing;

namespace {
std::vector<std::byte> pattern(size_t n, unsigned seed) {
  std::vector<std::byte> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = std::byte((i * 31 + seed + (i >> 8)) & 0xFF);
  return v;
}
}  // namespace

TEST_CASE("stfs block_to_offset", "[unit]") {
  CHECK(StfsPackage::block_to_offset(0, 0x971A, true) == 0xB000);
  CHECK(StfsPackage::block_to_offset(169, 0x971A, true) == 0xA000 + 170 * 0x1000);
  CHECK(StfsPackage::block_to_offset(170, 0x971A, true) == 0xA000 + 173 * 0x1000);
  CHECK(StfsPackage::block_to_offset(0, 0x971A, false) == 0xA000 + 2 * 0x1000);
}

TEST_CASE("stfs open lists entries and reads files", "[unit]") {
  auto a = pattern(5000, 1), b = pattern(100, 2);
  StfsBuilder sb;
  const int d = 0;  // index of the "data" entry
  sb.add_dir("data");
  sb.add_file("default.xex", a);
  sb.add_file("x.bin", b, d);
  auto bytes = sb.build();
  SpanReader rd(bytes);
  auto p = StfsPackage::open(rd);
  REQUIRE(p.ok());
  CHECK(p.value().header().magic == "LIVE");
  CHECK(p.value().header().content_type == 0x000D0000);
  CHECK(p.value().header().title_id == 0x584111F7);
  CHECK(p.value().header().display_name == u"Minecraft");
  CHECK(p.value().header().read_only);
  bool found = false, dir = false;
  for (auto& e : p.value().entries()) {
    if (e.path == "data/x.bin") found = true;
    if (e.path == "data" && e.is_dir) dir = true;
  }
  CHECK(found);
  CHECK(dir);
  auto f = p.value().open_file("default.xex");
  REQUIRE(f.ok());
  auto got = read_all(*f.value());
  REQUIRE(got.ok());
  CHECK(got.value() == a);
  auto g = p.value().open_file("DATA/x.bin");
  REQUIRE(g.ok());
  CHECK(read_all(*g.value()).value() == b);
  CHECK(p.value().open_file("nope").error().code == ErrorCode::NotFound);
  CHECK(p.value().open_file("data").error().code == ErrorCode::NotFound);
}

TEST_CASE("stfs file spanning hash-table blocks", "[unit]") {
  for (bool ro : {true, false}) {
    auto big = pattern(200 * 0x1000 + 123, 7);
    auto small = pattern(10, 9);
    StfsBuilder sb;
    sb.read_only(ro);
    sb.add_file("big.bin", big);
    sb.add_file("after.bin", small);
    auto bytes = sb.build();
    SpanReader rd(bytes);
    auto p = StfsPackage::open(rd);
    REQUIRE(p.ok());
    auto f = p.value().open_file("big.bin");
    REQUIRE(f.ok());
    CHECK(f.value()->size() == big.size());
    CHECK(read_all(*f.value()).value() == big);
    // A read straddling the hash-table gap after data block 169.
    std::vector<std::byte> win(0x2000);
    REQUIRE(f.value()->read(169 * 0x1000 + 0x800, win).ok());
    CHECK(std::equal(win.begin(), win.end(), big.begin() + 169 * 0x1000 + 0x800));
    CHECK(read_all(*p.value().open_file("after.bin").value()).value() == small);
    std::vector<std::byte> one(1);
    CHECK(f.value()->read(big.size(), one).error().code == ErrorCode::OutOfRange);
  }
}

TEST_CASE("stfs rejects malformed packages", "[unit]") {
  SECTION("bad magic") {
    StfsBuilder sb;
    sb.magic("XXXX").add_file("a", pattern(10, 1));
    auto bytes = sb.build();
    SpanReader rd(bytes);
    CHECK(StfsPackage::open(rd).error().code == ErrorCode::InvalidFormat);
  }
  SECTION("too small") {
    std::vector<std::byte> bytes(16);
    SpanReader rd(bytes);
    CHECK(!StfsPackage::open(rd).ok());
  }
  SECTION("parent index past table") {
    StfsBuilder sb;
    sb.add_file("a", pattern(10, 1), 5);
    auto bytes = sb.build();
    SpanReader rd(bytes);
    CHECK(StfsPackage::open(rd).error().code == ErrorCode::InvalidFormat);
  }
  SECTION("parent is a file") {
    StfsBuilder sb;
    int f = sb.add_file("a", pattern(10, 1));
    sb.add_file("b", pattern(10, 2), f);
    auto bytes = sb.build();
    SpanReader rd(bytes);
    CHECK(StfsPackage::open(rd).error().code == ErrorCode::InvalidFormat);
  }
  SECTION("parent cycle") {
    StfsBuilder sb;
    sb.add_dir("a", 1);
    sb.add_dir("b", 0);
    auto bytes = sb.build();
    SpanReader rd(bytes);
    CHECK(StfsPackage::open(rd).error().code == ErrorCode::InvalidFormat);
  }
  SECTION("non-consecutive file") {
    StfsBuilder sb;
    sb.add_file("a", pattern(10, 1));
    auto bytes = sb.build();
    // Entry 0 of the table sits at block 0 (read-only: 0xB000); clear its consecutive bit.
    bytes[0xB000 + 0x28] = std::byte(static_cast<uint8_t>(bytes[0xB000 + 0x28]) & ~0x40);
    SpanReader rd(bytes);
    auto p = StfsPackage::open(rd);
    REQUIRE(p.ok());
    auto f = p.value().open_file("a");
    REQUIRE(!f.ok());
    CHECK(f.error().code == ErrorCode::InvalidFormat);
    CHECK(f.error().message == "non-consecutive STFS files are not supported yet");
  }
  SECTION("file blocks run past the reader") {
    StfsBuilder sb;
    sb.add_file("a", pattern(3 * 0x1000, 1));
    auto bytes = sb.build();
    SpanReader rd(std::span<const std::byte>(bytes).first(bytes.size() - 0x1800));
    auto p = StfsPackage::open(rd);
    REQUIRE(p.ok());
    CHECK(p.value().open_file("a").error().code == ErrorCode::OutOfRange);
  }
  SECTION("file table past the reader") {
    StfsBuilder sb;
    sb.add_file("a", pattern(10, 1));
    auto bytes = sb.build();
    SpanReader rd(std::span<const std::byte>(bytes).first(0xB000 + 0x10));
    CHECK(StfsPackage::open(rd).error().code == ErrorCode::OutOfRange);
  }
}
