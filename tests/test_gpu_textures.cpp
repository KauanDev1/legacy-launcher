// SPDX-License-Identifier: MIT
// Texture formats and caches (Task 19): the Xenos tiled address math (checked against the independent CPU reference
// of support/xenos_tiling.h and hand-worked vectors), the endian swaps, CTX1, packed mips, the CPU loader's guest
// bounds, the sampler state taken from a fetch constant, and the Vulkan texture cache (uploads, views, heap indices,
// invalidation) with the validation layer on.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "common/log.h"
#include "runtime/gpu/pm4.h"
#include "runtime/gpu/registers.h"
#include "runtime/gpu/texture_format.h"
#include "runtime/gpu/vk/samplers.h"
#include "runtime/gpu/vk/textures.h"
#include "support/vk_shader_harness.h"
#include "support/xenos_tiling.h"

// FetchSpec is built with designated initializers naming only the fields a test cares about.
#pragma clang diagnostic ignored "-Wmissing-field-initializers"

using lr::gpu::ClampMode;
using lr::gpu::MemoryAccess;
using lr::gpu::TextureDimension;
using lr::gpu::TextureFetch;
using lr::gpu::XenosFormat;
namespace vk = lr::gpu::vk;

namespace {

constexpr uint32_t kXYZW = 0 | (1u << 3) | (2u << 6) | (3u << 9);
constexpr uint32_t kZYXW = 2 | (1u << 3) | (0u << 6) | (3u << 9);

uint32_t log2_of(uint32_t v) {
  uint32_t l = 0;
  while ((1u << l) < v) ++l;
  return l;
}

// A texture fetch constant from named fields (layout: runtime/gpu/texture_format.h).
struct FetchSpec {
  XenosFormat format = XenosFormat::k_8_8_8_8;
  TextureDimension dim = TextureDimension::k2DOrStacked;
  uint32_t w = 1, h = 1, d = 1;
  bool tiled = false, stacked = false, packed = false;
  uint32_t pitch = 0;  // texels; 0: width rounded up to 32
  uint32_t endian = 0;
  uint32_t base = 0, mip = 0;
  uint32_t mip_min = 0, mip_max = 0;
  uint32_t swizzle = kXYZW;
  uint32_t signs = 0;  // 2 bits per component x..w
  std::array<ClampMode, 3> clamp{ClampMode::kRepeat, ClampMode::kRepeat, ClampMode::kRepeat};
  uint32_t mag = 0, min = 0, mip_filter = 0, aniso = 0, border = 0;
  int32_t lod_bias = 0;  // 1/32 units
};

TextureFetch make_fetch(const FetchSpec& s) {
  TextureFetch f;
  const uint32_t pitch = s.pitch ? s.pitch : (s.w + 31) / 32 * 32;
  f.dwords[0] = 2u | (s.signs << 2) | (uint32_t(s.clamp[0]) << 10) | (uint32_t(s.clamp[1]) << 13) |
                (uint32_t(s.clamp[2]) << 16) | ((pitch / 32) << 22) | (uint32_t(s.tiled) << 31);
  f.dwords[1] = uint32_t(s.format) | (s.endian << 6) | (uint32_t(s.stacked) << 10) | (s.base & 0xFFFFF000u);
  switch (s.dim) {
    case TextureDimension::k1D: f.dwords[2] = s.w - 1; break;
    case TextureDimension::k3D: f.dwords[2] = (s.w - 1) | ((s.h - 1) << 11) | ((s.d - 1) << 22); break;
    default: f.dwords[2] = (s.w - 1) | ((s.h - 1) << 13) | ((s.d - 1) << 26); break;
  }
  f.dwords[3] = (s.swizzle << 1) | (s.mag << 19) | (s.min << 21) | (s.mip_filter << 23) | (s.aniso << 25);
  f.dwords[4] = (s.mip_min << 2) | (s.mip_max << 6) | ((uint32_t(s.lod_bias) & 0x3FFu) << 12);
  f.dwords[5] = s.border | (uint32_t(s.dim) << 9) | (uint32_t(s.packed) << 11) | (s.mip & 0xFFFFF000u);
  return f;
}

// Guest memory where physical address == guest address; `alloc_end` (0: none) is the end of the only physical
// allocation, starting at 0.
struct Guest {
  explicit Guest(size_t size, uint32_t alloc_end = 0) : bytes(size, 0), end(alloc_end) {}
  MemoryAccess access() {
    MemoryAccess m;
    m.base = bytes.data();
    const uint64_t n = bytes.size();
    m.to_guest = [n](uint32_t phys) -> std::optional<uint32_t> {
      if (phys >= n) return std::nullopt;
      return phys;
    };
    m.size = n;
    if (end != 0) {
      const uint32_t e = end;
      m.allocation_end = [e](uint32_t phys) -> std::optional<uint32_t> {
        if (phys >= e) return std::nullopt;
        return e;
      };
    }
    return m;
  }
  // Stores a 32-bit value big-endian (as the guest CPU does).
  void put_be32(size_t at, uint32_t v) {
    const uint32_t be = __builtin_bswap32(v);
    std::memcpy(bytes.data() + at, &be, 4);
  }
  std::vector<uint8_t> bytes;
  uint32_t end;
};

// Collects the Vulkan validation-layer errors (Device logs them with log::error) for the scope.
class ValidationErrors {
 public:
  ValidationErrors() {
    previous_ = lr::log::set_sink([this](lr::log::Level level, std::string_view msg) {
      if (level == lr::log::Level::Error) errors.emplace_back(msg);
      std::fprintf(stderr, "%.*s\n", static_cast<int>(msg.size()), msg.data());
    });
  }
  ~ValidationErrors() { lr::log::set_sink(std::move(previous_)); }
  std::vector<std::string> errors;

 private:
  lr::log::Sink previous_;
};

lr::Result<lr::gpu::HostTexture> load(const TextureFetch& f, const MemoryAccess& mem) {
  LR_TRY_ASSIGN(desc, lr::gpu::describe_texture(f));
  LR_TRY_ASSIGN(fp, lr::gpu::texture_footprint(desc, mem));
  return lr::gpu::load_texture(desc, mem, fp);
}

uint32_t le32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

}  // namespace

