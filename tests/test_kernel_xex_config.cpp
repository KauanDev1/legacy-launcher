// SPDX-License-Identifier: MIT
// XEX queries, console configuration and crypto imports (exports/xex.cpp, config.cpp, crypto.cpp).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "common/sha1.h"
#include "common/sha256.h"
#include "formats/xex2.h"
#include "runtime/be.h"
#include "runtime/kernel/status.h"
#include "runtime/kernel/variables.h"
#include "runtime/loader.h"
#include "support/kernel_fixture.h"
#include "support/xex2_builder.h"

using namespace lr;
using namespace lr::kernel;
using lr::runtime::be;
using lr::testing::KernelFixture;
using lr::testing::Xex2Builder;

namespace {

constexpr uint32_t kDbgPrint = 0x003;
constexpr uint32_t kSlotVar = 0x82002000, kSlotThunk = 0x82001010;

runtime::LoadedImage load(KernelFixture& f) {
  std::vector<uint8_t> text(0x100, 0), data(0x100, 0);
  lr::testing::put_be32(text, kSlotThunk - 0x82001000, 0x01000000u | kDbgPrint);
  lr::testing::put_be32(data, kSlotVar - 0x82002000, kDbgPrint);
  const auto bytes = Xex2Builder()
                         .entry_point(0x82001000)
                         .image_base(0x82000000)
                         .execution(0, 0x11223344, 0x55667788, 0x584111F7, 1, 2, 3, 4)
                         .raw_pair(0x00030000, 0x40)  // system flags: bit 6
                         .import_library("xboxkrnl.exe", {kSlotVar, kSlotThunk})
                         .pe_image(0x82000000, 0x3000, {{".text", 0x1000, text, true}, {".data", 0x2000, data, false}})
                         .build();
  auto img = runtime::load_image(f.mem, bytes);
  REQUIRE(img.ok());
  auto xex = formats::parse_xex2(bytes);
  REQUIRE(xex.ok());
  REQUIRE(resolve_variable_imports(f.mem, img.value(), xex.value()).ok());
  return img.value();
}

uint32_t put_str(KernelFixture& f, const std::string& s) {
  const uint32_t a = f.alloc(static_cast<uint32_t>(s.size()) + 1);
  std::memcpy(f.mem.translate<char>(a), s.c_str(), s.size() + 1);
  return a;
}

uint32_t put_bytes(KernelFixture& f, const std::string& s) {
  const uint32_t a = f.alloc(static_cast<uint32_t>(s.size()) + 1);
  std::memcpy(f.mem.translate<char>(a), s.data(), s.size());
  return a;
}

uint32_t u32(KernelFixture& f, uint32_t a) { return *f.mem.translate<be<uint32_t>>(a); }

std::string hex_at(KernelFixture& f, uint32_t a, size_t n) {
  return to_hex({f.mem.translate<uint8_t>(a), n});
}

}  // namespace

TEST_CASE("RtlImageXexHeaderField returns entry point value", "[unit][kernel][xex]") {
  KernelFixture f;
  const auto img = load(f);
  CHECK(static_cast<uint32_t>(f.call("RtlImageXexHeaderField", img.header_guest, 0x00010100u)) == 0x82001000u);
  CHECK(static_cast<uint32_t>(f.call("RtlImageXexHeaderField", img.header_guest, 0x00010201u)) == 0x82000000u);
  CHECK(f.call("RtlImageXexHeaderField", img.header_guest, 0x00FFFF01u) == 0u);  // absent
  // Execution info: a pointer into the guest header copy, title id at +0x0C.
  const uint32_t p = static_cast<uint32_t>(f.call("RtlImageXexHeaderField", img.header_guest, 0x00040006u));
  REQUIRE(p > img.header_guest);
  CHECK(u32(f, p + 0x0C) == 0x584111F7u);
}

