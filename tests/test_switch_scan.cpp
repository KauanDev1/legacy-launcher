// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <utility>
#include <vector>

#include "pipeline/switch_scan.h"

using namespace lr::pipeline;

namespace {

constexpr uint32_t kBase = 0x82000000;

// Synthetic PowerPC encoders (big-endian words written into a fake image).
uint32_t cmplwi(uint32_t crf, uint32_t r, uint32_t imm) { return (10u << 26) | (crf << 23) | (r << 16) | imm; }
uint32_t bgt(uint32_t crf, int32_t disp) { return (16u << 26) | (12u << 21) | ((crf * 4 + 1) << 16) | (static_cast<uint32_t>(disp) & 0xFFFC); }
uint32_t bgtlr(uint32_t crf) { return (19u << 26) | (12u << 21) | ((crf * 4 + 1) << 16) | (16u << 1); }
uint32_t lis(uint32_t r, uint32_t imm) { return (15u << 26) | (r << 21) | imm; }
uint32_t addi(uint32_t d, uint32_t a, int32_t imm) { return (14u << 26) | (d << 21) | (a << 16) | (static_cast<uint32_t>(imm) & 0xFFFF); }
uint32_t slwi(uint32_t a, uint32_t s, uint32_t n) { return (21u << 26) | (s << 21) | (a << 16) | (n << 11) | (0u << 6) | ((31 - n) << 1); }
uint32_t x31(uint32_t d, uint32_t a, uint32_t b, uint32_t xo) { return (31u << 26) | (d << 21) | (a << 16) | (b << 11) | (xo << 1); }
uint32_t lwzx(uint32_t d, uint32_t a, uint32_t b) { return x31(d, a, b, 23); }
uint32_t lbzx(uint32_t d, uint32_t a, uint32_t b) { return x31(d, a, b, 87); }
uint32_t add(uint32_t d, uint32_t a, uint32_t b) { return x31(d, a, b, 266); }
uint32_t mtctr(uint32_t s) { return (31u << 26) | (s << 21) | (9u << 16) | (467u << 1); }
uint32_t lwz(uint32_t d, uint32_t a, int32_t imm) { return (32u << 26) | (d << 21) | (a << 16) | (static_cast<uint32_t>(imm) & 0xFFFF); }
uint32_t mulli(uint32_t d, uint32_t a, int32_t imm) { return (7u << 26) | (d << 21) | (a << 16) | (static_cast<uint32_t>(imm) & 0xFFFF); }
constexpr uint32_t kNop = 0x60000000, kBctr = 0x4E800420, kBlr = 0x4E800020;

struct FakeImage {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(0x1000, 0);
  void word(uint32_t addr, uint32_t v) {
    size_t o = addr - kBase;
    bytes[o] = static_cast<uint8_t>(v >> 24);
    bytes[o + 1] = static_cast<uint8_t>(v >> 16);
    bytes[o + 2] = static_cast<uint8_t>(v >> 8);
    bytes[o + 3] = static_cast<uint8_t>(v);
  }
  void code(uint32_t addr, std::initializer_list<uint32_t> ws) {
    for (uint32_t w : ws) word(addr, w), addr += 4;
  }
};

const std::vector<std::pair<uint32_t, uint32_t>> kCode = {{kBase, 0x400}};

}  // namespace

TEST_CASE("scan_switch_tables: absolute table, scheduled order", "[unit]") {
  FakeImage img;
  // cmplwi; bgt default; lis; slwi; addi; lwzx; mtctr; bctr  (XenonAnalyse expects lis,addi,rlwinm)
  img.code(kBase, {cmplwi(6, 11, 2), bgt(6, 0x40), lis(12, 0x8200), slwi(0, 11, 2), addi(12, 12, 0x800),
                   lwzx(0, 12, 0), mtctr(0), kBctr});
  img.word(kBase + 0x800, kBase + 0x20);
  img.word(kBase + 0x804, kBase + 0x28);
  img.word(kBase + 0x808, kBase + 0x24);

  auto scan = scan_switch_tables(img.bytes, kBase, kCode);
  REQUIRE(scan.tables.size() == 1);
  CHECK(scan.unresolved.empty());
  const auto& t = scan.tables[0];
  CHECK(t.base == kBase + 0x1C);
  CHECK(t.r == 11);
  CHECK(t.default_label == kBase + 0x44);
  CHECK(t.labels == std::vector<uint32_t>{kBase + 0x20, kBase + 0x28, kBase + 0x24});
}