// --- address math ----------------------------------------------------------------------------------------------------

TEST_CASE("tiled offset matches known values", "[unit][gpu]") {
  // Hand-worked vectors (pitch 64, 4-byte blocks): pipe bits 6-7 step every 8 columns, y[3] flips pipe bit 1 and
  // moves the micro row up (bit 10), y[4] is the bank (bit 11).
  CHECK(lr::gpu::tiled_offset_2d(0, 0, 64, 4) == 0);
  CHECK(lr::gpu::tiled_offset_2d(8, 0, 64, 4) == 64);
  CHECK(lr::gpu::tiled_offset_2d(16, 0, 64, 4) == 128);
  CHECK(lr::gpu::tiled_offset_2d(0, 8, 64, 4) == 1152);
  CHECK(lr::gpu::tiled_offset_2d(8, 8, 64, 4) == 1216);
  CHECK(lr::gpu::tiled_offset_2d(0, 16, 64, 4) == 2048);
  CHECK(lr::gpu::tiled_offset_2d(1, 0, 64, 4) == 4);
  CHECK(lr::gpu::tiled_offset_2d(0, 1, 64, 4) == 16);
  CHECK(lr::gpu::tiled_offset_2d(32, 0, 64, 4) == 4096);  // second macro tile: contiguous 4 KiB per tile
  CHECK(lr::gpu::tiled_offset_2d(0, 32, 64, 4) == 8192);

  // Every block of assorted surfaces against the independent reference, for 1..16 bytes per block.
  const uint32_t bpb = GENERATE(1u, 2u, 4u, 8u, 16u);
  const uint32_t pitch = GENERATE(32u, 64u, 96u, 160u, 1280u);
  INFO("bpb " << bpb << " pitch " << pitch);
  std::set<uint32_t> seen;
  uint32_t mismatches = 0;
  for (uint32_t y = 0; y < 64; ++y)
    for (uint32_t x = 0; x < std::min(pitch, 160u); ++x) {
      const uint32_t got = lr::gpu::tiled_offset_2d(x, y, pitch, bpb);
      mismatches += got != lr::testing::xenos_tiled_offset_2d(x, y, pitch, log2_of(bpb)) ? 1 : 0;
      seen.insert(got);
    }
  CHECK(mismatches == 0);
  CHECK(seen.size() == size_t{64} * std::min(pitch, 160u));
}

TEST_CASE("tiled extents cover every block", "[unit][gpu]") {
  const uint32_t bpb = GENERATE(1u, 2u, 4u, 8u, 16u);
  for (const auto& [w, h] : std::vector<std::pair<uint32_t, uint32_t>>{{1, 1}, {17, 5}, {33, 40}, {100, 70}, {64, 64}}) {
    const uint32_t pitch = (w + 31) / 32 * 32;
    uint64_t max_end = 0;
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) max_end = std::max<uint64_t>(max_end, lr::gpu::tiled_offset_2d(x, y, pitch, bpb) + bpb);
    INFO("2D " << w << "x" << h << " bpb " << bpb);
    CHECK(lr::gpu::tiled_extent_2d(w, h, pitch, bpb) >= max_end);
  }
  for (const auto& [w, h, d] :
       std::vector<std::array<uint32_t, 3>>{{1, 1, 1}, {8, 8, 4}, {33, 17, 5}, {64, 32, 9}, {40, 70, 3}}) {
    const uint32_t pitch = (w + 31) / 32 * 32, height = (h + 31) / 32 * 32;
    uint64_t max_end = 0;
    std::set<uint64_t> seen;
    for (uint32_t z = 0; z < d; ++z)
      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
          const uint64_t o = lr::gpu::tiled_offset_3d(x, y, z, pitch, height, bpb);
          max_end = std::max<uint64_t>(max_end, o + bpb);
          seen.insert(o);
        }
    INFO("3D " << w << "x" << h << "x" << d << " bpb " << bpb);
    CHECK(seen.size() == size_t{w} * h * d);  // every block has its own address
    CHECK(lr::gpu::tiled_extent_3d(w, h, d, pitch, height, bpb) >= max_end);
  }
}

TEST_CASE("endian swap modes", "[unit][gpu]") {
  CHECK(lr::gpu::endian_swap32(0x11223344u, 0) == 0x11223344u);
  CHECK(lr::gpu::endian_swap32(0x11223344u, 1) == 0x22114433u);  // 8in16
  CHECK(lr::gpu::endian_swap32(0x11223344u, 2) == 0x44332211u);  // 8in32
  CHECK(lr::gpu::endian_swap32(0x11223344u, 3) == 0x33441122u);  // 16in32

  // Through the loader: 16-bit texels stored as big-endian halves (8in16) and 32-bit ones as big-endian words
  // (8in32) come out as their values; 8-bit texels take their byte from the swapped word.
  Guest g(1 << 16);
  const MemoryAccess mem = g.access();
  g.bytes[0x1000] = 0x12;  // k_16 texel 0 = 0x1234, texel 1 = 0xABCD, big-endian halves
  g.bytes[0x1001] = 0x34;
  g.bytes[0x1002] = 0xAB;
  g.bytes[0x1003] = 0xCD;
  auto r16 = load(make_fetch({.format = XenosFormat::k_16, .w = 2, .endian = 1, .base = 0x1000}), mem);
  REQUIRE(r16.ok());
  CHECK(r16.value().bytes == std::vector<uint8_t>{0x34, 0x12, 0xCD, 0xAB});
  g.put_be32(0x2000, 0xAABBCCDDu);
  auto r32 = load(make_fetch({.format = XenosFormat::k_8_8_8_8, .w = 1, .endian = 2, .base = 0x2000}), mem);
  REQUIRE(r32.ok());
  CHECK(le32(r32.value().bytes.data()) == 0xAABBCCDDu);
  auto r8 = load(make_fetch({.format = XenosFormat::k_8, .w = 4, .endian = 2, .base = 0x2000}), mem);
  REQUIRE(r8.ok());
  CHECK(r8.value().bytes == std::vector<uint8_t>{0xDD, 0xCC, 0xBB, 0xAA});
  auto r1616 = load(make_fetch({.format = XenosFormat::k_16_16, .w = 1, .endian = 3, .base = 0x2000}), mem);
  REQUIRE(r1616.ok());
  CHECK(le32(r1616.value().bytes.data()) == 0xBBAADDCCu);  // memory word 0xDDCCBBAA (LE), halves swapped
}

