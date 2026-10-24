// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "pipeline/function_discovery.h"

using namespace lr::pipeline;

namespace {

constexpr uint32_t kBase = 0x82000000;

// Synthetic PowerPC encoders (big-endian words written into a fake image).
uint32_t addi(uint32_t d, uint32_t a, int32_t imm) { return (14u << 26) | (d << 21) | (a << 16) | (static_cast<uint32_t>(imm) & 0xFFFF); }
uint32_t li(uint32_t d, int32_t imm) { return addi(d, 0, imm); }
uint32_t lis(uint32_t r, uint32_t imm) { return (15u << 26) | (r << 21) | imm; }
uint32_t cmpwi(uint32_t r, int32_t imm) { return (11u << 26) | (r << 16) | (static_cast<uint32_t>(imm) & 0xFFFF); }
uint32_t beq(int32_t disp) { return (16u << 26) | (12u << 21) | (2u << 16) | (static_cast<uint32_t>(disp) & 0xFFFC); }
uint32_t b(int32_t disp) { return (18u << 26) | (static_cast<uint32_t>(disp) & 0x03FFFFFC); }
uint32_t bl(int32_t disp) { return b(disp) | 1; }
uint32_t lwzx(uint32_t d, uint32_t a, uint32_t r) { return (31u << 26) | (d << 21) | (a << 16) | (r << 11) | (23u << 1); }
uint32_t mtctr(uint32_t s) { return (31u << 26) | (s << 21) | (9u << 16) | (467u << 1); }
constexpr uint32_t kBlr = 0x4E800020, kBctr = 0x4E800420;

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

// Code is the first 0x400 bytes; everything after it is data.
const std::vector<CodeRange> kCode = {{kBase, 0x400}};

}  // namespace

TEST_CASE("data table of code pointers yields functions", "[unit]") {
  FakeImage img;
  img.code(kBase, {li(3, 0), li(3, 0), li(3, 0), li(3, 0), li(3, 0), li(3, 0), li(3, 0), kBlr});  // .pdata F0
  img.code(kBase + 0x100, {li(3, 0), li(4, 0), li(5, 0), kBlr});                                 // .pdata F1
  // Four tiny functions without .pdata in the gap [0x20, 0x100).
  img.code(kBase + 0x20, {li(3, 1), kBlr});
  img.code(kBase + 0x28, {li(3, 0), kBlr});
  img.code(kBase + 0x30, {cmpwi(3, 0), beq(0xC), li(3, 1), kBlr, li(3, 0), kBlr});  // blr nested under the beq
  img.code(kBase + 0x50, {li(3, 2), b(0x100 - 0x54)});                               // after padding; tail call to F1
  img.code(kBase + 0x800, {kBase + 0x20, kBase + 0x28, kBase + 0x30, kBase + 0x50});  // callback table (data)
  const std::vector<FunctionRange> known = {{kBase, 0x20, false}, {kBase + 0x100, 0x10, false}};

  auto fns = discover_pointer_functions(img.bytes, kBase, kCode, known);
  CHECK(fns == std::vector<FunctionRange>{{kBase + 0x20, 8, false},
                                          {kBase + 0x28, 8, false},
                                          {kBase + 0x30, 0x18, false},
                                          {kBase + 0x50, 8, false}});
}

TEST_CASE("bl target bounds a pointer candidate and is not emitted", "[unit]") {
  FakeImage img;
  // F0 calls 0x60 and 0x78: XenonRecomp creates those functions itself, so they are only boundaries here.
  img.code(kBase, {li(3, 0), bl(0x60 - 0x4), bl(0x78 - 0x8), kBlr});
  img.code(kBase + 0x50, {li(3, 1), b(0x60 - 0x54)});  // pointer candidate: tail call to the bl target
  img.code(kBase + 0x60, {li(3, 7), kBlr});
  img.code(kBase + 0x70, {li(3, 1), li(4, 1)});  // pointer candidate falling through into the bl target 0x78
  img.code(kBase + 0x78, {li(3, 7), kBlr});
  img.code(kBase + 0x800, {kBase + 0x50, kBase + 0x70});
  const std::vector<FunctionRange> known = {{kBase, 0x10, false}};

  auto fns = discover_pointer_functions(img.bytes, kBase, kCode, known);
  CHECK(fns == std::vector<FunctionRange>{{kBase + 0x50, 8, false}});
}

TEST_CASE("candidate containing an unresolved switch bctr is rejected", "[unit]") {
  FakeImage img;
  img.code(kBase, {li(3, 0), kBlr});  // .pdata F0
  // The bctr is skipped over by the beq, so it is not the end; its case labels would be cut off.
  img.code(kBase + 0x20, {cmpwi(3, 0), beq(0x10), lwzx(0, 12, 11), mtctr(0), kBctr, li(3, 0), kBlr});
  img.code(kBase + 0x800, {kBase + 0x20});
  const std::vector<FunctionRange> known = {{kBase, 8, false}};

  CHECK(discover_pointer_functions(img.bytes, kBase, kCode, known).empty());
}

