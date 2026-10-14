// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

#include "common/fs_util.h"
#include "common/sha256.h"

namespace fs = std::filesystem;

static std::string hash_of(const std::string& s, size_t chunk) {
  lr::Sha256 h;
  const auto* p = reinterpret_cast<const std::byte*>(s.data());
  for (size_t i = 0; i < s.size(); i += chunk)
    h.update({p + i, std::min(chunk, s.size() - i)});
  auto d = h.finish();
  return lr::to_hex(d);
}

TEST_CASE("sha256 known vectors", "[unit]") {
  CHECK(hash_of("", 1) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(hash_of("abc", 1) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(hash_of(std::string(1000000, 'a'), 7) ==
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("sha256_file", "[unit]") {
  auto dir = fs::temp_directory_path() / "lr_test_sha_file";
  fs::remove_all(dir);
  fs::create_directories(dir);
  { std::ofstream(dir / "a") << "abc"; }
  auto r = lr::sha256_file(dir / "a");
  REQUIRE(r.ok());
  CHECK(r.value() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  auto bad = lr::sha256_file(dir / "missing");
  REQUIRE(!bad.ok());
  CHECK(bad.error().code == lr::ErrorCode::IoError);
  fs::remove_all(dir);
}

TEST_CASE("atomic_write round-trips and leaves no tmp", "[unit]") {
  auto dir = fs::temp_directory_path() / "lr_test_atomic";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const std::byte data[] = {std::byte{'h'}, std::byte{'i'}};
  REQUIRE(lr::atomic_write(dir / "f.bin", data).ok());
  CHECK(fs::exists(dir / "f.bin"));
  CHECK(!fs::exists(dir / "f.bin.tmp"));
  CHECK(fs::file_size(dir / "f.bin") == 2);
  fs::remove_all(dir);
}

TEST_CASE("atomic_copy_from", "[unit]") {
  auto dir = fs::temp_directory_path() / "lr_test_atomic_copy";
  fs::remove_all(dir);
  auto dst = dir / "sub" / "out.txt";
  auto fail = lr::atomic_copy_from(dst, [](std::ostream& os) -> lr::Result<void> {
    os << "partial";
    return lr::Error{lr::ErrorCode::Internal, "boom", ""};
  });
  CHECK(!fail.ok());
  CHECK(!fs::exists(dst));
  CHECK(!fs::exists(dst.string() + ".tmp"));
  auto ok = lr::atomic_copy_from(dst, [](std::ostream& os) -> lr::Result<void> {
    os << "hello";
    return {};
  });
  REQUIRE(ok.ok());
  CHECK(fs::file_size(dst) == 5);
  CHECK(!fs::exists(dst.string() + ".tmp"));
  fs::remove_all(dir);
}
