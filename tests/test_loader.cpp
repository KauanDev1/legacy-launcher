// SPDX-License-Identifier: MIT
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

#include "runtime/kernel/guest_structs.h"
#include "runtime/memory_map.h"
#include "support/kernel_fixture.h"
#include "support/xex2_builder.h"

namespace {
std::vector<std::byte> loadable_xex(lr::testing::Xex2Builder b) {
  std::vector<uint8_t> text(0x100, 0), data(0x100, 0);
  return b.entry_point(0x82001000)
      .image_base(0x82000000)
      .pe_image(0x82000000, 0x3000, {{".text", 0x1000, text, true}, {".data", 0x2000, data, false}})
      .build();
}
}  // namespace

TEST_CASE("loader copies header to kernel data", "[unit]") {
  lr::testing::KernelFixture f;
  const auto xex = loadable_xex(std::move(lr::testing::Xex2Builder().tls(8, 0x82002000, 0x40, 0x20).stack_size(0x80000)));
  auto r = load_image(f.mem, xex);
  REQUIRE(r.ok());
  const LoadedImage& img = r.value();
  CHECK(img.base == 0x82000000u);
  CHECK(img.entry == 0x82001000u);
  CHECK(img.header_guest == kKernelDataBase);
  const uint32_t header_size = (static_cast<uint32_t>(xex[8]) << 24) | (static_cast<uint32_t>(xex[9]) << 16) |
                               (static_cast<uint32_t>(xex[10]) << 8) | static_cast<uint32_t>(xex[11]);
  REQUIRE(header_size > 0x18);
  CHECK(std::memcmp(f.mem.translate<uint8_t>(img.header_guest), xex.data(), header_size) == 0);

  REQUIRE(img.ldr_entry >= kKernelDataBase + header_size);
  REQUIRE(img.ldr_entry < kKernelDataEnd);
  const auto* ldr = f.mem.translate<lr::kernel::X_LDR_DATA_TABLE_ENTRY>(img.ldr_entry);
  CHECK(ldr->xex_header_base == kKernelDataBase);
  CHECK(ldr->dll_base == 0x82000000u);
  CHECK(ldr->image_base == 0x82000000u);
  CHECK(ldr->image_size == img.size);
  CHECK(ldr->entry_point == 0x82001000u);
  CHECK(ldr->in_load_order_links.flink == img.ldr_entry);  // the only module: list points back at itself

  REQUIRE(img.tls.has_value());
  CHECK(img.tls->slot_count == 8u);
  CHECK(img.tls->raw_data_address == 0x82002000u);
  CHECK(img.stack_size == 0x80000u);
}

TEST_CASE("loader defaults the stack size to 256 KiB", "[unit]") {
  lr::testing::KernelFixture f;
  auto r = load_image(f.mem, loadable_xex(lr::testing::Xex2Builder()));
  REQUIRE(r.ok());
  CHECK(r.value().stack_size == 256u * 1024u);
  CHECK_FALSE(r.value().tls.has_value());
}