TEST_CASE("XexGetModuleHandle(NULL) is the LDR entry", "[unit][kernel][xex]") {
  KernelFixture f;
  const auto img = load(f);
  const uint32_t out = f.alloc(4);
  CHECK(f.call("XexGetModuleHandle", 0u, out) == 0u);
  CHECK(u32(f, out) == img.ldr_entry);
  CHECK(f.call("XexGetModuleHandle", put_str(f, "default.xex"), out) == 0u);
  CHECK(u32(f, out) == img.ldr_entry);
  CHECK(f.call("XexGetModuleHandle", put_str(f, "xboxkrnl.exe"), out) == 0u);
  const uint32_t krnl = u32(f, out);
  CHECK(krnl != 0);
  CHECK(krnl != img.ldr_entry);
  CHECK(f.call("XexGetModuleHandle", put_str(f, "xboxkrnl.exe"), out) == 0u);
  CHECK(u32(f, out) == krnl);
  CHECK(f.call("XexGetModuleHandle", put_str(f, "xam.xex"), out) == 0u);
  CHECK(u32(f, out) != krnl);
  CHECK(f.call("XexGetModuleHandle", put_str(f, "nope.dll"), out) == ERROR_NOT_FOUND);
  CHECK(u32(f, out) == 0u);
}

TEST_CASE("XexCheckExecutablePrivilege reads the system flags", "[unit][kernel][xex]") {
  KernelFixture f;
  load(f);
  CHECK(f.call("XexCheckExecutablePrivilege", 6u) == 1u);
  CHECK(f.call("XexCheckExecutablePrivilege", 5u) == 0u);
}

TEST_CASE("XexGetProcedureAddress maps an ordinal to its thunk", "[unit][kernel][xex]") {
  KernelFixture f;
  load(f);
  const uint32_t out = f.alloc(4), mod = f.alloc(4);
  f.call("XexGetModuleHandle", put_str(f, "xboxkrnl.exe"), mod);
  CHECK(f.call("XexGetProcedureAddress", u32(f, mod), kDbgPrint, out) == 0u);
  CHECK(u32(f, out) == kSlotThunk);
  CHECK(static_cast<uint32_t>(f.call("XexGetProcedureAddress", u32(f, mod), 0x7FFu, out)) == 0xC0000263u);
  CHECK(u32(f, out) == 0u);
  CHECK(static_cast<uint32_t>(f.call("XexGetProcedureAddress", 0xDEADu, kDbgPrint, out)) == STATUS_INVALID_HANDLE);
}

TEST_CASE("XGetLanguage maps pt_BR.UTF-8 to 9", "[unit][kernel][config]") {
  struct Case {
    const char* locale;
    uint32_t lang;
  } cases[] = {{"pt_BR.UTF-8", 9}, {"en_US.UTF-8", 1}, {"ja_JP", 2}, {"de_DE", 3},  {"fr_FR", 4}, {"es_ES", 5},
               {"it_IT", 6},       {"ko_KR", 7},       {"zh_TW", 8}, {"zh_HK", 8},  {"zh_CN", 10}, {"zh", 10},
               {"pl_PL", 11},      {"ru_RU", 12},      {"C", 1},     {"", 1},       {"xx_YY", 1}};
  for (const auto& c : cases) {
    INFO(c.locale);
    KernelConfig cfg;
    cfg.language = c.locale;
    KernelFixture f(cfg);
    CHECK(f.call("XGetLanguage") == c.lang);
  }
}

TEST_CASE("ExGetXConfigSetting returns the Xenia defaults", "[unit][kernel][config]") {
  KernelConfig cfg;
  cfg.language = "pt_BR";
  KernelFixture f(cfg);
  const uint32_t buf = f.alloc(16), req = f.alloc(4);
  CHECK(f.call("ExGetXConfigSetting", 3u, 9u, buf, 4u, req) == 0u);  // USER_LANGUAGE
  CHECK(u32(f, buf) == 9u);
  CHECK(u32(f, req) == 4u);
  CHECK(f.call("ExGetXConfigSetting", 2u, 2u, buf, 4u, req) == 0u);  // SECURED_AV_REGION
  CHECK(u32(f, buf) == 0x1000u);
  CHECK(f.call("ExGetXConfigSetting", 3u, 0xAu, buf, 4u, req) == 0u);  // USER_VIDEO_FLAGS
  CHECK(u32(f, buf) == 0x00040000u);
  CHECK(f.call("ExGetXConfigSetting", 3u, 0xEu, buf, 1u, req) == 0u);  // USER_COUNTRY
  CHECK(*f.mem.translate<uint8_t>(buf) == 103u);
  CHECK(u32(f, req) == 1u);
  CHECK(static_cast<uint32_t>(f.call("ExGetXConfigSetting", 3u, 9u, buf, 2u, req)) == STATUS_BUFFER_TOO_SMALL);
  CHECK(static_cast<uint32_t>(f.call("ExGetXConfigSetting", 3u, 0x77u, buf, 4u, req)) == 0xC00000F0u);
  CHECK(static_cast<uint32_t>(f.call("ExGetXConfigSetting", 9u, 1u, buf, 4u, req)) == 0xC00000EFu);
}

