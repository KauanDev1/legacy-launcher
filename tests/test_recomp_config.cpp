// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <sstream>
#include <string>

#include <toml++/toml.hpp>

#include "pipeline/stages/patch_stage.h"
#include "pipeline/stages/recompile_stage.h"

using namespace lr;
using namespace lr::pipeline;
namespace fs = std::filesystem;

namespace {

constexpr const char* kSample = R"(# sample
[main]
file_path = "../private/default.xex"
patch_file_path = "../private/default.xexp"
patched_file_path = "../private/default_patched.xex"
out_directory_path = "../ppc"
switch_table_file_path = "switch.toml"
skip_lr = false
restgprlr_14_address = 0x831B0B40
savegprlr_14_address = 0x831B0AF0
functions = [
    { address = 0x824E7EF0, size = 0x98 },
]

[[midasm_hook]]
name = "Hook"
address = 0x82E26244
registers = ["r3"]
)";

toml::table parse(const std::string& s) {
#if TOML_EXCEPTIONS
  return toml::parse(s);
#else
  return std::move(toml::parse(s)).table();
#endif
}

}  // namespace

TEST_CASE("render_recomp_config rewrites paths and drops patch keys", "[unit]") {
  const fs::path image = "/data/work/image/default_tu1.xex";
  const fs::path switches = "/data/work/analysis/switch_tables.toml";
  const fs::path out = "/data/work/generated";
  auto r = render_recomp_config(kSample, image, switches, out);
  REQUIRE(r);
  const std::string& text = r.value();

  auto t = parse(text);
  const auto* main = t["main"].as_table();
  REQUIRE(main);
  CHECK((*main)["file_path"].value<std::string>() == image.string());
  CHECK((*main)["switch_table_file_path"].value<std::string>() == switches.string());
  CHECK((*main)["out_directory_path"].value<std::string>() == out.string());
  CHECK_FALSE(main->contains("patch_file_path"));
  CHECK_FALSE(main->contains("patched_file_path"));

  // Everything else is kept, hex addresses verbatim.
  CHECK(text.find("restgprlr_14_address = 0x831B0B40") != std::string::npos);
  CHECK((*main)["savegprlr_14_address"].value<int64_t>() == 0x831B0AF0);
  CHECK((*main)["skip_lr"].value<bool>() == false);
  REQUIRE((*main)["functions"].as_array());
  CHECK((*main)["functions"].as_array()->size() == 1);
  REQUIRE(t["midasm_hook"].as_array());
  CHECK(t["midasm_hook"][0]["name"].value<std::string>() == "Hook");
}

TEST_CASE("render_recomp_config rejects bad input", "[unit]") {
  auto bad = render_recomp_config("[main\nfile_path = 1", "/a", "/b", "/c");
  REQUIRE_FALSE(bad);
  CHECK(bad.error().code == ErrorCode::InvalidFormat);

  auto no_main = render_recomp_config("x = 1\n", "/a", "/b", "/c");
  REQUIRE_FALSE(no_main);
  CHECK(no_main.error().code == ErrorCode::InvalidFormat);

  auto rel = render_recomp_config(kSample, "relative.xex", "/b", "/c");
  REQUIRE_FALSE(rel);
  CHECK(rel.error().code == ErrorCode::Internal);
}

TEST_CASE("parse_recomp_progress reads XenonRecomp percentages", "[unit]") {
  auto a = parse_recomp_progress("Recompiling functions... 50%");
  REQUIRE(a);
  CHECK(*a == 0.5f);
  auto b = parse_recomp_progress("Recompiling functions... 0.0123%");
  REQUIRE(b);
  CHECK(*b < 0.001f);
  auto c = parse_recomp_progress("Recompiling functions... 100%");
  REQUIRE(c);
  CHECK(*c == 1.0f);
  CHECK_FALSE(parse_recomp_progress("ERROR: something"));
  CHECK_FALSE(parse_recomp_progress("Recompiling functions... abc%"));
  CHECK_FALSE(parse_recomp_progress("Recompiling functions... 12"));
}

TEST_CASE("xex_patcher_result_name names every XexPatcher result", "[unit]") {
  CHECK(xex_patcher_result_name(0) == "Success");
  CHECK(xex_patcher_result_name(6) == "PatchIncompatible");
  CHECK(xex_patcher_result_name(8) == "PatchUnsupported");
  CHECK(xex_patcher_result_name(99) == "Unknown");
}