TEST_CASE("bl into its own interior rejects a pointer candidate", "[unit]") {
  FakeImage img;
  img.code(kBase, {li(3, 0), kBlr});  // .pdata F0
  img.code(kBase + 0x20, {cmpwi(3, 0), beq(0xC), bl(0x4), li(3, 0), kBlr});  // bl 0x2C: XenonRecomp splits there
  img.code(kBase + 0x800, {kBase + 0x20});
  const std::vector<FunctionRange> known = {{kBase, 8, false}};

  CHECK(discover_pointer_functions(img.bytes, kBase, kCode, known).empty());
}

TEST_CASE("candidate overlapping a pdata function is rejected", "[unit]") {
  FakeImage img;
  // F0 has an inner blr (0x0C) skipped by a beq: a pointer to 0x10 is inside F0.
  img.code(kBase, {cmpwi(3, 0), beq(0xC), li(3, 1), kBlr, li(3, 0), kBlr});
  // Falls through into the .pdata function F2 at 0x48 without ending.
  img.code(kBase + 0x40, {li(3, 1), li(4, 1)});
  img.code(kBase + 0x48, {li(3, 0), kBlr});
  // F1 has an exception handler: the 8 bytes before it are handler data, even if they decode as code.
  img.code(kBase + 0xF8, {li(3, 0), kBlr});
  img.code(kBase + 0x100, {li(3, 0), kBlr});
  img.code(kBase + 0x800, {kBase + 0x10, kBase + 0x40, kBase + 0xF8});
  const std::vector<FunctionRange> known = {
      {kBase, 0x18, false}, {kBase + 0x48, 8, false}, {kBase + 0x100, 8, true}};

  auto fns = discover_pointer_functions(img.bytes, kBase, kCode, known);
  CHECK(fns.empty());
}

TEST_CASE("pointer into the middle of a function is ignored", "[unit]") {
  FakeImage img;
  img.code(kBase, {li(3, 0), kBlr});  // .pdata F0
  // G (no .pdata): a pointer to its start is a function, one to its second instruction is not.
  img.code(kBase + 0x200, {li(3, 0), li(4, 0), li(5, 0), kBlr});
  // H: the word before 0x24C is a blr, but the beq at 0x244 branches into [0x24C, 0x258).
  img.code(kBase + 0x240, {cmpwi(3, 0), beq(0xC), kBlr, li(3, 1), li(4, 1), kBlr});
  img.code(kBase + 0x800, {kBase + 0x200, kBase + 0x204, kBase + 0x24C});
  const std::vector<FunctionRange> known = {{kBase, 8, false}};

  auto fns = discover_pointer_functions(img.bytes, kBase, kCode, known);
  CHECK(fns == std::vector<FunctionRange>{{kBase + 0x200, 0x10, false}});
}

TEST_CASE("lis/addi address of code yields a function", "[unit]") {
  FakeImage img;
  // F0 builds a comparator address in r6 (qsort-style) without calling it.
  img.code(kBase, {lis(11, 0x8200), addi(6, 11, 0x80), li(3, 0), kBlr});
  img.code(kBase + 0x80, {li(3, 1), kBlr});
  const std::vector<FunctionRange> known = {{kBase, 0x10, false}};

  auto fns = discover_pointer_functions(img.bytes, kBase, kCode, known);
  CHECK(fns == std::vector<FunctionRange>{{kBase + 0x80, 8, false}});
}

// An import thunk (XenonRecomp rewrites each to nop, nop, nop, blr and maps it to the kernel import) reached only
// through a pointer table, like the CRT's NtReadFile, looks like a tiny pointer-only function. Emitting it made a
// second function mapping at the thunk's address that ran the placeholder bytes instead of calling the kernel.
TEST_CASE("import thunks are never emitted as pointer-only functions", "[unit]") {
  FakeImage img;
  constexpr uint32_t kNop = 0x60000000;
  img.code(kBase, {li(3, 0), kBlr});                             // .pdata F0
  img.code(kBase + 0x10, {kNop, kNop, kNop, kBlr});              // thunk A
  img.code(kBase + 0x20, {kNop, kNop, kNop, kBlr});              // thunk B
  img.code(kBase + 0x30, {li(3, 7), b(0x10 - 0x34)});            // pointer-only function tail-calling thunk A
  img.code(kBase + 0x800, {kBase + 0x10, kBase + 0x20, kBase + 0x30});  // pointer table (data)
  const std::vector<FunctionRange> known = {{kBase, 8, false}};

  // Without the thunk list the thunks come out as functions (the bug this guards against).
  CHECK(discover_pointer_functions(img.bytes, kBase, kCode, known).size() == 3);

  const std::vector<uint32_t> thunks = {kBase + 0x10, kBase + 0x20};
  auto fns = discover_pointer_functions(img.bytes, kBase, kCode, known, thunks);
  CHECK(fns == std::vector<FunctionRange>{{kBase + 0x30, 8, false}});
}