TEST_CASE("CTX1 decode", "[unit][gpu]") {
  // Endpoint 0 = (R 0x90, G 0x30), endpoint 1 = (R 0x30, G 0xF0); R in the high byte of each half.
  const uint32_t word0 = 0x30F09030u;
  const uint32_t word1 = 0u | (1u << 2) | (2u << 4) | (3u << 6);  // texels 0..3: codes 0, 1, 2, 3; the rest 0
  const auto t = lr::gpu::decode_ctx1_block(word0, word1);
  CHECK(t[0] == std::array<uint8_t, 2>{0x90, 0x30});
  CHECK(t[1] == std::array<uint8_t, 2>{0x30, 0xF0});
  CHECK(t[2] == std::array<uint8_t, 2>{0x70, 0x70});  // (2 e0 + e1) / 3
  CHECK(t[3] == std::array<uint8_t, 2>{0x50, 0xB0});  // (e0 + 2 e1) / 3
  CHECK(t[15] == std::array<uint8_t, 2>{0x90, 0x30});

  // A 4x4 linear CTX1 texture (8in32 words in guest memory) decodes to R8G8 texels row-major; texel (1, 0) uses the
  // code at bits 2-3.
  Guest g(1 << 16);
  g.put_be32(0x1000, word0);
  g.put_be32(0x1004, word1 | (1u << 30));  // texel 15 -> endpoint 1
  auto r = load(make_fetch({.format = XenosFormat::k_CTX1, .w = 4, .h = 4, .endian = 2, .base = 0x1000}), g.access());
  REQUIRE(r.ok());
  const std::vector<uint8_t>& b = r.value().bytes;
  REQUIRE(b.size() == 32);
  CHECK(b[0] == 0x90);
  CHECK(b[1] == 0x30);
  CHECK(b[2] == 0x30);
  CHECK(b[3] == 0xF0);
  CHECK(b[30] == 0x30);
  CHECK(b[31] == 0xF0);
}

TEST_CASE("packed mips sit in the tail of the last unpacked level", "[unit][gpu]") {
  // A 16x16 texture: everything is in the tail (packed level 0): base at (16, 0), 8x8 at (8, 0), 4x4 at (4, 0),
  // then 2x2 at (0, 8) and 1x1 at (0, 4) along the other axis (the layout diagram in Xenia's texture_util.cc).
  CHECK(lr::gpu::packed_mip_level(16, 16) == 0);
  const std::array<std::array<uint32_t, 2>, 5> want{{{16, 0}, {8, 0}, {4, 0}, {0, 8}, {0, 4}}};
  for (uint32_t level = 0; level < 5; ++level) {
    uint32_t x = 99, y = 99, z = 99;
    CHECK(lr::gpu::packed_mip_offset(16, 16, 1, 1, 1, level, x, y, z));
    CHECK(std::array<uint32_t, 2>{x, y} == want[level]);
  }
  // 256x64: levels with the shorter side above 16 are not packed; the tail starts at level 2 (64 >> 2 = 16) and is
  // wider than tall, so the slots go down y.
  CHECK(lr::gpu::packed_mip_level(256, 64) == 2);
  uint32_t x = 0, y = 0, z = 0;
  CHECK_FALSE(lr::gpu::packed_mip_offset(256, 64, 1, 1, 1, 1, x, y, z));
  CHECK(lr::gpu::packed_mip_offset(256, 64, 1, 1, 1, 2, x, y, z));
  CHECK((x == 0 && y == 16));
  CHECK(lr::gpu::packed_mip_offset(256, 64, 1, 1, 1, 3, x, y, z));
  CHECK((x == 0 && y == 8));
  // Compressed: offsets are in blocks (texels / 4).
  CHECK(lr::gpu::packed_mip_offset(16, 16, 1, 4, 4, 1, x, y, z));
  CHECK((x == 2 && y == 0));

  // Through the loader: an 8x8 k_8_8_8_8 tiled texture with packed mips reads its base from (16, 0) of the tail.
  Guest g(1 << 20);
  for (uint32_t ty = 0; ty < 8; ++ty)
    for (uint32_t tx = 0; tx < 8; ++tx)
      g.put_be32(0x10000 + lr::testing::xenos_tiled_offset_2d(16 + tx, ty, 32, 2), 0x01000000u * (tx + 8 * ty + 1));
  auto r = load(make_fetch({.w = 8, .h = 8, .tiled = true, .packed = true, .endian = 2, .base = 0x10000}), g.access());
  REQUIRE(r.ok());
  CHECK(le32(r.value().bytes.data()) == 0x01000000u);
  CHECK(le32(r.value().bytes.data() + 4 * 63) == 0x40000000u);
}

TEST_CASE("mip range follows the fetch constant", "[unit][gpu]") {
  auto range = [](uint32_t base, uint32_t mip, uint32_t lo, uint32_t hi) {
    return lr::gpu::mip_range(make_fetch({.w = 64, .h = 64, .base = base, .mip = mip, .mip_min = lo, .mip_max = hi}));
  };
  auto a = range(0x1000, 0, 2, 5);  // no mip address: base only
  CHECK((a.min_level == 0 && a.max_level == 0 && a.has_base && !a.has_mips));
  auto b = range(0x1000, 0x2000, 0, 15);  // clamped to log2(64) = 6
  CHECK((b.min_level == 0 && b.max_level == 6 && b.has_base && b.has_mips));
  auto c = range(0, 0x2000, 0, 3);  // no base: starts at 1
  CHECK((c.min_level == 1 && c.max_level == 3 && !c.has_base && c.has_mips));
  auto d = range(0x1000, 0x2000, 2, 3);  // min above 0 drops the base
  CHECK((d.min_level == 2 && !d.has_base));
}