TEST_CASE("scan_switch_tables: byte offsets with nop padding and bgtlr", "[unit]") {
  FakeImage img;
  const uint32_t at = kBase + 0x100;
  img.code(at, {cmplwi(7, 9, 2), bgtlr(7), lis(12, 0x8200), addi(12, 12, 0x900), lbzx(0, 12, 9), slwi(0, 0, 2),
                lis(12, 0x8200), kNop, addi(12, 12, 0x300), kNop, add(12, 12, 0), mtctr(12), kBctr});
  img.bytes[0x900] = 0;
  img.bytes[0x901] = 1;
  img.bytes[0x902] = 3;

  auto scan = scan_switch_tables(img.bytes, kBase, kCode);
  REQUIRE(scan.tables.size() == 1);
  const auto& t = scan.tables[0];
  CHECK(t.r == 9);
  CHECK(t.default_label == 0);
  CHECK(t.labels == std::vector<uint32_t>{kBase + 0x300, kBase + 0x304, kBase + 0x30C});
}

TEST_CASE("scan_switch_tables: virtual calls are ignored, unsupported guards are reported", "[unit]") {
  FakeImage img;
  // Virtual call: no bound check, no indexed load -> neither table nor unresolved.
  img.code(kBase, {kBlr, lwz(11, 3, 0), lwz(0, 11, 4), mtctr(0), kBctr});
  // Bound check followed by an instruction the evaluator does not model.
  img.code(kBase + 0x40, {cmplwi(6, 11, 4), bgt(6, 0x40), mulli(0, 11, 4), lis(12, 0x8200), addi(12, 12, 0x800),
                          lwzx(0, 12, 0), mtctr(0), kBctr});
  // Table entry outside the code range -> unresolved, not a bogus table.
  img.code(kBase + 0x80, {cmplwi(6, 11, 0), bgt(6, 0x40), lis(12, 0x8200), slwi(0, 11, 2), addi(12, 12, 0xA00),
                          lwzx(0, 12, 0), mtctr(0), kBctr});
  img.word(kBase + 0xA00, kBase + 0x800);

  auto scan = scan_switch_tables(img.bytes, kBase, kCode);
  CHECK(scan.tables.empty());
  CHECK(scan.unresolved == std::vector<uint32_t>{kBase + 0x5C, kBase + 0x9C});
}

TEST_CASE("scan_switch_tables never reads outside the image", "[unit]") {
  FakeImage img;
  img.code(kBase, {kBctr});  // bctr at the very start: nothing before it
  img.code(kBase + 0x10, {cmplwi(6, 11, 1), bgt(6, 0x40), lis(12, 0x9000), slwi(0, 11, 2), lwzx(0, 12, 0), mtctr(0), kBctr});
  std::vector<std::pair<uint32_t, uint32_t>> code = {{kBase, 0x1000}, {kBase + 0x2000, 0x100}};
  auto scan = scan_switch_tables(img.bytes, kBase, code);
  CHECK(scan.tables.empty());
  CHECK(scan.unresolved == std::vector<uint32_t>{kBase + 0x28});
}

TEST_CASE("format_switch_tables writes XenonAnalyse's TOML layout", "[unit]") {
  std::vector<SwitchTable> t = {{0x82001000, 11, 0x82001100, {0x82001010, 0x82001020}}};
  CHECK(format_switch_tables(t) ==
        "[[switch]]\nbase = 0x82001000\nr = 11\ndefault = 0x82001100\nlabels = [\n    0x82001010,\n    0x82001020,\n]\n\n");
}

