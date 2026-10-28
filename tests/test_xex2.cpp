// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include "formats/xex2.h"
#include "support/xex2_builder.h"

using namespace lr;
using namespace lr::formats;
using namespace lr::testing;

namespace {
Xex2Builder full() {
  Xex2Builder b;
  b.entry_point(0x82500000).image_base(0x82000000)
      .execution(0x11223344, 0x01000100, 0x01000000, 0x584111F7, 1, 2, 1, 2)
      .libraries({"xboxkrnl.exe", "xam.xex"});
  return b;
}
}  // namespace

TEST_CASE("xex2 parses all fields", "[unit]") {
  auto bytes = full().module_flags(0x8).pe_data_offset(0x3000).build();
  auto r = parse_xex2(bytes);
  REQUIRE(r.ok());
  const auto& i = r.value();
  CHECK(i.module_flags == 0x8);
  CHECK(i.pe_data_offset == 0x3000);
  CHECK(i.entry_point == 0x82500000u);
  CHECK(i.image_base == 0x82000000u);
  REQUIRE(i.execution.has_value());
  CHECK(i.execution->media_id == 0x11223344u);
  CHECK(i.execution->title_id == 0x584111F7u);
  CHECK(i.execution->version == 0x01000100u);
  CHECK(i.execution->base_version == 0x01000000u);
  CHECK(i.execution->platform == 1);
  CHECK(i.execution->executable_type == 2);
  CHECK(i.execution->disc_number == 1);
  CHECK(i.execution->disc_count == 2);
  CHECK(i.import_libraries == std::vector<std::string>{"xboxkrnl.exe", "xam.xex"});
}

TEST_CASE("xex2 optional headers are optional and unknown keys ignored", "[unit]") {
  Xex2Builder b;
  b.raw_pair(0x00012345, 7).raw_pair(0x00000201, 9);
  auto bytes = b.build();
  auto r = parse_xex2(bytes);
  REQUIRE(r.ok());
  CHECK_FALSE(r.value().entry_point.has_value());
  CHECK_FALSE(r.value().execution.has_value());
  CHECK(r.value().import_libraries.empty());
}

TEST_CASE("xex2 duplicate keys: last wins", "[unit]") {
  Xex2Builder b;
  b.entry_point(1).entry_point(2);
  auto bytes = b.build();
  auto r = parse_xex2(bytes);
  REQUIRE(r.ok());
  CHECK(r.value().entry_point == 2u);
}

TEST_CASE("xex2 format_version", "[unit]") {
  CHECK(format_version(0x01000100) == "0.1.1.0");
  CHECK(format_version(0x20ABCD05) == "2.0.43981.5");
}

TEST_CASE("xex2 rejects bad input", "[unit]") {
  auto bad = Xex2Builder().magic("XEX1").build();
  auto r = parse_xex2(bad);
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == ErrorCode::InvalidFormat);

  auto few = full().claimed_count(1000).build();
  r = parse_xex2(few);
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == ErrorCode::OutOfRange);

  auto tiny = std::vector<std::byte>(8);
  CHECK(parse_xex2(tiny).error().code == ErrorCode::OutOfRange);

  // truncating the data region puts block offsets out of range
  auto cut = full().build();
  cut.resize(0x18 + 4 * 8 + 10);
  r = parse_xex2(cut);
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == ErrorCode::OutOfRange);
}

TEST_CASE("xex2 rejects corrupt import table", "[unit]") {
  auto bytes = Xex2Builder().libraries({"a.xex"}).build();
  // string table size (block offset + 4) -> huge
  size_t blk = 0x18 + 8;
  bytes[blk + 4] = std::byte{0x7F};
  auto r = parse_xex2(bytes);
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == ErrorCode::OutOfRange);
}

TEST_CASE("xex2 parses import records, tls and stack size", "[unit]") {
  auto bytes = Xex2Builder()
                   .import_library("xboxkrnl.exe", {0x82002000, 0x82001010, 0x82002004})
                   .import_library("xam.xex", {0x82002008})
                   .tls(64, 0x82003000, 0x20, 0x10)
                   .stack_size(0x80000)
                   .build();
  auto r = parse_xex2(bytes);
  REQUIRE(r.ok());
  const Xex2File& x = r.value();
  CHECK(x.import_libraries == std::vector<std::string>{"xboxkrnl.exe", "xam.xex"});
  REQUIRE(x.imports.size() == 2);
  CHECK(x.imports[0].name == "xboxkrnl.exe");
  CHECK(x.imports[0].records == std::vector<uint32_t>{0x82002000, 0x82001010, 0x82002004});
  CHECK(x.imports[1].name == "xam.xex");
  CHECK(x.imports[1].records == std::vector<uint32_t>{0x82002008});
  REQUIRE(x.tls.has_value());
  CHECK(x.tls->slot_count == 64u);
  CHECK(x.tls->raw_data_address == 0x82003000u);
  CHECK(x.tls->data_size == 0x20u);
  CHECK(x.tls->raw_data_size == 0x10u);
  CHECK(x.default_stack_size == 0x80000u);

  auto plain = parse_xex2(Xex2Builder().build());
  REQUIRE(plain.ok());
  CHECK(plain.value().imports.empty());
  CHECK_FALSE(plain.value().tls.has_value());
  CHECK_FALSE(plain.value().default_stack_size.has_value());
}

TEST_CASE("xex2 rejects an import library whose records run past the block", "[unit]") {
  auto bytes = Xex2Builder().import_library("xboxkrnl.exe", {1, 2}).build();
  // The library's record count sits 0x26 bytes into the library, after the 12-byte header and the
  // 16-byte string table; claim far more records than the block holds.
  const size_t blk = 0x18 + 8;
  bytes[blk + 12 + 16 + 0x26] = std::byte{0x7F};
  auto r = parse_xex2(bytes);
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == ErrorCode::OutOfRange);
}