TEST_CASE("edge-size and unsupported-format textures", "[unit][gpu]") {
  SECTION("unsupported and signed formats are named errors") {
    auto bad = lr::gpu::describe_texture(make_fetch({.format = XenosFormat::k_1_REVERSE, .base = 0x1000}));
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().message == "texture format k_1_REVERSE (0) is not supported");
    auto yuv = lr::gpu::describe_texture(make_fetch({.format = XenosFormat::k_Y1_Cr_Y0_Cb_REP, .base = 0x1000}));
    REQUIRE_FALSE(yuv.ok());
    CHECK(yuv.error().message.find("k_Y1_Cr_Y0_Cb_REP") != std::string::npos);
    auto sgn = lr::gpu::describe_texture(make_fetch({.base = 0x1000, .signs = 0x55}));
    REQUIRE_FALSE(sgn.ok());
    CHECK(sgn.error().message.find("signed texture formats are not supported in M3 (k_8_8_8_8") != std::string::npos);
    // A sign on a component the format does not have does not matter.
    CHECK(lr::gpu::describe_texture(make_fetch({.format = XenosFormat::k_8, .base = 0x1000, .signs = 0x54})).ok());
  }
  SECTION("empty and unbound fetch constants are the null texture") {
    TextureFetch empty;
    auto e = lr::gpu::describe_texture(empty);
    REQUIRE(e.ok());
    CHECK(e.value().null);
    auto no_address = lr::gpu::describe_texture(make_fetch({.w = 4, .h = 4}));
    REQUIRE(no_address.ok());
    CHECK(no_address.value().null);
  }
  SECTION("1x1 and 1000x3, linear and tiled") {
    Guest g(1 << 20);
    g.put_be32(0x1000, 0x11223344u);
    auto one = load(make_fetch({.w = 1, .h = 1, .endian = 2, .base = 0x1000}), g.access());
    REQUIRE(one.ok());
    CHECK(one.value().bytes.size() == 4);
    CHECK(le32(one.value().bytes.data()) == 0x11223344u);
    for (const bool tiled : {false, true}) {
      INFO((tiled ? "tiled" : "linear"));
      for (uint32_t y = 0; y < 3; ++y)
        for (uint32_t x = 0; x < 1000; ++x) {
          const uint32_t off = tiled ? lr::testing::xenos_tiled_offset_2d(x, y, 1024, 2) : (y * 1024 + x) * 4;
          g.put_be32(0x10000 + off, (x << 8) | y);
        }
      auto r = load(make_fetch({.w = 1000, .h = 3, .tiled = tiled, .endian = 2, .base = 0x10000}), g.access());
      REQUIRE(r.ok());
      REQUIRE(r.value().bytes.size() == 1000 * 3 * 4);
      uint32_t wrong = 0;
      for (uint32_t y = 0; y < 3; ++y)
        for (uint32_t x = 0; x < 1000; ++x) wrong += le32(r.value().bytes.data() + (y * 1000 + x) * 4) != ((x << 8) | y);
      CHECK(wrong == 0);
    }
  }
  SECTION("8192x8192 RGBA8 within memory") {
    constexpr uint32_t kSide = 8192, kBase = 0x1000;
    Guest g(size_t{kSide} * kSide * 4 + kBase);
    for (uint32_t y = 0; y < kSide; y += 1021)
      for (uint32_t x = 0; x < kSide; x += 3) g.put_be32(kBase + (size_t{y} * kSide + x) * 4, (x << 13) ^ y ^ 0x80000000u);
    auto r = load(make_fetch({.w = kSide, .h = kSide, .endian = 2, .base = kBase}), g.access());
    REQUIRE(r.ok());
    REQUIRE(r.value().bytes.size() == size_t{kSide} * kSide * 4);
    uint32_t wrong = 0;
    for (uint32_t y = 0; y < kSide; y += 1021)
      for (uint32_t x = 0; x < kSide; x += 3)
        wrong += le32(r.value().bytes.data() + (size_t{y} * kSide + x) * 4) != ((x << 13) ^ y ^ 0x80000000u);
    CHECK(wrong == 0);
    CHECK(le32(r.value().bytes.data() + 4) == 0);  // never written
  }
  SECTION("8192x8192 past the end of memory reads zeros, never past it") {
    constexpr uint32_t kSide = 8192;
    Guest g(1 << 20, 1 << 20);
    g.put_be32(0x80000, 0xCAFEF00Du);
    auto r = load(make_fetch({.w = kSide, .h = kSide, .endian = 2, .base = 0x80000}), g.access());
    REQUIRE(r.ok());
    CHECK(le32(r.value().bytes.data()) == 0xCAFEF00Du);
    CHECK(le32(r.value().bytes.data() + (size_t{kSide} * 1000) * 4) == 0);
    auto desc = lr::gpu::describe_texture(make_fetch({.w = kSide, .h = kSide, .base = 0x80000}));
    auto fp = lr::gpu::texture_footprint(desc.value(), g.access());
    REQUIRE(fp.ok());
    REQUIRE(fp.value().base.has_value());
    CHECK(fp.value().base->size == 0x80000);  // clamped to the allocation
  }
  SECTION("too large for the host, or not in guest memory, is a named error") {
    Guest g(1 << 20, 1 << 20);
    auto huge = load(make_fetch({.format = XenosFormat::k_32_32_32_32_FLOAT, .w = 8192, .h = 8192, .base = 0x1000}),
                     g.access());
    REQUIRE_FALSE(huge.ok());
    CHECK(huge.error().message.find("k_32_32_32_32_FLOAT 8192x8192") != std::string::npos);
    CHECK(huge.error().message.find("MiB of host memory") != std::string::npos);
    auto outside = load(make_fetch({.w = 4, .h = 4, .base = 0x200000}), g.access());
    REQUIRE_FALSE(outside.ok());
    CHECK(outside.error().message.find("is not in guest memory") != std::string::npos);
    Guest small(1 << 20, 0x8000);
    auto unallocated = load(make_fetch({.w = 4, .h = 4, .base = 0x9000}), small.access());
    REQUIRE_FALSE(unallocated.ok());
    CHECK(unallocated.error().message.find("is not in a physical allocation") != std::string::npos);
  }
}

