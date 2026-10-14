// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>

#include "formats/reader.h"
#include "formats/xdvdfs.h"
#include "support/xdvdfs_builder.h"

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

  auto path = std::filesystem::temp_directory_path() / "lr_test_filereader.bin";
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
