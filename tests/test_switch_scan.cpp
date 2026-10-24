// SPDX-License-Identifier: MIT
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
  img.code(kBase + 0x40, {cmplwi(6, 11, 4), bgt(6, 0x200), mulli(0, 11, 4), lis(12, 0x8200), addi(12, 12, 0x800),
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

namespace {

// Leaf-function fixture: .pdata A = [0x000, 0x040), C = [0x200, 0x240); leaf code 0x050..0x1F0 (nops);
// A calls 0x050. One table in the leaf at 0x050 with labels up to 0x0B0 -> derived range [0x050, 0x1F0).
struct LeafFixture {
  FakeImage img;
  std::vector<FunctionRange> pdata = {{kBase, 0x40, false}, {kBase + 0x200, 0x40, false}};
  LeafFixture() {
    for (uint32_t a = kBase + 0x50; a < kBase + 0x1F0; a += 4) img.word(a, kNop);
    img.word(kBase + 0x10, (18u << 26) | 0x40u | 1u);  // bl -> 0x050
  }
  std::vector<SwitchTable> tables() const { return {{kBase + 0x60, 3, 0, {kBase + 0x70, kBase + 0xB0}}}; }
};

}  // namespace

TEST_CASE("fit_switch_functions keeps a clean leaf range", "[unit]") {
  LeafFixture f;
  auto fit = fit_switch_functions(f.img.bytes, kBase, kCode, f.pdata, f.tables());
  CHECK(fit.functions == std::vector<FunctionRange>{{kBase + 0x50, 0x1A0, false}});
  CHECK(fit.tables.size() == 1);
  CHECK(fit.dropped.empty());
  CHECK(fit.dropped_functions.empty());
}

TEST_CASE("fit_switch_functions rejects ranges entered from outside", "[unit]") {
  SECTION("a call target strictly inside") {
    LeafFixture f;
    // bl -> 0x090, between the cases (the range is [0x050, 0x1F0) and 0x090 < last label 0x0B0).
    f.img.word(kBase + 0x18, (18u << 26) | (0x78u & 0x03FFFFFC) | 1u);
    auto fit = fit_switch_functions(f.img.bytes, kBase, kCode, f.pdata, f.tables());
    CHECK(fit.functions.empty());
    CHECK(fit.tables.empty());
    CHECK(fit.dropped == std::vector<uint32_t>{kBase + 0x60});
    REQUIRE(fit.dropped_functions.size() == 1);
    CHECK(fit.dropped_functions[0].start == kBase + 0x50);
  }
  SECTION("a plain branch from outside into the middle") {
    LeafFixture f;
    f.img.word(kBase + 0x20, (18u << 26) | (0x80u & 0x03FFFFFC));  // b 0x0A0, from inside .pdata A
    auto fit = fit_switch_functions(f.img.bytes, kBase, kCode, f.pdata, f.tables());
    CHECK(fit.functions.empty());
    CHECK(fit.dropped == std::vector<uint32_t>{kBase + 0x60});
  }
  SECTION("branches inside the range and tail calls to its start are fine") {
    LeafFixture f;
    f.img.word(kBase + 0x80, (18u << 26) | (0x20u & 0x03FFFFFC));       // b 0x0A0 from inside
    f.img.word(kBase + 0x24, (18u << 26) | (0x2Cu & 0x03FFFFFC));       // b 0x050 (tail call to start)
    auto fit = fit_switch_functions(f.img.bytes, kBase, kCode, f.pdata, f.tables());
    CHECK(fit.functions.size() == 1);
  }
}

TEST_CASE("fit_switch_functions rejects overlapping ranges", "[unit]") {
  SECTION("overlap with a hand-written function") {
    LeafFixture f;
    const std::vector<FunctionRange> manual = {{kBase + 0x100, 0x20, false}};
    auto fit = fit_switch_functions(f.img.bytes, kBase, kCode, f.pdata, f.tables(), manual);
    CHECK(fit.functions.empty());
    CHECK(fit.dropped == std::vector<uint32_t>{kBase + 0x60});
  }
  SECTION("two derived ranges that overlap but differ") {
    LeafFixture f;
    // A bl to 0x0C0 ends the second table's range at [0x050, 0x0C0); the first table (labels up to
    // 0x0D0) gets [0x050, 0x1F0). The first is rejected for the call into its middle, the second
    // only for overlapping the first.
    auto t = f.tables();
    t[0].labels = {kBase + 0x70, kBase + 0xD0};                  // first: range runs to the gap end
    t.push_back({kBase + 0x64, 4, 0, {kBase + 0x70, kBase + 0xA8}});  // second: ends at bl target 0x0C0
    f.img.word(kBase + 0x1C, (18u << 26) | (0xA4u & 0x03FFFFFC) | 1u);  // bl -> 0x0C0
    auto fit = fit_switch_functions(f.img.bytes, kBase, kCode, f.pdata, t);
    CHECK(fit.functions.empty());
    CHECK(fit.tables.empty());
    CHECK(fit.dropped.size() == 2);
    CHECK(fit.dropped_functions.size() == 2);
  }
}

TEST_CASE("scan_switch_tables treats ldu/stdu as writing rA", "[unit]") {
  FakeImage img;
  const uint32_t stdu_12 = (62u << 26) | (1u << 21) | (12u << 16) | (0x10u & 0xFFFC) | 1u;  // stdu r1, 0x10(r12)
  img.code(kBase, {cmplwi(6, 11, 1), bgt(6, 0x40), lis(12, 0x8200), addi(12, 12, 0x800), stdu_12, slwi(0, 11, 2),
                   lwzx(0, 12, 0), mtctr(0), kBctr});
  img.word(kBase + 0x800, kBase + 0x30);
  img.word(kBase + 0x804, kBase + 0x34);
  auto scan = scan_switch_tables(img.bytes, kBase, kCode);
  CHECK(scan.tables.empty());  // r12 was updated by stdu: the table address is no longer known
  CHECK(scan.unresolved == std::vector<uint32_t>{kBase + 0x20});

  FakeImage img2;
  const uint32_t ldu_12 = (58u << 26) | (5u << 21) | (12u << 16) | (0x10u & 0xFFFC) | 1u;  // ldu r5, 0x10(r12)
  const uint32_t ld_12 = (58u << 26) | (5u << 21) | (12u << 16) | (0x10u & 0xFFFC);        // ld r5, 0x10(r12)
  for (uint32_t probe : {ldu_12, ld_12}) {
    img2.code(kBase, {cmplwi(6, 11, 1), bgt(6, 0x40), lis(12, 0x8200), addi(12, 12, 0x800), probe, slwi(0, 11, 2),
                      lwzx(0, 12, 0), mtctr(0), kBctr});
    img2.word(kBase + 0x800, kBase + 0x30);
    img2.word(kBase + 0x804, kBase + 0x34);
    auto s2 = scan_switch_tables(img2.bytes, kBase, kCode);
    CHECK(s2.tables.size() == (probe == ld_12 ? 1u : 0u));
  }
}

namespace {

uint32_t rlwinm(uint32_t a, uint32_t s, uint32_t sh, uint32_t mb, uint32_t me) {
  return (21u << 26) | (s << 21) | (a << 16) | (sh << 11) | (mb << 6) | (me << 1);
}
uint32_t lhzx(uint32_t d, uint32_t a, uint32_t b) { return x31(d, a, b, 279); }

void half(FakeImage& img, uint32_t addr, uint32_t v) {
  img.bytes[addr - kBase] = static_cast<uint8_t>(v >> 8);
  img.bytes[addr - kBase + 1] = static_cast<uint8_t>(v);
}

// Halfword table at 0x300 with byte offsets relative to the table itself. The compiler hoisted
// the table address above the bound check and scales the index with `add rI, rIdx, rIdx`.
//   lis r12; cmplwi cr6, r11, 34; addi r12, r12, 0x300; bgt cr6, default
//   add r10, r11, r11; lhzx r0, r12, r10; add r0, r0, r12; mtctr r0; bctr
constexpr uint32_t kHalfCases = 35, kHalfTable = kBase + 0x300;
uint32_t half_offset(uint32_t v) { return 0x50 + 4 * ((v * 7) % kHalfCases); }

void halfword_relative_switch(FakeImage& img) {
  img.code(kBase, {lis(12, 0x8200), cmplwi(6, 11, kHalfCases - 1), addi(12, 12, 0x300), bgt(6, 0x100),
                   add(10, 11, 11), lhzx(0, 12, 10), add(0, 0, 12), mtctr(0), kBctr});
  for (uint32_t v = 0; v < kHalfCases; ++v) half(img, kHalfTable + 2 * v, half_offset(v));
}

}  // namespace

TEST_CASE("scan_switch_tables: halfword relative table resolves 35 labels", "[unit]") {
  FakeImage img;
  halfword_relative_switch(img);
  auto scan = scan_switch_tables(img.bytes, kBase, kCode);
  REQUIRE(scan.tables.size() == 1);
  CHECK(scan.unresolved.empty());
  const auto& t = scan.tables[0];
  CHECK(t.base == kBase + 0x20);
  CHECK(t.r == 11);
  CHECK(t.default_label == kBase + 0x10C);
  REQUIRE(t.labels.size() == kHalfCases);
  for (uint32_t v = 0; v < kHalfCases; ++v) CHECK(t.labels[v] == kHalfTable + half_offset(v));
}

TEST_CASE("scan_switch_tables: byte relative table resolves 17 labels", "[unit]") {
  FakeImage img;
  // Byte table at 0x380 with offsets relative to the table; table address built across the guard,
  // and the index copied by mr before the compare.
  //   mr r31, r4; lis r9; cmplwi cr6, r4, 16; bgt cr6, default; addi r9, r9, 0x380
  //   lbzx r8, r9, r31; add r8, r8, r9; mtctr r8; bctr
  constexpr uint32_t kCases = 17, kTable = kBase + 0x380;
  img.code(kBase + 0x40, {x31(4, 31, 4, 444), lis(9, 0x8200), cmplwi(6, 4, kCases - 1), bgt(6, 0x80),
                          addi(9, 9, 0x380), lbzx(8, 9, 31), add(8, 8, 9), mtctr(8), kBctr});
  auto offset = [](uint32_t v) { return 0x14 + 4 * ((v * 5) % kCases); };
  for (uint32_t v = 0; v < kCases; ++v) img.bytes[kTable - kBase + v] = static_cast<uint8_t>(offset(v));

  auto scan = scan_switch_tables(img.bytes, kBase, kCode);
  REQUIRE(scan.tables.size() == 1);
  CHECK(scan.unresolved.empty());
  const auto& t = scan.tables[0];
  CHECK(t.base == kBase + 0x60);
  CHECK(t.r == 4);
  CHECK(t.default_label == kBase + 0x4C + 0x80);
  REQUIRE(t.labels.size() == kCases);
  for (uint32_t v = 0; v < kCases; ++v) CHECK(t.labels[v] == kTable + offset(v));
}

TEST_CASE("scan_switch_tables: relative table with scale 2", "[unit]") {
  FakeImage img;
  // Index scaled before the compare, halfword entries counted in instructions from a separate
  // code base, everything after the guard:
  //   rlwinm r10, r11, 1, 0, 30; cmplwi cr6, r11, 3; bgt cr6, default; lis r12; addi r12, r12, 0x900
  //   lhzx r0, r12, r10; lis r9; addi r9, r9, 0x100; rlwinm r0, r0, 2, 0, 29; add r0, r0, r9; mtctr r0; bctr
  img.code(kBase + 0x80, {rlwinm(10, 11, 1, 0, 30), cmplwi(6, 11, 3), bgt(6, 0x40), lis(12, 0x8200),
                          addi(12, 12, 0x900), lhzx(0, 12, 10), lis(9, 0x8200), addi(9, 9, 0x100),
                          rlwinm(0, 0, 2, 0, 29), add(0, 0, 9), mtctr(0), kBctr});
  const uint32_t entries[] = {0, 3, 1, 0xBF};
  for (uint32_t v = 0; v < 4; ++v) half(img, kBase + 0x900 + 2 * v, entries[v]);

  auto scan = scan_switch_tables(img.bytes, kBase, kCode);
  REQUIRE(scan.tables.size() == 1);
  CHECK(scan.tables[0].r == 11);
  CHECK(scan.tables[0].labels == std::vector<uint32_t>{kBase + 0x100, kBase + 0x10C, kBase + 0x104, kBase + 0x3FC});
}

TEST_CASE("scan_switch_tables: relative table without bound is reported unbounded", "[unit]") {
  FakeImage img;
  // Same code as the halfword table, minus the cmplwi/bgt: the number of cases is unknown, so no
  // table is emitted; the site is reported for a human look.
  img.code(kBase, {kBlr, lis(12, 0x8200), addi(12, 12, 0x300), add(10, 11, 11), lhzx(0, 12, 10), add(0, 0, 12),
                   mtctr(0), kBctr});
  for (uint32_t v = 0; v < kHalfCases; ++v) half(img, kHalfTable + 2 * v, half_offset(v));
  auto scan = scan_switch_tables(img.bytes, kBase, kCode);
  CHECK(scan.tables.empty());
  CHECK(scan.unbounded == std::vector<uint32_t>{kBase + 0x1C});
}

TEST_CASE("scan_switch_tables: relative table label outside code section rejects table", "[unit]") {
  SECTION("label past the end of the code section") {
    FakeImage img;
    halfword_relative_switch(img);
    half(img, kHalfTable + 2 * 20, 0x100);  // 0x300 + 0x100 = 0x400: first byte after the code
    auto scan = scan_switch_tables(img.bytes, kBase, kCode);
    CHECK(scan.tables.empty());
    CHECK(scan.unresolved == std::vector<uint32_t>{kBase + 0x20});
  }
  SECTION("misaligned label") {
    FakeImage img;
    halfword_relative_switch(img);
    half(img, kHalfTable + 2 * 34, 0x52);
    auto scan = scan_switch_tables(img.bytes, kBase, kCode);
    CHECK(scan.tables.empty());
    CHECK(scan.unresolved == std::vector<uint32_t>{kBase + 0x20});
  }
}

TEST_CASE("scan_switch_tables: relative table hoisting stops at the index copy", "[unit]") {
  FakeImage img;
  // r31 is read before `mr r31, r11` makes it a copy of the index: the scaled index in r10 is not
  // known, so the site must stay unresolved instead of yielding labels computed from a stale r31.
  //   rlwinm r10, r31, 1, 0, 30; mr r31, r11; cmplwi cr6, r11, 34; bgt cr6, default
  //   lis r12; addi r12, r12, 0x300; lhzx r0, r12, r10; add r0, r0, r12; mtctr r0; bctr
  img.code(kBase, {rlwinm(10, 31, 1, 0, 30), x31(11, 31, 11, 444), cmplwi(6, 11, kHalfCases - 1), bgt(6, 0x100),
                   lis(12, 0x8200), addi(12, 12, 0x300), lhzx(0, 12, 10), add(0, 0, 12), mtctr(0), kBctr});
  for (uint32_t v = 0; v < kHalfCases; ++v) half(img, kHalfTable + 2 * v, half_offset(v));
  auto scan = scan_switch_tables(img.bytes, kBase, kCode);
  CHECK(scan.tables.empty());
  CHECK(scan.unresolved == std::vector<uint32_t>{kBase + 0x24});
}

TEST_CASE("scan_switch_tables: relative table hoisting stops at a branch target", "[unit]") {
  // lis r12 (0x00); addi r12, r12, 0x300 (0x04); cmplwi cr6, r11, 34 (0x08); bgt cr6 (0x0C);
  // add r10, r11, r11; lhzx r0, r12, r10; add r0, r0, r12; mtctr r0; bctr (0x20).
  // Another path entering between the lis and the cmplwi brings its own r12: the base is unknown.
  auto build = [](FakeImage& img) {
    img.code(kBase, {lis(12, 0x8200), addi(12, 12, 0x300), cmplwi(6, 11, kHalfCases - 1), bgt(6, 0x100),
                     add(10, 11, 11), lhzx(0, 12, 10), add(0, 0, 12), mtctr(0), kBctr});
    for (uint32_t v = 0; v < kHalfCases; ++v) half(img, kHalfTable + 2 * v, half_offset(v));
  };
  auto b = [](uint32_t from, uint32_t to) { return (18u << 26) | ((to - from) & 0x03FFFFFC); };
  auto beq = [](uint32_t from, uint32_t to) { return (16u << 26) | (12u << 21) | (2u << 16) | ((to - from) & 0xFFFC); };
  SECTION("b into the hoisted range") {
    FakeImage img;
    build(img);
    img.word(kBase + 0x40, b(kBase + 0x40, kBase + 0x04));
    auto scan = scan_switch_tables(img.bytes, kBase, kCode);
    CHECK(scan.tables.empty());
    CHECK(scan.unresolved == std::vector<uint32_t>{kBase + 0x20});
  }
  SECTION("bc into the hoisted range") {
    FakeImage img;
    build(img);
    img.word(kBase + 0x40, beq(kBase + 0x40, kBase + 0x04));
    auto scan = scan_switch_tables(img.bytes, kBase, kCode);
    CHECK(scan.tables.empty());
    CHECK(scan.unresolved == std::vector<uint32_t>{kBase + 0x20});
  }
  SECTION("a target at the window start is fine") {
    FakeImage img;
    build(img);
    img.word(kBase + 0x40, b(kBase + 0x40, kBase));
    auto scan = scan_switch_tables(img.bytes, kBase, kCode);
    REQUIRE(scan.tables.size() == 1);
    CHECK(scan.tables[0].labels.size() == kHalfCases);
  }
  SECTION("never hoists below the code range start") {
    FakeImage img;
    build(img);
    const std::vector<std::pair<uint32_t, uint32_t>> code = {{kBase + 4, 0x3FC}};
    auto scan = scan_switch_tables(img.bytes, kBase, code);
    CHECK(scan.tables.empty());
    CHECK(scan.unresolved == std::vector<uint32_t>{kBase + 0x20});
  }
}

namespace {

uint32_t b_to(uint32_t from, uint32_t to) { return (18u << 26) | ((to - from) & 0x03FFFFFC); }
uint32_t bl_to(uint32_t from, uint32_t to) { return b_to(from, to) | 1u; }
// ble crN, target (bc 4, 4*crN+gt): branch if "greater than" is clear.
uint32_t ble_to(uint32_t crf, uint32_t from, uint32_t to) {
  return (16u << 26) | (4u << 21) | ((crf * 4 + 1) << 16) | ((to - from) & 0xFFFC);
}
uint32_t li(uint32_t d, int32_t imm) { return addi(d, 0, imm); }

// Inverted guard: `cmplwi cr6, r11, 34; ble cr6, +8; b default`, then the halfword table block.
void inverted_guard_switch(FakeImage& img) {
  img.code(kBase, {lwz(11, 14, 28), cmplwi(6, 11, kHalfCases - 1), ble_to(6, kBase + 0x08, kBase + 0x10),
                   b_to(kBase + 0x0C, kBase + 0x100), lis(12, 0x8200), rlwinm(0, 11, 1, 0, 30),
                   addi(12, 12, 0x300), lhzx(0, 12, 0), lis(12, 0x8200), addi(12, 12, 0), kNop, add(12, 12, 0),
                   mtctr(12), kBctr});
  for (uint32_t v = 0; v < kHalfCases; ++v) half(img, kHalfTable + 2 * v, half_offset(v));
}

}  // namespace

TEST_CASE("scan_switch_tables: inverted guard (ble over an unconditional b to the default)", "[unit]") {
  SECTION("resolves") {
    FakeImage img;
    inverted_guard_switch(img);
    auto scan = scan_switch_tables(img.bytes, kBase, kCode);
    REQUIRE(scan.tables.size() == 1);
    const auto& t = scan.tables[0];
    CHECK(t.base == kBase + 0x34);
    CHECK(t.r == 11);
    CHECK(t.default_label == kBase + 0x100);
    REQUIRE(t.labels.size() == kHalfCases);
    for (uint32_t v = 0; v < kHalfCases; ++v) CHECK(t.labels[v] == kBase + half_offset(v));
    CHECK(scan.unbounded.empty());
  }
  SECTION("another path into the table block") {
    FakeImage img;
    inverted_guard_switch(img);
    img.word(kBase + 0x200, b_to(kBase + 0x200, kBase + 0x10));
    auto scan = scan_switch_tables(img.bytes, kBase, kCode);
    CHECK(scan.tables.empty());
    CHECK(scan.unresolved.empty());
    CHECK(scan.joined == std::vector<uint32_t>{kBase + 0x34});
  }
  SECTION("the skipped branch is not a bound check") {
    FakeImage img;
    inverted_guard_switch(img);
    img.word(kBase + 0x08, (16u << 26) | (4u << 21) | ((6u * 4 + 0) << 16) | 8u);  // bge cr6 (lt clear)
    auto scan = scan_switch_tables(img.bytes, kBase, kCode);
    CHECK(scan.tables.empty());
  }
}

namespace {

// Function [0x000, 0x100) (in .pdata): a guarded byte switch on r9 whose case 1/2 label is the block
// of a second, unguarded halfword switch on the same r9.
//   0x00 cmplwi cr6, r9, 2; bgt cr6, 0x80; lis r12; addi r12, r12, 0x380; lbzx r0, r12, r9
//   0x14 lis r12; add r12, r12, r0; mtctr r12; bctr (0x20)       -> labels 0x24, 0x2C, 0x2C
//   0x24 li r3, 1; nop
//   0x2C lis r11; add r10, r9, r9; lis r12; addi r12, r12, 0x3A0; lhzx r0, r12, r10; add r0, r0, r11
//   0x44 mtctr r0; bctr (0x48)                                      -> labels 0x4C, 0x54, 0x50
//   0x4C blr; blr; blr ... 0x80 blr
struct DominatedFixture {
  FakeImage img;
  std::vector<FunctionRange> pdata = {{kBase, 0x100, false}};
  DominatedFixture() {
    img.code(kBase, {cmplwi(6, 9, 2), bgt(6, 0x7C), lis(12, 0x8200), addi(12, 12, 0x380), lbzx(0, 12, 9),
                     lis(12, 0x8200), add(12, 12, 0), mtctr(12), kBctr, li(3, 1), kNop,
                     lis(11, 0x8200), add(10, 9, 9), lis(12, 0x8200), addi(12, 12, 0x3A0), lhzx(0, 12, 10),
                     add(0, 0, 11), mtctr(0), kBctr, kBlr, kBlr, kBlr});
    img.word(kBase + 0x80, kBlr);
    img.bytes[0x380] = 0x24;
    img.bytes[0x381] = 0x2C;
    img.bytes[0x382] = 0x2C;
    half(img, kBase + 0x3A0, 0x4C);
    half(img, kBase + 0x3A2, 0x54);
    half(img, kBase + 0x3A4, 0x50);
  }
  std::vector<SwitchTable> resolve() const {
    auto scan = scan_switch_tables(img.bytes, kBase, kCode);
    return resolve_dominated_switches(img.bytes, kBase, kCode, pdata, scan.tables, scan.unbounded);
  }
};

}  // namespace

TEST_CASE("resolve_dominated_switches bounds an index checked in an earlier block", "[unit]") {
  DominatedFixture f;
  auto scan = scan_switch_tables(f.img.bytes, kBase, kCode);
  REQUIRE(scan.tables.size() == 1);
  REQUIRE(scan.unbounded == std::vector<uint32_t>{kBase + 0x48});
  auto t = f.resolve();
  REQUIRE(t.size() == 1);
  CHECK(t[0].base == kBase + 0x48);
  CHECK(t[0].r == 9);
  CHECK(t[0].default_label == 0);
  CHECK(t[0].labels == std::vector<uint32_t>{kBase + 0x4C, kBase + 0x54, kBase + 0x50});
}

TEST_CASE("resolve_dominated_switches refuses when the bound does not hold on every path", "[unit]") {
  SECTION("the index is rewritten on one path") {
    DominatedFixture f;
    f.img.word(kBase + 0x24, li(9, 7));
    CHECK(f.resolve().empty());
  }
  SECTION("a call on one path") {
    DominatedFixture f;
    f.img.word(kBase + 0x28, bl_to(kBase + 0x28, kBase + 0x200));
    CHECK(f.resolve().empty());
  }
  SECTION("a branch from another function into the block") {
    DominatedFixture f;
    f.img.word(kBase + 0x200, b_to(kBase + 0x200, kBase + 0x2C));
    CHECK(f.resolve().empty());
  }
  SECTION("a case of the new table changes the index and loops back") {
    DominatedFixture f;
    f.img.code(kBase + 0x50, {addi(9, 9, 1), b_to(kBase + 0x54, kBase + 0x2C)});
    half(f.img, kBase + 0x3A2, 0x50);
    half(f.img, kBase + 0x3A4, 0x4C);
    CHECK(f.resolve().empty());
  }
  SECTION("an unresolved switch elsewhere in the function") {
    DominatedFixture f;
    // Case 0 jumps to 0x60: an unguarded indexed jump with unknown targets (could land anywhere in
    // the function, e.g. on the block at 0x2C with r9 changed).
    f.img.word(kBase + 0x24, b_to(kBase + 0x24, kBase + 0x60));
    f.img.code(kBase + 0x60, {lis(12, 0x8200), lwzx(0, 12, 5), mtctr(0), kBctr});
    CHECK(f.resolve().empty());
  }
  SECTION("outside any .pdata function") {
    DominatedFixture f;
    f.pdata.clear();
    CHECK(f.resolve().empty());
  }
}

TEST_CASE("fit_switch_functions ends a leaf range at padding after its last case", "[unit]") {
  SECTION("blr + zero word after the last label") {
    LeafFixture f;
    f.img.word(kBase + 0xC0, kBlr);
    f.img.word(kBase + 0xC4, 0);  // padding: a separate (pointer-only) function starts at 0x0C8
    auto fit = fit_switch_functions(f.img.bytes, kBase, kCode, f.pdata, f.tables());
    CHECK(fit.functions == std::vector<FunctionRange>{{kBase + 0x50, 0x74, false}});
    CHECK(fit.tables.size() == 1);
  }
  SECTION("padding before a later in-function branch target is not an end") {
    LeafFixture f;
    f.img.word(kBase + 0xC0, kBlr);
    f.img.word(kBase + 0xC4, 0);
    f.img.word(kBase + 0x90, (16u << 26) | (12u << 21) | (2u << 16) | (0x40u & 0xFFFC));  // beq 0x0D0
    auto fit = fit_switch_functions(f.img.bytes, kBase, kCode, f.pdata, f.tables());
    CHECK(fit.functions == std::vector<FunctionRange>{{kBase + 0x50, 0x1A0, false}});
  }
  SECTION("a zero word not preceded by an unconditional branch is not an end") {
    LeafFixture f;
    f.img.word(kBase + 0xC4, 0);
    auto fit = fit_switch_functions(f.img.bytes, kBase, kCode, f.pdata, f.tables());
    CHECK(fit.functions == std::vector<FunctionRange>{{kBase + 0x50, 0x1A0, false}});
  }
}

TEST_CASE("fit_switch_functions splits leaf functions at padding on both sides", "[unit]") {
  // Leaf A at 0x050 (switch, labels up to 0x0B0, blr at 0x0C0), padding at 0x0C4, leaf B at 0x0C8
  // (reached only through a pointer: no bl) with its own switch, labels up to 0x100.
  LeafFixture f;
  f.img.word(kBase + 0xC0, kBlr);
  f.img.word(kBase + 0xC4, 0);
  auto t = f.tables();
  t.push_back({kBase + 0xD0, 3, 0, {kBase + 0xE0, kBase + 0x100}});
  auto fit = fit_switch_functions(f.img.bytes, kBase, kCode, f.pdata, t);
  CHECK(fit.dropped.empty());
  CHECK(fit.dropped_functions.empty());
  CHECK(fit.functions == std::vector<FunctionRange>{{kBase + 0x50, 0x74, false}, {kBase + 0xC8, 0x128, false}});
  CHECK(fit.tables.size() == 2);
}

namespace {

uint32_t cmpwi(uint32_t crf, uint32_t r, int32_t imm) {
  return (11u << 26) | (crf << 23) | (r << 16) | (static_cast<uint32_t>(imm) & 0xFFFF);
}
// bc bo, bi, target (relative, no link).
uint32_t bc_to(uint32_t bo, uint32_t bi, uint32_t from, uint32_t to) {
  return (16u << 26) | (bo << 21) | (bi << 16) | ((to - from) & 0xFFFC);
}

// Resolves the fixture's second switch (0x48) after a mutation, with the tables found on the
// unmutated code: only the dataflow's own reasoning decides.
std::vector<SwitchTable> resolve_after(void (*mutate)(DominatedFixture&)) {
  DominatedFixture f;
  const auto scan = scan_switch_tables(f.img.bytes, kBase, kCode);
  mutate(f);
  const std::vector<uint32_t> site = {kBase + 0x48};
  return resolve_dominated_switches(f.img.bytes, kBase, kCode, f.pdata, scan.tables, site);
}

}  // namespace

TEST_CASE("resolve_dominated_switches: only an unsigned compare of the index on the right CR bit bounds it", "[unit]") {
  CHECK(resolve_after([](DominatedFixture&) {}).size() == 1);  // control
  SECTION("signed compare (cmpwi)") {
    CHECK(resolve_after([](DominatedFixture& f) { f.img.word(kBase, cmpwi(6, 9, 2)); }).empty());
  }
  SECTION("blt instead of bgt") {
    CHECK(resolve_after([](DominatedFixture& f) { f.img.word(kBase + 4, bc_to(12, 6 * 4 + 0, kBase + 4, kBase + 0x80)); }).empty());
  }
  SECTION("bge instead of bgt") {
    CHECK(resolve_after([](DominatedFixture& f) { f.img.word(kBase + 4, bc_to(4, 6 * 4 + 0, kBase + 4, kBase + 0x80)); }).empty());
  }
  SECTION("ble instead of bgt (in-range edge is the taken one, not the fall-through)") {
    CHECK(resolve_after([](DominatedFixture& f) { f.img.word(kBase + 4, bc_to(4, 6 * 4 + 1, kBase + 4, kBase + 0x80)); }).empty());
  }
  SECTION("compare in cr7, bgt on cr6") {
    CHECK(resolve_after([](DominatedFixture& f) { f.img.word(kBase, cmplwi(7, 9, 2)); }).empty());
  }
  SECTION("compare of another register in the right CR field") {
    CHECK(resolve_after([](DominatedFixture& f) { f.img.word(kBase, cmplwi(6, 10, 2)); }).empty());
  }
  SECTION("ambiguous index: the target does not depend on any input register") {
    CHECK(resolve_after([](DominatedFixture& f) { f.img.word(kBase + 0x30, li(10, 0)); }).empty());
  }
}

TEST_CASE("resolve_dominated_switches: labels of dropped tables are entries", "[unit]") {
  DominatedFixture f;
  const auto scan = scan_switch_tables(f.img.bytes, kBase, kCode);
  const std::vector<uint32_t> site = {kBase + 0x48};
  CHECK(resolve_dominated_switches(f.img.bytes, kBase, kCode, f.pdata, scan.tables, site).size() == 1);
  const std::vector<uint32_t> dropped = {kBase + 0x2C};
  CHECK(resolve_dominated_switches(f.img.bytes, kBase, kCode, f.pdata, scan.tables, site, dropped).empty());
}

TEST_CASE("scan_switch_tables: another path joining after the compare disables the guard", "[unit]") {
  SECTION("inverted guard: compare in cr7, ble on cr6") {
    FakeImage img;
    inverted_guard_switch(img);
    img.word(kBase + 0x04, cmplwi(7, 11, kHalfCases - 1));
    auto scan = scan_switch_tables(img.bytes, kBase, kCode);
    CHECK(scan.tables.empty());
  }
  SECTION("inverted guard: branch between the compare and the ble") {
    FakeImage img;
    inverted_guard_switch(img);
    img.word(kBase + 0x04, kNop);
    img.code(kBase, {cmplwi(6, 11, kHalfCases - 1), kNop});  // cmplwi at 0x00, nop at 0x04 (joined)
    img.word(kBase + 0x200, b_to(kBase + 0x200, kBase + 0x04));
    auto scan = scan_switch_tables(img.bytes, kBase, kCode);
    CHECK(scan.tables.empty());
    CHECK(scan.joined == std::vector<uint32_t>{kBase + 0x34});
  }
  // bgt form: lis r12 (0x00); addi (0x04); cmplwi (0x08); bgt (0x0C); add (0x10); lhzx; add; mtctr; bctr (0x20)
  auto build = [](FakeImage& img) {
    img.code(kBase, {lis(12, 0x8200), addi(12, 12, 0x300), cmplwi(6, 11, kHalfCases - 1), bgt(6, 0x100),
                     add(10, 11, 11), lhzx(0, 12, 10), add(0, 0, 12), mtctr(0), kBctr});
    for (uint32_t v = 0; v < kHalfCases; ++v) half(img, kHalfTable + 2 * v, half_offset(v));
  };
  SECTION("bgt form: control") {
    FakeImage img;
    build(img);
    CHECK(scan_switch_tables(img.bytes, kBase, kCode).tables.size() == 1);
  }
  SECTION("bgt form: branch onto the bgt") {
    FakeImage img;
    build(img);
    img.word(kBase + 0x200, b_to(kBase + 0x200, kBase + 0x0C));
    auto scan = scan_switch_tables(img.bytes, kBase, kCode);
    CHECK(scan.tables.empty());
    CHECK(scan.joined == std::vector<uint32_t>{kBase + 0x20});
  }
  SECTION("bgt form: branch between the bgt and the bctr") {
    FakeImage img;
    build(img);
    img.word(kBase + 0x200, b_to(kBase + 0x200, kBase + 0x14));
    auto scan = scan_switch_tables(img.bytes, kBase, kCode);
    CHECK(scan.tables.empty());
    CHECK(scan.joined == std::vector<uint32_t>{kBase + 0x20});
  }
}

TEST_CASE("fit_switch_functions keeps the start when code after the padding branches back before it", "[unit]") {
  LeafFixture f;  // bl -> 0x050; table base 0x060, labels 0x070..0x0B0
  f.img.word(kBase + 0x54, kBlr);
  f.img.word(kBase + 0x58, 0);                                 // "padding" below the table...
  f.img.word(kBase + 0x90, b_to(kBase + 0x90, kBase + 0x50));  // ...but 0x090 loops back to 0x050
  auto fit = fit_switch_functions(f.img.bytes, kBase, kCode, f.pdata, f.tables());
  CHECK(fit.functions == std::vector<FunctionRange>{{kBase + 0x50, 0x1A0, false}});
  CHECK(fit.dropped.empty());
}

namespace {

// Loop whose latch re-checks the index and branches back into the table block, past the guard:
//   0x00 cmplwi cr6, r11, 2; bgt cr6, 0x80
//   0x08 lis r12; add r10, r11, r11; addi r12, r12, 0x3A0; lhzx r0, r12, r10; lis r9; add r0, r0, r9
//   0x20 mtctr r0; bctr (0x24)                         -> labels 0x28, 0x30, 0x38
//   0x28 lwz r11, 0(r3); b 0x40;  0x30 blr;  0x38 blr
//   0x40 cmplwi cr6, r11, 2; ble cr6, 0x08 (back edge into the table block); blr
struct LoopFixture {
  FakeImage img;
  std::vector<FunctionRange> pdata = {{kBase, 0x100, false}};
  LoopFixture() {
    img.code(kBase, {cmplwi(6, 11, 2), bgt(6, 0x7C), lis(12, 0x8200), add(10, 11, 11), addi(12, 12, 0x3A0),
                     lhzx(0, 12, 10), lis(9, 0x8200), add(0, 0, 9), mtctr(0), kBctr, lwz(11, 3, 0),
                     b_to(kBase + 0x2C, kBase + 0x40), kBlr, kNop, kBlr});
    img.code(kBase + 0x40, {cmplwi(6, 11, 2), ble_to(6, kBase + 0x44, kBase + 0x08), kBlr});
    img.word(kBase + 0x80, kBlr);
    half(img, kBase + 0x3A0, 0x28);
    half(img, kBase + 0x3A2, 0x30);
    half(img, kBase + 0x3A4, 0x38);
  }
  std::vector<SwitchTable> resolve() const {
    auto scan = scan_switch_tables(img.bytes, kBase, kCode);
    CHECK(scan.tables.empty());
    return resolve_dominated_switches(img.bytes, kBase, kCode, pdata, scan.tables, scan.joined);
  }
};

}  // namespace

TEST_CASE("resolve_dominated_switches proves a guard joined by a re-checked loop back edge", "[unit]") {
  SECTION("latch re-checks the same bound") {
    LoopFixture f;
    auto t = f.resolve();
    REQUIRE(t.size() == 1);
    CHECK(t[0].base == kBase + 0x24);
    CHECK(t[0].r == 11);
    CHECK(t[0].labels == std::vector<uint32_t>{kBase + 0x28, kBase + 0x30, kBase + 0x38});
  }
  SECTION("latch checks a larger bound") {
    LoopFixture f;
    f.img.word(kBase + 0x40, cmplwi(6, 11, 5));
    CHECK(f.resolve().empty());
  }
  SECTION("latch does not check") {
    LoopFixture f;
    f.img.word(kBase + 0x44, b_to(kBase + 0x44, kBase + 0x08));
    CHECK(f.resolve().empty());
  }
}

// XenonRecomp's own analysis of a bl-target function follows a forward `b` (a tail jump) into the next function: when
// that next function is a derived leaf range, the analysed function swallows its switch and XenonRecomp reports the
// cases as outside the function. The function just before the leaf range is then bounded explicitly too.
TEST_CASE("fit_switch_functions bounds the function that tail-jumps into a leaf range", "[unit]") {
  LeafFixture f;
  // P = [0x040, 0x050): li; beq 0x04C; b 0x050 (tail jump into the leaf); blr. A calls P.
  f.img.code(kBase + 0x40, {addi(3, 0, 0), (16u << 26) | (12u << 21) | (2u << 16) | 8u, (18u << 26) | 8u, kBlr});
  f.img.word(kBase + 0x14, (18u << 26) | (0x2Cu & 0x03FFFFFC) | 1u);  // bl -> 0x040
  auto fit = fit_switch_functions(f.img.bytes, kBase, kCode, f.pdata, f.tables());
  CHECK(fit.functions == std::vector<FunctionRange>{{kBase + 0x40, 0x10, false}, {kBase + 0x50, 0x1A0, false}});
  CHECK(fit.tables.size() == 1);
  CHECK(fit.dropped_functions.empty());

  SECTION("no forward branch into the leaf: nothing extra") {
    f.img.word(kBase + 0x48, kNop);
    auto plain = fit_switch_functions(f.img.bytes, kBase, kCode, f.pdata, f.tables());
    CHECK(plain.functions == std::vector<FunctionRange>{{kBase + 0x50, 0x1A0, false}});
  }
  SECTION("a branch from outside into P's interior rejects P, and the table goes with it") {
    f.img.word(kBase + 0x20, (18u << 26) | (0x2Cu & 0x03FFFFFC));  // b 0x04C, from inside .pdata A
    auto bad = fit_switch_functions(f.img.bytes, kBase, kCode, f.pdata, f.tables());
    CHECK(bad.tables.empty());
    CHECK(bad.dropped == std::vector<uint32_t>{kBase + 0x60});
  }
}