TEST_CASE("tiled front buffer decodes with the fetch constant's format", "[unit][gpu]") {
  // 64x40 A8R8G8B8 (D3D: k_8_8_8_8, 8in32, swizzle ZYXW), tiled at pitch 64, rows allocated to 64.
  Guest g(1 << 20);
  for (uint32_t y = 0; y < 40; ++y)
    for (uint32_t x = 0; x < 64; ++x)
      g.put_be32(0x20000 + lr::testing::xenos_tiled_offset_2d(x, y, 64, 2), 0x7F000000u | (x << 16) | (y << 8) | 0xC0u);
  const TextureFetch f = make_fetch({.w = 64, .h = 40, .tiled = true, .endian = 2, .base = 0x55000, .swizzle = kZYXW});
  auto rgba = lr::gpu::front_buffer_rgba8(f, 0x20000, 64, 40, g.access());
  REQUIRE(rgba.ok());
  uint32_t wrong = 0;
  for (uint32_t y = 0; y < 40; ++y)
    for (uint32_t x = 0; x < 64; ++x) {
      const uint8_t* p = rgba.value().data() + (y * 64 + x) * 4;
      wrong += (p[0] == x && p[1] == y && p[2] == 0xC0 && p[3] == 0xFF) ? 0 : 1;
    }
  CHECK(wrong == 0);
  auto dxt = lr::gpu::front_buffer_rgba8(make_fetch({.format = XenosFormat::k_DXT1, .w = 64, .h = 40}), 0x20000, 64,
                                         40, g.access());
  REQUIRE_FALSE(dxt.ok());
  CHECK(dxt.error().message == "front buffer format k_DXT1 is not supported");
}