TEST_CASE("scan_switch_tables follows an index copied by mr before the bound check", "[unit]") {
  FakeImage img;
  const uint32_t mr_31_4 = x31(4, 31, 4, 444);  // or r31, r4, r4
  img.code(kBase, {mr_31_4, cmplwi(6, 4, 1), bgt(6, 0x40), lis(12, 0x8200), slwi(0, 31, 2), addi(12, 12, 0x800),
                   lwzx(0, 12, 0), mtctr(0), kBctr});
  img.word(kBase + 0x800, kBase + 0x30);
  img.word(kBase + 0x804, kBase + 0x34);
  // An unguarded indexed load (array of objects, then a virtual call) is not a switch.
  img.code(kBase + 0x100, {kBlr, slwi(9, 10, 2), lwzx(3, 9, 11), lwz(8, 3, 0), lwz(7, 8, 16), mtctr(7), kBctr});

  auto scan = scan_switch_tables(img.bytes, kBase, kCode);
  REQUIRE(scan.tables.size() == 1);
  CHECK(scan.tables[0].r == 4);
  CHECK(scan.tables[0].labels == std::vector<uint32_t>{kBase + 0x30, kBase + 0x34});
  CHECK(scan.unresolved.empty());
}

TEST_CASE("scan_switch_tables tolerates stores and register moves in the window", "[unit]") {
  FakeImage img;
  const uint32_t stw_11_80_r1 = (36u << 26) | (11u << 21) | (1u << 16) | 80u;
  const uint32_t mr_5_11 = x31(11, 5, 11, 444);  // or r5, r11, r11
  img.code(kBase, {cmplwi(6, 11, 1), stw_11_80_r1, bgt(6, 0x40), mr_5_11, lis(12, 0x8200), slwi(0, 5, 2),
                   addi(12, 12, 0x800), lwzx(0, 12, 0), mtctr(0), kBctr});
  img.word(kBase + 0x800, kBase + 0x30);
  img.word(kBase + 0x804, kBase + 0x34);
  auto scan = scan_switch_tables(img.bytes, kBase, kCode);
  REQUIRE(scan.tables.size() == 1);
  CHECK(scan.tables[0].labels == std::vector<uint32_t>{kBase + 0x30, kBase + 0x34});
}

TEST_CASE("scan_switch_tables skips unrelated instructions scheduled into the window", "[unit]") {
  FakeImage img;
  const uint32_t lfs_31 = (48u << 26) | (31u << 21) | (10u << 16) | 0x100u;  // lfs f31, 0x100(r10)
  const uint32_t subfic_6_7 = (8u << 26) | (6u << 21) | (7u << 16);         // subfic r6, r7, 0
  const uint32_t vmx = (4u << 26) | 0x123u;                                   // some VMX op
  img.code(kBase, {cmplwi(6, 11, 1), lfs_31, subfic_6_7, vmx, bgt(6, 0x40), lis(12, 0x8200), slwi(0, 11, 2),
                   addi(12, 12, 0x800), lwzx(0, 12, 0), mtctr(0), kBctr});
  img.word(kBase + 0x800, kBase + 0x30);
  img.word(kBase + 0x804, kBase + 0x34);
  // A cmplw-guarded virtual call (no indexed load) is neither a table nor unresolved.
  img.code(kBase + 0x100, {x31(6 << 2, 10, 11, 32), bgt(6, 8), lwz(11, 3, 0), lwz(11, 11, 24), mtctr(11), kBctr});
  auto scan = scan_switch_tables(img.bytes, kBase, kCode);
  REQUIRE(scan.tables.size() == 1);
  CHECK(scan.tables[0].labels == std::vector<uint32_t>{kBase + 0x30, kBase + 0x34});
  CHECK(scan.unresolved.empty());
}

