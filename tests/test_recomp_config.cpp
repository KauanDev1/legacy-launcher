// SPDX-License-Identifier: MIT
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

TEST_CASE("render_recomp_config appends analysed function boundaries", "[unit]") {
  const std::vector<FunctionRange> extra = {
      {0x824E7F00, 0x40, false},  // overlaps the hand-written 0x824E7EF0+0x98 entry: skipped
      {0x82500000, 0x120, false},
  };
  auto r = render_recomp_config(kSample, "/i.xex", "/s.toml", "/o", extra);
  REQUIRE(r);
  auto t = parse(r.value());
  const auto* fns = t["main"]["functions"].as_array();
  REQUIRE(fns);
  REQUIRE(fns->size() == 2);
  CHECK((*fns)[0].as_table()->at("address").value<int64_t>() == 0x824E7EF0);
  CHECK((*fns)[1].as_table()->at("address").value<int64_t>() == 0x82500000);
  CHECK((*fns)[1].as_table()->at("size").value<int64_t>() == 0x120);
  CHECK(r.value().find("address = 0x82500000") != std::string::npos);

  auto none = render_recomp_config("[main]\n", "/i.xex", "/s.toml", "/o", extra);
  REQUIRE(none);
  CHECK(parse(none.value())["main"]["functions"].as_array()->size() == 2);
}

TEST_CASE("parse_function_overrides reads the analyse stage output", "[unit]") {
  auto ok = parse_function_overrides("functions = [\n  { address = 0x82000010, size = 0x20 },\n]\n");
  REQUIRE(ok);
  REQUIRE(ok.value().size() == 1);
  CHECK(ok.value()[0].start == 0x82000010u);
  CHECK(ok.value()[0].size == 0x20u);
  CHECK(parse_function_overrides("").value().empty());
  CHECK_FALSE(parse_function_overrides("functions = [ { address = 1 } ]"));
  CHECK_FALSE(parse_function_overrides("functions = [ 3"));
}

TEST_CASE("classify_recomp_line flags every ERROR line", "[unit]") {
  CHECK(classify_recomp_line("Recompiling functions... 42%") == RecompLine::Progress);
  CHECK(classify_recomp_line("ERROR: __restgprlr_14 address is unspecified") == RecompLine::Error);
  CHECK(classify_recomp_line("ERROR: Switch case at 8235C19C is trying to jump outside function: 8235C218") ==
        RecompLine::Error);
  CHECK(classify_recomp_line("ERROR: Unable to apply the patch file, patch failed") == RecompLine::Error);
  CHECK(classify_recomp_line("Unrecognized instruction at 0x82200000: bdzf") == RecompLine::Other);
  CHECK(classify_recomp_line("  ERROR: indented is not XenonRecomp's format") == RecompLine::Other);
  CHECK(classify_recomp_line("") == RecompLine::Other);
}

TEST_CASE("recomp_errors reports the count and quotes the first lines", "[unit]") {
  const std::vector<std::string> first = {"ERROR: a", "ERROR: b"};
  auto e = recomp_errors(7, first, "/data/work/logs/recompile.log");
  CHECK(e.code == ErrorCode::ToolFailed);
  CHECK(e.message == "XenonRecomp reported 7 errors: ERROR: a");
  CHECK(e.hint.find("ERROR: b") != std::string::npos);
  CHECK(e.hint.find("5 more") != std::string::npos);
  CHECK(e.hint.find("/data/work/logs/recompile.log") != std::string::npos);
  CHECK(recomp_errors(1, std::span<const std::string>(first.data(), 1), "/l").message ==
        "XenonRecomp reported 1 error: ERROR: a");
}

TEST_CASE("count_recomp_warnings counts mnemonics and float16 packs", "[unit]") {
  const std::string log =
      "Recompiling functions... 10%\n"
      "Unrecognized instruction at 0x82200000: bdzf\n"
      "Unrecognized instruction at 0x82200010: stfsu\r\n"
      "Unrecognized instruction at 0x82200020: bdzf\n"
      "Unexpected float16_4 pack instruction at 82300000\n"
      "Unexpected float16_4 pack instruction at 82300008\n"
      "ERROR: not a warning\n"
      "Unrecognized instruction at 0x82200030: bdzf";  // no trailing newline
  const auto w = count_recomp_warnings(log);
  CHECK(w.unrecognized.size() == 2);
  CHECK(w.unrecognized.at("bdzf") == 3);
  CHECK(w.unrecognized.at("stfsu") == 1);
  CHECK(w.float16_pack == 2);

  const auto none = count_recomp_warnings("Recompiling functions... 100%\n");
  CHECK(none.unrecognized.empty());
  CHECK(none.float16_pack == 0);
}

TEST_CASE("format_recomp_warnings summarises the top mnemonics", "[unit]") {
  CHECK(format_recomp_warnings({}) == "unrecognized instructions: 0");

  RecompWarnings w;
  w.unrecognized = {{"stfsu", 507}, {"bdzf", 754}, {"aaa", 507}};
  CHECK(format_recomp_warnings(w) == "unrecognized instructions: 1768 (top: bdzf 754, aaa 507, stfsu 507)");
  w.float16_pack = 4;
  CHECK(format_recomp_warnings(w) ==
        "unrecognized instructions: 1768 (top: bdzf 754, aaa 507, stfsu 507), float16_4 packs: 4");

  RecompWarnings many;
  for (char c = 'a'; c < 'a' + 12; ++c) many.unrecognized[std::string(1, c)] = 1;
  const auto s = format_recomp_warnings(many);
  CHECK(s.find("j 1") != std::string::npos);
  CHECK(s.find("k 1") == std::string::npos);

  RecompWarnings only16;
  only16.float16_pack = 2;
  CHECK(format_recomp_warnings(only16) == "unrecognized instructions: 0, float16_4 packs: 2");
}
