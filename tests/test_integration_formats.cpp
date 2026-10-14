// SPDX-License-Identifier: MIT
#include <toml++/toml.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/sha256.h"
#include "formats/reader.h"
#include "formats/stfs.h"
#include "formats/xdvdfs.h"

using namespace lr::formats;

namespace {
std::string sha_hex(const std::vector<std::byte>& b) {
  lr::Sha256 h;
  h.update(b);
  return lr::to_hex(h.finish());
}
}  // namespace

TEST_CASE("real ISO: package and title update match game.toml", "[integration]") {
  const char* iso = std::getenv("LEGACYRECOMP_ISO");
  if (!iso) SKIP("LEGACYRECOMP_ISO not set");

  auto cfg = toml::parse_file(std::string(LR_SOURCE_DIR) + "/config/minecraft-tu1/game.toml");
  const std::string pkg_path = cfg["sources"]["package"].value_or("");
  const std::string tu_path = cfg["sources"]["title_update"].value_or("");

  auto reader = FileReader::open(iso);
  REQUIRE(reader);
  auto img = XdvdfsImage::open(*reader.value());
  REQUIRE(img);

  struct Case { const char* src_key; const char* hash_key; const char* name_key; std::string path; };
  const Case cases[] = {{"package", "package_xex", "package_xex", pkg_path},
                        {"title_update", "tu_xexp", "tu_xexp", tu_path}};
  for (const auto& c : cases) {
    INFO(c.path);
    auto file = img.value().open_file(c.path);
    REQUIRE(file);
    auto pkg = StfsPackage::open(*file.value());
    REQUIRE(pkg);
    CHECK(pkg.value().header().title_id == cfg["title_id"].value_or<uint32_t>(0));

    const std::string name = cfg["sources"][c.name_key].value_or("");
    const StfsEntry* entry = nullptr;
    for (const auto& e : pkg.value().entries()) {
      if (e.path == name) entry = &e;
    }
    REQUIRE(entry != nullptr);
    auto inner = pkg.value().open_file(name);
    REQUIRE(inner);
    auto bytes = read_all(*inner.value());
    REQUIRE(bytes);
    CHECK(bytes.value().size() == entry->size);
    REQUIRE(bytes.value().size() >= 4);
    CHECK(std::memcmp(bytes.value().data(), "XEX2", 4) == 0);
    CHECK(sha_hex(bytes.value()) == cfg["hashes"][c.hash_key].value_or(""));
  }
}