TEST_CASE("scan_switch_tables sizes unguarded tables from an index mask", "[unit]") {
  FakeImage img;
  const uint32_t clrlwi_10_9_30 = (21u << 26) | (9u << 21) | (10u << 16) | (0u << 11) | (30u << 6) | (31u << 1);
  img.code(kBase, {kBlr, lwz(9, 3, 0), clrlwi_10_9_30, lis(12, 0x8200), addi(12, 12, 0x900), lbzx(0, 12, 10),
                   slwi(0, 0, 2), lis(12, 0x8200), addi(12, 12, 0x300), add(12, 12, 0), mtctr(12), kBctr});
  img.bytes[0x900] = 0;
  img.bytes[0x901] = 2;
  img.bytes[0x902] = 1;
  img.bytes[0x903] = 0;
  auto scan = scan_switch_tables(img.bytes, kBase, kCode);
  REQUIRE(scan.tables.size() == 1);
  CHECK(scan.tables[0].r == 10);
  CHECK(scan.tables[0].default_label == 0);
  CHECK(scan.tables[0].labels == std::vector<uint32_t>{kBase + 0x300, kBase + 0x308, kBase + 0x304, kBase + 0x300});
}

TEST_CASE("parse_pdata reads begin address, length and exception flag", "[unit]") {
  FakeImage img;
  img.word(kBase + 0xC00, kBase + 0x200);
  img.word(kBase + 0xC04, (0x10u << 8) | 0x80000000u | 3u);  // 0x10 instructions, EH flag
  img.word(kBase + 0xC08, 0);                                // empty entry
  img.word(kBase + 0xC0C, 0);
  img.word(kBase + 0xC10, kBase + 0x100);
  img.word(kBase + 0xC14, 0x8u << 8);
  auto p = parse_pdata(img.bytes, kBase, kBase + 0xC00, 0x18);
  REQUIRE(p.size() == 2);
  CHECK(p[0] == FunctionRange{kBase + 0x100, 0x20, false});
  CHECK(p[1] == FunctionRange{kBase + 0x200, 0x40, true});
}

TEST_CASE("fit_switch_functions sizes leaf functions and checks .pdata functions", "[unit]") {
  FakeImage img;
  // .pdata: A = [0x000, 0x040), C = [0x200, 0x240) with exception handler words at 0x1F8.
  const std::vector<FunctionRange> pdata = {{kBase, 0x40, false}, {kBase + 0x200, 0x40, true}};
  // A calls the leaf at 0x050 and another leaf at 0x0C0; 0x040..0x04F is padding.
  img.word(kBase + 0x10, (18u << 26) | (0x40u & 0x03FFFFFC) | 1u);  // bl +0x40 -> 0x050
  img.word(kBase + 0x14, (18u << 26) | (0xACu & 0x03FFFFFC) | 1u);  // bl +0xAC -> 0x0C0
  for (uint32_t a = kBase + 0x50; a < kBase + 0x1F0; a += 4) img.word(a, kNop);  // leaf code (0x1F0.. padding)
  img.word(kBase + 0xC0, kBlr);
  img.word(kBase + 0x1F8, 0x828B0CD0);  // handler + data words before C
  img.word(kBase + 0x1FC, 0x82000000);
  std::vector<SwitchTable> tables = {
      {kBase + 0x60, 3, 0, {kBase + 0x70, kBase + 0xB0}},   // leaf at 0x050, ends at next bl target 0x0C0
      {kBase + 0xD0, 3, 0, {kBase + 0xE0, kBase + 0x1E0}},  // leaf at 0x0C0, ends before C's handler
      {kBase + 0x20, 3, 0, {kBase + 0x24, kBase + 0x3C}},   // inside A, labels inside: kept
      {kBase + 0x210, 3, 0, {kBase + 0x220, kBase + 0x300}},  // inside C, label outside: dropped
  };
  auto fit = fit_switch_functions(img.bytes, kBase, kCode, pdata, tables);
  CHECK(fit.tables.size() == 3);
  CHECK(fit.dropped == std::vector<uint32_t>{kBase + 0x210});
  REQUIRE(fit.functions.size() == 2);
  CHECK(fit.functions[0] == FunctionRange{kBase + 0x50, 0x70, false});
  CHECK(fit.functions[1] == FunctionRange{kBase + 0xC0, 0x130, false});  // trailing padding trimmed
}
