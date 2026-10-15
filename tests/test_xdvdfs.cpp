// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>

#include "formats/reader.h"
#include "formats/xdvdfs.h"
#include "support/xdvdfs_builder.h"
#include "support/temp_dir.h"

using namespace lr;
using namespace lr::formats;
using namespace lr::testing;

namespace {
std::vector<std::byte> bytes_of(std::string_view s) {
  std::vector<std::byte> v(s.size());
  std::memcpy(v.data(), s.data(), s.size());
  return v;
}
BuiltImage sample() {
  XdvdfsBuilder b;
  b.add("Default.xex", bytes_of("XEX2data"));
  b.add("Content/A/b.bin", bytes_of("hello world, this is b.bin"));
  return b.build();
}
}  // namespace

TEST_CASE("xdvdfs open, list and open_file", "[unit]") {
  auto img = sample();
  auto rd = img.reader();
  auto x = XdvdfsImage::open(*rd);
  REQUIRE(x.ok());
  CHECK(x.value().partition_offset() == 0xFD90000);

  auto l = x.value().list("/");
  REQUIRE(l.ok());
  REQUIRE(l.value().size() == 2);
  CHECK(l.value()[0].name == "Default.xex");
  CHECK_FALSE(l.value()[0].is_dir);
  CHECK(l.value()[0].size == 8);
  CHECK(l.value()[1].name == "Content");
  CHECK(l.value()[1].is_dir);
  CHECK(x.value().list("").ok());
  CHECK(x.value().list("content/a").value().size() == 1);

  auto f = x.value().open_file("content/A/b.bin");
  REQUIRE(f.ok());
  auto all = read_all(*f.value());
  REQUIRE(all.ok());
  CHECK(all.value() == bytes_of("hello world, this is b.bin"));
}

TEST_CASE("xdvdfs missing file", "[unit]") {
  auto img = sample();
  auto rd = img.reader();
  auto x = XdvdfsImage::open(*rd);
  REQUIRE(x.ok());
  auto f = x.value().open_file("missing");
  REQUIRE_FALSE(f.ok());
  CHECK(f.error().code == ErrorCode::NotFound);
  CHECK(x.value().open_file("Content").error().code == ErrorCode::NotFound);
  CHECK(x.value().list("nope").error().code == ErrorCode::NotFound);
}

TEST_CASE("xdvdfs truncated image", "[unit]") {
  auto img = sample();
  img.data.resize(33 * kSector + 10);
  auto rd = img.reader();
  auto x = XdvdfsImage::open(*rd);
  if (x.ok()) {
    auto l = x.value().list("/");
    REQUIRE_FALSE(l.ok());
    CHECK((l.error().code == ErrorCode::OutOfRange || l.error().code == ErrorCode::InvalidFormat));
  } else {
    CHECK((x.error().code == ErrorCode::OutOfRange || x.error().code == ErrorCode::InvalidFormat));
  }
}

TEST_CASE("xdvdfs random bytes", "[unit]") {
  std::vector<std::byte> junk(4 << 20, std::byte{0xAB});
  SpanReader rd(junk);
  auto x = XdvdfsImage::open(rd);
  REQUIRE_FALSE(x.ok());
  CHECK(x.error().code == ErrorCode::InvalidFormat);
  CHECK(x.error().hint.find("not an Xbox 360 disc image") != std::string::npos);
}

TEST_CASE("xdvdfs cyclic tree", "[unit]") {
  auto img = sample();
  // Second root entry starts after "Default.xex" (14+11=25 -> 28 bytes): offset 7 units.
  size_t second = static_cast<size_t>(img.root_sector) * kSector + 28;
  img.data[second + 2] = std::byte{7};
  img.data[second + 3] = std::byte{0};
  auto rd = img.reader();
  auto x = XdvdfsImage::open(*rd);
  REQUIRE(x.ok());
  auto l = x.value().list("/");
  REQUIRE_FALSE(l.ok());
  CHECK(l.error().code == ErrorCode::InvalidFormat);
}

