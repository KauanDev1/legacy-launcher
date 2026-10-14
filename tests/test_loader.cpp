// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <cstring>

#include "runtime/guest_memory.h"
#include "runtime/loader.h"
#include "support/elf_builder.h"

using namespace lr;
using namespace lr::runtime;

namespace {
std::span<const std::byte> as_bytes(const std::vector<uint8_t>& v) {
  return {reinterpret_cast<const std::byte*>(v.data()), v.size()};
}
}  // namespace

TEST_CASE("load_image copies every section to its absolute guest address", "[unit]") {
  auto mem = GuestMemory::reserve(0x1'0000'0000ull);
  REQUIRE(mem.ok());
  auto elf = test::build_elf(0x82000100, {{".text", 0x82000100, {1, 2, 3, 4, 5, 6, 7, 8}, true},
                                          {".data", 0x82000200, {9, 8, 7}, false}});
  auto r = load_image(mem.value(), as_bytes(elf));
  REQUIRE(r.ok());
  CHECK(r.value().base == 0x82000000u + 0x100u);
  CHECK(r.value().entry == 0x82000100u);
  CHECK(r.value().size >= 0x200u + 3u - 0x100u);
  const uint8_t* t = mem.value().translate<uint8_t>(0x82000100);
  CHECK(t[0] == 1);
  CHECK(t[7] == 8);
  CHECK(mem.value().translate<uint8_t>(0x82000200)[2] == 7);
}

TEST_CASE("load_image rejects garbage and out-of-memory sections", "[unit]") {
  auto mem = GuestMemory::reserve(0x1'0000'0000ull);
  REQUIRE(mem.ok());
  std::vector<uint8_t> junk(64, 0xAB);
  auto r = load_image(mem.value(), as_bytes(junk));
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == ErrorCode::InvalidFormat);

  std::vector<uint8_t> empty;
  CHECK_FALSE(load_image(mem.value(), as_bytes(empty)).ok());

  auto small = GuestMemory::reserve(0x1000'0000ull);
  REQUIRE(small.ok());
  auto elf = test::build_elf(0x82000100, {{".text", 0x82000100, {1, 2, 3, 4}, true}});
  auto r2 = load_image(small.value(), as_bytes(elf));
  REQUIRE_FALSE(r2.ok());
  CHECK(r2.error().code == ErrorCode::OutOfRange);
}

TEST_CASE("load_image rejects an entry point outside the image", "[unit]") {
  auto mem = GuestMemory::reserve(0x1'0000'0000ull);
  REQUIRE(mem.ok());
  auto elf = test::build_elf(0x90000000, {{".text", 0x82000100, {1, 2, 3, 4}, true}});
  auto r = load_image(mem.value(), as_bytes(elf));
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == ErrorCode::InvalidFormat);
}
