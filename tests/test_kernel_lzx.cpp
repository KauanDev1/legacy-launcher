// SPDX-License-Identifier: MIT
// LDI* LZX decompression (runtime/kernel/exports/lzx.cpp) over libmspack's lzxd.c.
//
// Hand-built stream layout (an LZX stream is a sequence of 16-bit LITTLE-endian words whose bits are read MSB
// first; see lzxd.c and the LZX spec):
//   first block only:  1 bit  Intel E8 header flag = 0
//   every block:       3 bits block type (3 = uncompressed), 24 bits block length (MSB first)
//   type 3:            the rest of the current 16-bit word is padding, then 12 bytes R0, R1, R2 (LE u32 each),
//                      then the raw bytes (plus one pad byte after an odd-sized block).
// For a 1024-byte first block: bits 0 | 011 | 000000000000 | 010000000000 are 0x3000 and 0x4000 as words
// (4 padding bits), i.e. the bytes 00 30 00 40.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

#include "runtime/be.h"
#include "runtime/kernel/status.h"
#include "support/kernel_fixture.h"

using lr::runtime::be;
using lr::testing::KernelFixture;

namespace {

constexpr uint32_t kWindow = 1u << 17;

std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(seed + i * 7 + (i >> 8));
  return v;
}

// First block (with the E8 header bit) of an uncompressed LZX stream carrying `data` (even length, < 64 KiB).
std::vector<uint8_t> uncompressed_stream(const std::vector<uint8_t>& data) {
  const uint32_t len = static_cast<uint32_t>(data.size());
  // bits: 0 | 011 | len(24) -> 28 bits, MSB first, zero padded to 32
  const uint32_t bits = (0x3u << 24 | len) << 4;
  std::vector<uint8_t> s = {static_cast<uint8_t>(bits >> 16), static_cast<uint8_t>(bits >> 24),
                            static_cast<uint8_t>(bits), static_cast<uint8_t>(bits >> 8)};
  for (int r = 0; r < 3; ++r) s.insert(s.end(), {1, 0, 0, 0});
  s.insert(s.end(), data.begin(), data.end());
  return s;
}

struct Lzx {
  KernelFixture f;
  uint32_t handle_ptr = f.alloc(4);
  uint32_t cfg = f.alloc(8);
  uint32_t out_max = f.alloc(4);
  uint32_t out_src = f.alloc(4);
  uint32_t dst_size = f.alloc(4);
  uint32_t src = f.alloc(0x10000);
  uint32_t dst = f.alloc(0x10000);

  uint32_t create(uint32_t window = kWindow) {
    *f.mem.translate<be<uint32_t>>(cfg) = window;
    *f.mem.translate<be<uint32_t>>(cfg + 4) = 1;
    return static_cast<uint32_t>(f.call("LDICreateDecompression", out_max, cfg, 0, 0, out_src, handle_ptr));
  }
  uint32_t handle() { return *f.mem.translate<be<uint32_t>>(handle_ptr); }
  uint32_t decompress(const std::vector<uint8_t>& in, uint32_t want) {
    std::memcpy(f.mem.translate<uint8_t>(src), in.data(), in.size());
    *f.mem.translate<be<uint32_t>>(dst_size) = want;
    return static_cast<uint32_t>(
        f.call("LDIDecompress", handle(), src, static_cast<uint32_t>(in.size()), dst, dst_size));
  }
  uint32_t produced() { return *f.mem.translate<be<uint32_t>>(dst_size); }
  bool dst_equals(const std::vector<uint8_t>& v) {
    return std::memcmp(f.mem.translate<uint8_t>(dst), v.data(), v.size()) == 0;
  }
};

}  // namespace

TEST_CASE("uncompressed LZX block round trips", "[unit][kernel][lzx]") {
  Lzx t;
  REQUIRE(t.create() == 0);
  CHECK(*t.f.mem.translate<be<uint32_t>>(t.out_max) == 32768);
  const auto data = pattern(1024, 5);
  const auto stream = uncompressed_stream(data);
  CHECK(stream[0] == 0x00);
  CHECK(stream[1] == 0x30);
  CHECK(stream[2] == 0x00);
  CHECK(stream[3] == 0x40);
  REQUIRE(t.decompress(stream, 1024) == 0);
  CHECK(t.produced() == 1024);
  CHECK(t.dst_equals(data));
  CHECK(t.f.call("LDIDestroyDecompression", t.handle()) == 0);
}

TEST_CASE("reset allows a second independent stream", "[unit][kernel][lzx]") {
  Lzx t;
  REQUIRE(t.create() == 0);
  const auto a = pattern(1024, 1), b = pattern(2048, 99);
  REQUIRE(t.decompress(uncompressed_stream(a), 1024) == 0);
  CHECK(t.dst_equals(a));
  // Without a reset the next stream's header bit is not expected: it is only valid after LDIResetDecompression.
  REQUIRE(t.f.call("LDIResetDecompression", t.handle()) == 0);
  REQUIRE(t.decompress(uncompressed_stream(b), 2048) == 0);
  CHECK(t.produced() == 2048);
  CHECK(t.dst_equals(b));
  CHECK(t.f.call("LDIDestroyDecompression", t.handle()) == 0);
  CHECK(t.f.call("LDIDestroyDecompression", t.handle()) != 0);  // already closed
}

TEST_CASE("corrupt input returns an error status, no crash", "[unit][kernel][lzx]") {
  Lzx t;
  REQUIRE(t.create() == 0);

  SECTION("invalid block type") {
    auto s = uncompressed_stream(pattern(1024, 3));
    s[1] = 0x70;  // 0 | 111: block type 7
    CHECK(t.decompress(s, 1024) != 0);
  }
  SECTION("truncated block") {
    auto s = uncompressed_stream(pattern(1024, 3));
    s.resize(100);
    CHECK(t.decompress(s, 1024) != 0);
  }
  SECTION("garbage compressed data") {
    std::vector<uint8_t> s(4000);
    uint32_t x = 12345;
    for (auto& b : s) b = static_cast<uint8_t>((x = x * 1664525u + 1013904223u) >> 24);
    s[0] &= 0x7F;
    s[1] = static_cast<uint8_t>((s[1] & 0x0F) | 0x20);  // verbatim block
    CHECK(t.decompress(s, 32768) != 0);
  }
  SECTION("empty input") { CHECK(t.decompress({}, 1024) != 0); }
  SECTION("bad parameters") {
    *t.f.mem.translate<be<uint32_t>>(t.dst_size) = 40000;  // above the 32 KiB block limit
    CHECK(t.f.call("LDIDecompress", t.handle(), t.src, 16, t.dst, t.dst_size) != 0);
    CHECK(t.f.call("LDIDecompress", 0x1234, t.src, 16, t.dst, t.dst_size) != 0);
    CHECK(t.create(12345) != 0);  // not a power of two
    CHECK(t.create(1u << 30) != 0);
  }
}
