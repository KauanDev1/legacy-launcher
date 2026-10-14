// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>

#include "runtime/be.h"

using lr::runtime::be;

TEST_CASE("be<uint32_t> stores big-endian bytes", "[unit]") {
  be<uint32_t> v;
  v = 0x11223344u;
  uint8_t b[4];
  std::memcpy(b, &v, 4);
  CHECK(b[0] == 0x11);
  CHECK(b[1] == 0x22);
  CHECK(b[2] == 0x33);
  CHECK(b[3] == 0x44);
  CHECK(static_cast<uint32_t>(v) == 0x11223344u);
}

TEST_CASE("be<float>/be<double>/be<uint16_t> round-trip", "[unit]") {
  be<float> f;
  f = 1.5f;
  CHECK(static_cast<float>(f) == 1.5f);
  be<double> d;
  d = -2.25;
  CHECK(static_cast<double>(d) == -2.25);
  be<uint16_t> s(0xABCD);
  uint8_t b[2];
  std::memcpy(b, &s, 2);
  CHECK(b[0] == 0xAB);
  CHECK(static_cast<uint16_t>(s) == 0xABCD);
  be<int32_t> n(-2);
  CHECK(static_cast<int32_t>(n) == -2);
  static_assert(sizeof(be<uint32_t>) == 4);
  static_assert(sizeof(be<double>) == 8);
}

#include "runtime/guest_memory.h"

using lr::runtime::GuestMemory;

TEST_CASE("GuestMemory reserves 4 GiB and translates guest addresses", "[unit]") {
  auto r = GuestMemory::reserve(0x1'0000'0000ull);
  REQUIRE(r.ok());
  GuestMemory mem = std::move(r.value());
  CHECK(mem.size() == 0x1'0000'0000ull);
  REQUIRE(mem.base() != nullptr);
  *mem.translate<uint32_t>(0x82000000) = 7;
  CHECK(*mem.translate<uint32_t>(0x82000000) == 7);
  CHECK(mem.translate<uint8_t>(0x82000000) == mem.base() + 0x82000000);
  GuestMemory moved = std::move(mem);
  CHECK(mem.base() == nullptr);  // NOLINT: moved-from is defined as empty
  CHECK(*moved.translate<uint32_t>(0x82000000) == 7);
}

TEST_CASE("GuestMemory::reserve rejects zero size", "[unit]") {
  CHECK_FALSE(GuestMemory::reserve(0).ok());
}