TEST_CASE("fixed console identity", "[unit][kernel][config]") {
  KernelFixture f;
  load(f);
  CHECK(f.call("XamGetCurrentTitleId") == 0x584111F7u);
  CHECK(f.call("XGetGameRegion") == 0xFFFFu);
  const uint32_t out = f.alloc(4);
  CHECK(f.call("XamGetExecutionId", out) == 0u);
  CHECK(u32(f, u32(f, out) + 0x0C) == 0x584111F7u);
  CHECK(f.call("XamLoaderGetLaunchDataSize", out) == ERROR_NOT_FOUND);
  CHECK(u32(f, out) == 0u);
  CHECK(f.call("XamLoaderGetLaunchData", out, 4u) == ERROR_NOT_FOUND);
}

TEST_CASE("XeCryptSha over two buffers equals sha1 of concatenation", "[unit][kernel][crypto]") {
  KernelFixture f;
  const uint32_t a = put_bytes(f, "hello "), b = put_bytes(f, "world"), out = f.alloc(32);
  f.call("XeCryptSha", a, 6u, b, 5u, 0u, 0u, out, 20u);
  Sha1 h;
  const std::string all = "hello world";
  h.update({reinterpret_cast<const std::byte*>(all.data()), all.size()});
  const auto d = h.finish();
  CHECK(hex_at(f, out, 20) == to_hex(d));
}

TEST_CASE("XeCryptSha incremental with a cloned guest state", "[unit][kernel][crypto]") {
  KernelFixture f;
  const uint32_t st = f.alloc(0x58), copy = f.alloc(0x58), out = f.alloc(32);
  const std::string part1(100, 'x'), part2(37, 'y');
  f.call("XeCryptShaInit", st);
  f.call("XeCryptShaUpdate", st, put_bytes(f, part1), 100u);
  std::memcpy(f.mem.translate<char>(copy), f.mem.translate<char>(st), 0x58);  // guest clones the state
  f.call("XeCryptShaUpdate", st, put_bytes(f, part2), 37u);
  f.call("XeCryptShaFinal", st, out, 20u);
  Sha1 h;
  const std::string all = part1 + part2;
  h.update({reinterpret_cast<const std::byte*>(all.data()), all.size()});
  CHECK(hex_at(f, out, 20) == to_hex(h.finish()));
  // the clone still holds only part1
  f.call("XeCryptShaFinal", copy, out, 20u);
  Sha1 h1;
  h1.update({reinterpret_cast<const std::byte*>(part1.data()), part1.size()});
  CHECK(hex_at(f, out, 20) == to_hex(h1.finish()));
}

TEST_CASE("XeCryptSha256 incremental matches Sha256", "[unit][kernel][crypto]") {
  KernelFixture f;
  const uint32_t st = f.alloc(0x64), out = f.alloc(32);
  const std::string p1(70, 'a'), p2(3, 'b');
  f.call("XeCryptSha256Init", st);
  f.call("XeCryptSha256Update", st, put_bytes(f, p1), 70u);
  f.call("XeCryptSha256Update", st, put_bytes(f, p2), 3u);
  f.call("XeCryptSha256Final", st, out, 32u);
  Sha256 h;
  const std::string all = p1 + p2;
  h.update({reinterpret_cast<const std::byte*>(all.data()), all.size()});
  CHECK(hex_at(f, out, 32) == to_hex(h.finish()));
}

TEST_CASE("XeCryptRandom fills the buffer", "[unit][kernel][crypto]") {
  KernelFixture f;
  const uint32_t buf = f.alloc(64);
  f.call("XeCryptRandom", buf, 64u);
  bool nonzero = false;
  for (int i = 0; i < 64; ++i) nonzero |= *f.mem.translate<uint8_t>(buf + static_cast<uint32_t>(i)) != 0;
  CHECK(nonzero);
}