TEST_CASE("readers", "[unit]") {
  std::vector<std::byte> d = bytes_of("0123456789");
  SpanReader sr(d);
  CHECK(sr.size() == 10);
  std::byte buf[4];
  CHECK(sr.read(6, buf).ok());
  CHECK(buf[0] == std::byte{'6'});
  CHECK(sr.read(7, buf).error().code == ErrorCode::OutOfRange);
  CHECK(sr.read(~uint64_t{0}, buf).error().code == ErrorCode::OutOfRange);

  SubReader sub(sr, 2, 5);
  CHECK(sub.size() == 5);
  CHECK(sub.read(1, buf).ok());
  CHECK(buf[0] == std::byte{'3'});
  CHECK(sub.read(2, buf).error().code == ErrorCode::OutOfRange);

  lr::testing::TempDir tmp{"lr_test_filereader"};
  const auto path = tmp.path / "file.bin";
  { std::ofstream(path, std::ios::binary) << "0123456789"; }
  auto fr = FileReader::open(path);
  REQUIRE(fr.ok());
  CHECK(fr.value()->size() == 10);
  CHECK(fr.value()->read(3, buf).ok());
  CHECK(buf[0] == std::byte{'3'});
  CHECK(fr.value()->read(8, buf).error().code == ErrorCode::OutOfRange);
  auto all = read_all(*fr.value());
  REQUIRE(all.ok());
  CHECK(all.value() == d);
  std::filesystem::remove(path);
  CHECK(FileReader::open(path).error().code == ErrorCode::NotFound);
}

namespace {
void poke32(BuiltImage& img, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) img.data[at + i] = std::byte((v >> (8 * i)) & 0xFF);
}
size_t root_at(const BuiltImage& img) { return static_cast<size_t>(img.root_sector) * kSector; }
void check_list_error(BuiltImage& img, ErrorCode code) {
  auto rd = img.reader();
  auto x = XdvdfsImage::open(*rd);
  REQUIRE(x.ok());
  auto l = x.value().list("/");
  REQUIRE_FALSE(l.ok());
  CHECK(l.error().code == code);
}
}  // namespace

TEST_CASE("xdvdfs child offset beyond directory size", "[unit]") {
  auto img = sample();
  size_t second = root_at(img) + 28;
  img.data[second + 2] = std::byte{0xFF};  // right = 0x7FFF units -> far past 2048 bytes
  img.data[second + 3] = std::byte{0x7F};
  check_list_error(img, ErrorCode::InvalidFormat);
}

TEST_CASE("xdvdfs name_len overruns directory", "[unit]") {
  auto img = sample();
  poke32(img, 32 * kSector + 0x18, 48);  // second entry header fits (42) but name ends at 49
  check_list_error(img, ErrorCode::InvalidFormat);
}

TEST_CASE("xdvdfs directory size above cap", "[unit]") {
  auto img = sample();
  poke32(img, 32 * kSector + 0x18, 65u << 20);
  check_list_error(img, ErrorCode::InvalidFormat);
}

TEST_CASE("xdvdfs directory extent past image end", "[unit]") {
  auto img = sample();
  poke32(img, 32 * kSector + 0x18, 64 * 1024);  // under the cap, but beyond the image
  check_list_error(img, ErrorCode::OutOfRange);
}

TEST_CASE("xdvdfs open_file extending past image end", "[unit]") {
  auto img = sample();
  poke32(img, root_at(img) + 8, 0x10000000);  // Default.xex size
  auto rd = img.reader();
  auto x = XdvdfsImage::open(*rd);
  REQUIRE(x.ok());
  auto f = x.value().open_file("Default.xex");
  REQUIRE_FALSE(f.ok());
  CHECK(f.error().code == ErrorCode::OutOfRange);
}

TEST_CASE("xdvdfs tree with left pointers", "[unit]") {
  XdvdfsBuilder b;
  b.add("m", bytes_of("M")).add("a", bytes_of("A")).add("z", bytes_of("Z"));
  auto img = b.build();
  // Entries are 16 bytes each at offsets 0, 16, 32 (units 0, 4, 8). Make "m" the root with
  // left="a" and right="z"; "a" must not chain to "z".
  size_t r = root_at(img);
  img.data[r + 0] = std::byte{4};
  img.data[r + 2] = std::byte{8};
  img.data[r + 16 + 2] = std::byte{0};
  auto rd = img.reader();
  auto x = XdvdfsImage::open(*rd);
  REQUIRE(x.ok());
  auto l = x.value().list("/");
  REQUIRE(l.ok());
  REQUIRE(l.value().size() == 3);
  CHECK(l.value()[0].name == "a");
  CHECK(l.value()[1].name == "m");
  CHECK(l.value()[2].name == "z");
  auto f = x.value().open_file("A");
  REQUIRE(f.ok());
  CHECK(read_all(*f.value()).value() == bytes_of("A"));
}