TEST_CASE("format table", "[unit][gpu]") {
  CHECK(std::string(lr::gpu::format_info(XenosFormat::k_1_REVERSE).name) == "k_1_REVERSE");
  CHECK(std::string(lr::gpu::format_info(XenosFormat::k_DXN).name) == "k_DXN");
  CHECK(std::string(lr::gpu::format_info(XenosFormat::k_CTX1).name) == "k_CTX1");
  CHECK(std::string(lr::gpu::format_info(XenosFormat::k_2_10_10_10_FLOAT_EDRAM).name) == "k_2_10_10_10_FLOAT_EDRAM");
  CHECK(vk::host_format(XenosFormat::k_DXT1) == VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
  CHECK(vk::host_format(XenosFormat::k_DXT2_3) == VK_FORMAT_BC2_UNORM_BLOCK);
  CHECK(vk::host_format(XenosFormat::k_DXT4_5) == VK_FORMAT_BC3_UNORM_BLOCK);
  CHECK(vk::host_format(XenosFormat::k_DXN) == VK_FORMAT_BC5_UNORM_BLOCK);
  CHECK(vk::host_format(XenosFormat::k_DXT5A) == VK_FORMAT_BC4_UNORM_BLOCK);
  CHECK(vk::host_format(XenosFormat::k_CTX1) == VK_FORMAT_R8G8_UNORM);
  CHECK(vk::host_format(XenosFormat::k_5_6_5) == VK_FORMAT_R8G8B8A8_UNORM);
  CHECK(vk::host_format(XenosFormat::k_8_8_8_8) == VK_FORMAT_R8G8B8A8_UNORM);
  CHECK(vk::host_format(XenosFormat::k_16_16_16_16_FLOAT) == VK_FORMAT_R16G16B16A16_SFLOAT);
  CHECK_FALSE(vk::host_format(XenosFormat::k_24_8).has_value());
  CHECK(vk::srgb_format(VK_FORMAT_BC1_RGBA_UNORM_BLOCK) == VK_FORMAT_BC1_RGBA_SRGB_BLOCK);
  uint32_t supported = 0;
  for (uint32_t i = 0; i < 64; ++i) supported += lr::gpu::format_supported(XenosFormat(i)) ? 1 : 0;
  CHECK(supported == 36);

  // 16-bit packed formats become RGBA8 with X in red.
  Guest g(1 << 16);
  g.bytes[0x1000] = 0xF8;  // k_5_6_5 0xF81F big-endian halves: X = 0x1F, Y = 0, Z = 0x1F
  g.bytes[0x1001] = 0x1F;
  auto r = load(make_fetch({.format = XenosFormat::k_5_6_5, .w = 1, .endian = 1, .base = 0x1000}), g.access());
  REQUIRE(r.ok());
  CHECK(r.value().bytes == std::vector<uint8_t>{0xFF, 0x00, 0xFF, 0xFF});
}

TEST_CASE("fetch constant swizzle maps to the view", "[unit][gpu]") {
  using M = VkComponentMapping;
  auto eq = [](const M& a, const M& b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; };
  CHECK(eq(vk::view_component_mapping(XenosFormat::k_8_8_8_8, kZYXW),
           M{VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A}));
  CHECK(eq(vk::view_component_mapping(XenosFormat::k_8, kXYZW),
           M{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R}));
  CHECK(eq(vk::view_component_mapping(XenosFormat::k_CTX1, kXYZW),
           M{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_G}));
  const uint32_t zero_one = 4 | (5u << 3) | (3u << 6) | (0u << 9);  // 0, 1, W, X
  CHECK(eq(vk::view_component_mapping(XenosFormat::k_8_8_8_8, zero_one),
           M{VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_R}));
}

// --- samplers --------------------------------------------------------------------------------------------------------

TEST_CASE("sampler fields come from the fetch constant", "[unit][gpu]") {
  vk::SamplerLimits limits;
  limits.anisotropy = true;
  limits.max_anisotropy = 8.0f;
  limits.max_lod_bias = 15.0f;
  limits.mirror_clamp_to_edge = true;
  FetchSpec s{.dim = TextureDimension::k3D, .w = 64, .h = 64, .d = 8, .base = 0x1000, .mip = 0x20000, .mip_min = 1,
              .mip_max = 3};
  s.clamp = {ClampMode::kRepeat, ClampMode::kMirrorClampToEdge, ClampMode::kClampToBorder};
  s.mag = 1;          // linear
  s.min = 0;          // point
  s.mip_filter = 1;   // linear
  s.aniso = 5;        // 16:1, clamped to 8
  s.border = 1;       // ABGR white
  s.lod_bias = -16;   // -0.5
  const VkSamplerCreateInfo c = vk::sampler_create_info(make_fetch(s), limits);
  CHECK(c.addressModeU == VK_SAMPLER_ADDRESS_MODE_REPEAT);
  CHECK(c.addressModeV == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE);
  CHECK(c.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
  CHECK(c.magFilter == VK_FILTER_LINEAR);
  CHECK(c.minFilter == VK_FILTER_NEAREST);
  CHECK(c.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR);
  CHECK(c.anisotropyEnable == VK_TRUE);
  CHECK(c.maxAnisotropy == 8.0f);
  CHECK(c.minLod == 1.0f);
  CHECK(c.maxLod == 3.0f);
  CHECK(c.mipLodBias == -0.5f);
  CHECK(c.borderColor == VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE);
  CHECK(c.unnormalizedCoordinates == VK_FALSE);

  // Without the optional device features: mirror once falls back to mirrored repeat, anisotropy is off.
  const VkSamplerCreateInfo plain = vk::sampler_create_info(make_fetch(s), vk::SamplerLimits{});
  CHECK(plain.addressModeV == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT);
  CHECK(plain.anisotropyEnable == VK_FALSE);
  CHECK(plain.mipLodBias == 0.0f);  // device max bias 0

  // 2D: z is unused (clamp to edge, no border); the border colour then does not matter; base map: one level.
  FetchSpec t{.w = 64, .h = 64, .base = 0x1000, .mip = 0x20000, .mip_min = 0, .mip_max = 6};
  t.clamp = {ClampMode::kClampToHalfway, ClampMode::kMirrorClampToBorder, ClampMode::kClampToBorder};
  t.border = 1;
  t.mip_filter = 2;  // base map
  const VkSamplerCreateInfo two_d = vk::sampler_create_info(make_fetch(t), limits);
  CHECK(two_d.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
  CHECK(two_d.addressModeV == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE);
  CHECK(two_d.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
  CHECK(two_d.borderColor == VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);
  CHECK(two_d.maxLod == two_d.minLod);
  t.clamp[0] = ClampMode::kClampToBorder;
  t.border = 2;  // a YCbCr black
  CHECK(vk::sampler_create_info(make_fetch(t), limits).borderColor == VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK);
  t.border = 0;
  CHECK(vk::sampler_create_info(make_fetch(t), limits).borderColor == VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);
}

TEST_CASE("equal sampler states share one heap entry", "[unit][vulkan]") {
  auto h = lr::testing::ShaderHarness::create();
  if (!h.ok()) SKIP("no Vulkan ICD installed");
  ValidationErrors vk_errors;
  {
    auto cache = vk::SamplerCache::create(h.value().device());
    REQUIRE(cache.ok());
    vk::SamplerCache& sc = *cache.value();
    FetchSpec a{.w = 64, .h = 64, .base = 0x1000};
    a.mag = a.min = 1;
    FetchSpec b = a;
    b.format = XenosFormat::k_DXT1;  // texture fields differ, sampler state does not
    b.base = 0x9000;
    b.swizzle = kZYXW;
    FetchSpec c = a;
    c.clamp[0] = ClampMode::kClampToEdge;
    const uint32_t ia = sc.heap_index(make_fetch(a));
    CHECK(ia != 0);
    CHECK(sc.heap_index(make_fetch(b)) == ia);
    const uint32_t ic = sc.heap_index(make_fetch(c));
    CHECK(ic != 0);
    CHECK(ic != ia);
    CHECK(sc.heap_index(make_fetch(a)) == ia);
    CHECK(sc.size() == 2);
    CHECK(sc.set() != VK_NULL_HANDLE);
    CHECK(sc.set_layout() != VK_NULL_HANDLE);
  }
  CHECK(vk_errors.errors.empty());
}

// --- the Vulkan texture cache ----------------------------------------------------------------------------------------

TEST_CASE("DXT1 tiled texture uploads correctly", "[unit][vulkan]") {
  auto h = lr::testing::ShaderHarness::create();
  if (!h.ok()) SKIP("no Vulkan ICD installed");
  ValidationErrors vk_errors;
  {
    // 64x64 DXT1: 16x16 blocks of 8 bytes, tiled (pitch 32 blocks), 8in16 (big-endian 16-bit words).
    Guest g(1 << 20);
    constexpr uint32_t kBase = 0x40000;
    std::vector<uint8_t> want(16 * 16 * 8);
    for (uint32_t by = 0; by < 16; ++by)
      for (uint32_t bx = 0; bx < 16; ++bx) {
        uint8_t block[8];
        for (uint32_t i = 0; i < 8; ++i) block[i] = uint8_t(bx * 16 + by + i * 37);
        std::memcpy(want.data() + (by * 16 + bx) * 8, block, 8);  // the BC1 bytes the host should get
        const uint32_t at = kBase + lr::testing::xenos_tiled_offset_2d(bx, by, 32, 3);
        for (uint32_t i = 0; i < 8; i += 2) {  // guest: each 16-bit word big-endian
          g.bytes[at + i] = block[i + 1];
          g.bytes[at + i + 1] = block[i];
        }
      }
    auto cache = vk::TextureCache::create(h.value().device(), g.access());
    REQUIRE(cache.ok());
    vk::TextureCache& tc = *cache.value();
    const TextureFetch f =
        make_fetch({.format = XenosFormat::k_DXT1, .w = 64, .h = 64, .tiled = true, .endian = 1, .base = kBase});
    auto view = tc.get(f);
    INFO((view.ok() ? std::string("ok") : view.error().message));
    REQUIRE(view.ok());
    CHECK(view.value() != VK_NULL_HANDLE);
    CHECK(tc.textures_created() == 1);
    auto bytes = tc.download(f, 0);
    REQUIRE(bytes.ok());
    CHECK(bytes.value() == want);
  }
  CHECK(vk_errors.errors.empty());
}

TEST_CASE("1D texture uploads as a height-1 2D image", "[unit][vulkan]") {
  auto h = lr::testing::ShaderHarness::create();
  if (!h.ok()) SKIP("no Vulkan ICD installed");
  ValidationErrors vk_errors;
  {
    Guest g(1 << 20);
    for (uint32_t x = 0; x < 100; ++x) g.put_be32(0x3000 + x * 4, 0x01020304u * (x + 1));
    auto cache = vk::TextureCache::create(h.value().device(), g.access());
    REQUIRE(cache.ok());
    vk::TextureCache& tc = *cache.value();
    const TextureFetch f = make_fetch({.dim = TextureDimension::k1D, .w = 100, .endian = 2, .base = 0x3000});
    auto bound = tc.bind(3, f);
    INFO((bound.ok() ? std::string("ok") : bound.error().message));
    REQUIRE(bound.ok());
    const vk::TextureHeapIndices idx = tc.heap_index(3);
    CHECK(idx.texture_2d != 0);
    CHECK(idx.texture_3d == 0);
    CHECK(idx.texture_cube == 0);
    CHECK(tc.heap_index(4) == vk::TextureHeapIndices{});  // never bound
    auto bytes = tc.download(f, 0);
    REQUIRE(bytes.ok());
    REQUIRE(bytes.value().size() == 400);
    uint32_t wrong = 0;
    for (uint32_t x = 0; x < 100; ++x) wrong += le32(bytes.value().data() + x * 4) != 0x01020304u * (x + 1);
    CHECK(wrong == 0);

    // An empty fetch constant binds the null textures; an unsupported format is an error naming it.
    REQUIRE(tc.bind(5, TextureFetch{}).ok());
    CHECK(tc.heap_index(5) == vk::TextureHeapIndices{});
    auto bad = tc.bind(6, make_fetch({.format = XenosFormat::k_24_8, .w = 4, .h = 4, .base = 0x3000}));
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().message == "texture format k_24_8 (22) is not supported");
    CHECK(tc.heap_index(6) == vk::TextureHeapIndices{});
    REQUIRE(tc.get(TextureFetch{}).ok());

    // A cube and a 3D texture land in their own heaps.
    REQUIRE(tc.bind(7, make_fetch({.dim = TextureDimension::kCube, .w = 4, .h = 4, .d = 6, .base = 0x8000})).ok());
    CHECK(tc.heap_index(7).texture_cube != 0);
    REQUIRE(tc.bind(8, make_fetch({.dim = TextureDimension::k3D, .w = 4, .h = 4, .d = 4, .base = 0x10000})).ok());
    CHECK(tc.heap_index(8).texture_3d != 0);
    // 1x1 and 1000x3 through the device.
    REQUIRE(tc.get(make_fetch({.w = 1, .h = 1, .base = 0x20000})).ok());
    REQUIRE(tc.get(make_fetch({.w = 1000, .h = 3, .tiled = true, .base = 0x40000})).ok());
    // A mipped DXT1 texture (64x64, 7 levels, packed tail) uploads every level.
    REQUIRE(tc.get(make_fetch({.format = XenosFormat::k_DXT1, .w = 64, .h = 64, .tiled = true, .packed = true,
                               .base = 0x60000, .mip = 0x70000, .mip_min = 0, .mip_max = 6}))
                .ok());
  }
  CHECK(vk_errors.errors.empty());
}

TEST_CASE("fetch constant swizzle selects one image and two views", "[unit][vulkan]") {
  auto h = lr::testing::ShaderHarness::create();
  if (!h.ok()) SKIP("no Vulkan ICD installed");
  ValidationErrors vk_errors;
  {
    Guest g(1 << 20);
    auto cache = vk::TextureCache::create(h.value().device(), g.access());
    REQUIRE(cache.ok());
    vk::TextureCache& tc = *cache.value();
    REQUIRE(tc.bind(0, make_fetch({.w = 8, .h = 8, .base = 0x1000, .swizzle = kXYZW})).ok());
    REQUIRE(tc.bind(1, make_fetch({.w = 8, .h = 8, .base = 0x1000, .swizzle = kZYXW})).ok());
    CHECK(tc.image_count() == 1);
    CHECK(tc.view_count() == 2);
    CHECK(tc.heap_index(0).texture_2d != tc.heap_index(1).texture_2d);
    REQUIRE(tc.bind(2, make_fetch({.w = 8, .h = 8, .base = 0x1000, .swizzle = kZYXW})).ok());
    CHECK(tc.heap_index(2) == tc.heap_index(1));
    CHECK(tc.view_count() == 2);
  }
  CHECK(vk_errors.errors.empty());
}

TEST_CASE("modified guest memory invalidates the texture", "[unit][vulkan]") {
  auto h = lr::testing::ShaderHarness::create();
  if (!h.ok()) SKIP("no Vulkan ICD installed");
  ValidationErrors vk_errors;
  {
    Guest g(1 << 20);
    g.put_be32(0x5000, 0x11111111u);
    auto cache = vk::TextureCache::create(h.value().device(), g.access());
    REQUIRE(cache.ok());
    vk::TextureCache& tc = *cache.value();
    const TextureFetch f = make_fetch({.w = 4, .h = 4, .endian = 2, .base = 0x5000});
    REQUIRE(tc.get(f).ok());
    CHECK(tc.uploads() == 1);
    auto first = [&] {
      auto b = tc.download(f, 0);
      REQUIRE(b.ok());
      return le32(b.value().data());
    };
    CHECK(first() == 0x11111111u);

    // Same frame: the bytes are not hashed again.
    g.put_be32(0x5000, 0x22222222u);
    REQUIRE(tc.get(f).ok());
    CHECK(tc.uploads() == 1);
    // Next frame: the hash differs, one re-upload.
    tc.begin_frame();
    REQUIRE(tc.get(f).ok());
    CHECK(tc.uploads() == 2);
    CHECK(first() == 0x22222222u);
    // Next frame, nothing changed: no upload.
    tc.begin_frame();
    REQUIRE(tc.get(f).ok());
    CHECK(tc.uploads() == 2);
    // A write the GPU reports (a resolve) is seen in the same frame.
    g.put_be32(0x5000, 0x33333333u);
    tc.invalidate_range(0x5000, 4);
    REQUIRE(tc.get(f).ok());
    CHECK(tc.uploads() == 3);
    CHECK(first() == 0x33333333u);
    // A range outside the texture's footprint does not.
    tc.invalidate_range(0x90000, 0x1000);
    REQUIRE(tc.get(f).ok());
    CHECK(tc.uploads() == 3);
    CHECK(tc.image_count() == 1);
    CHECK(tc.textures_created() == 1);
  }
  CHECK(vk_errors.errors.empty());
}

// --- fix round 1 -------------------------------------------------------------------------------------------------------

TEST_CASE("tiled 3D offset matches hand-worked values", "[unit][gpu]") {
  // pitch 32, height 32, 4-byte blocks. (1, 0, 1): micro z[1:0] << 5 | x = 33, o = 132; o[7:5] = 4 -> bit 8..10,
  // o[3:0] = 4: 1024 + 4. (0, 0, 4): second z slab = macro 2 (two 16-row macro tiles per 32 rows), o = 1024 ->
  // o >> 8 = 4 at bit 12; bank y[3] ^ z[2] = 1 (bit 11); pipe = bank << 1 = 2 (bits 6-7): 16384 + 2048 + 128.
  // (8, 8, 0): bank 1, pipe x[4:3] ^ 2 = 3: 2048 + 192.
  CHECK(lr::gpu::tiled_offset_3d(0, 0, 0, 32, 32, 4) == 0);
  CHECK(lr::gpu::tiled_offset_3d(1, 0, 1, 32, 32, 4) == 1028);
  CHECK(lr::gpu::tiled_offset_3d(0, 0, 4, 32, 32, 4) == 18560);
  CHECK(lr::gpu::tiled_offset_3d(8, 8, 0, 32, 32, 4) == 2240);
  // Past 4 GiB without wrapping: the last 16-byte block of a 2048x2048x1024 volume.
  CHECK(lr::gpu::tiled_offset_3d(2047, 2047, 1023, 2048, 2048, 16) > (uint64_t{1} << 32));
}

TEST_CASE("packed 16 and 32-bit formats convert to their host values", "[unit][gpu]") {
  Guest g(1 << 16);
  const MemoryAccess mem = g.access();
  auto halves = [&](uint32_t at, uint16_t v) {  // a 16-bit texel, big-endian (read with 8in16)
    g.bytes[at] = uint8_t(v >> 8);
    g.bytes[at + 1] = uint8_t(v);
  };
  // k_1_5_5_5: X 31, Y 0, Z 16, W 1 -> 255, 0, round(16 * 255 / 31) = 132, 255.
  halves(0x1000, uint16_t(31 | (0 << 5) | (16 << 10) | (1 << 15)));
  auto r1555 = load(make_fetch({.format = XenosFormat::k_1_5_5_5, .w = 1, .endian = 1, .base = 0x1000}), mem);
  REQUIRE(r1555.ok());
  CHECK(r1555.value().bytes == std::vector<uint8_t>{255, 0, 132, 255});
  // k_4_4_4_4: X 15, Y 8, Z 0, W 3 -> 255, 136, 0, 51.
  halves(0x2000, uint16_t(15 | (8 << 4) | (0 << 8) | (3 << 12)));
  auto r4444 = load(make_fetch({.format = XenosFormat::k_4_4_4_4, .w = 1, .endian = 1, .base = 0x2000}), mem);
  REQUIRE(r4444.ok());
  CHECK(r4444.value().bytes == std::vector<uint8_t>{255, 136, 0, 51});
  auto u16 = [](const std::vector<uint8_t>& b) {
    std::array<uint16_t, 4> v{};
    std::memcpy(v.data(), b.data(), 8);
    return v;
  };
  // k_10_11_11: X 11 bits 2047, Y 11 bits 0, Z 10 bits 512 -> 65535, 0, round(512 * 65535 / 1023) = 32800, A 65535.
  g.put_be32(0x3000, 2047u | (0u << 11) | (512u << 22));
  auto r101111 = load(make_fetch({.format = XenosFormat::k_10_11_11, .w = 1, .endian = 2, .base = 0x3000}), mem);
  REQUIRE(r101111.ok());
  CHECK(u16(r101111.value().bytes) == std::array<uint16_t, 4>{65535, 0, 32800, 65535});
  // k_11_11_10: X 10 bits 1023, Y 11 bits 1024, Z 11 bits 0 -> 65535, round(1024 * 65535 / 2047) = 32784, 0, 65535.
  g.put_be32(0x4000, 1023u | (1024u << 10) | (0u << 21));
  auto r111110 = load(make_fetch({.format = XenosFormat::k_11_11_10, .w = 1, .endian = 2, .base = 0x4000}), mem);
  REQUIRE(r111110.ok());
  CHECK(u16(r111110.value().bytes) == std::array<uint16_t, 4>{65535, 32784, 0, 65535});
}

TEST_CASE("a guest write during a load is never left stale", "[unit][vulkan]") {
  auto h = lr::testing::ShaderHarness::create();
  if (!h.ok()) SKIP("no Vulkan ICD installed");
  ValidationErrors vk_errors;
  {
    Guest g(1 << 20);
    g.put_be32(0x6000, 0xAAAAAAAAu);
    auto cache = vk::TextureCache::create(h.value().device(), g.access());
    REQUIRE(cache.ok());
    vk::TextureCache& tc = *cache.value();
    const TextureFetch f = make_fetch({.w = 4, .h = 4, .endian = 2, .base = 0x6000});
    // The guest writes between the hash and the read of the first load.
    bool written = false;
    tc.set_after_hash_hook([&] {
      if (written) return;
      g.put_be32(0x6000, 0xBBBBBBBBu);
      written = true;
    });
    REQUIRE(tc.get(f).ok());
    CHECK(tc.uploads() == 1);
    auto texel = [&] {
      auto b = tc.download(f, 0);
      REQUIRE(b.ok());
      return le32(b.value().data());
    };
    CHECK(texel() == 0xBBBBBBBBu);  // the load read the new bytes
    // The hash was of the old bytes: the next frame uploads once more, then stays put.
    tc.begin_frame();
    REQUIRE(tc.get(f).ok());
    CHECK(tc.uploads() == 2);
    CHECK(texel() == 0xBBBBBBBBu);
    tc.begin_frame();
    REQUIRE(tc.get(f).ok());
    CHECK(tc.uploads() == 2);
  }
  CHECK(vk_errors.errors.empty());
}
