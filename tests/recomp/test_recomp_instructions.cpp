// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <utility>

#include "cases.h"
#include "ppc_recomp_shared.h"
#include "runtime/guest_memory.h"

namespace {

using lr::test::recomp_cases;

// Generated code addresses guest memory as base + 32-bit guest address, so the test needs the full 4 GiB
// reservation (lazily committed) instead of a std::vector.
lr::runtime::GuestMemory& guest_memory() {
  static auto mem = [] {
    auto r = lr::runtime::GuestMemory::reserve(0x1'0000'0000ull);
    REQUIRE(r.ok());
    return std::move(r.value());
  }();
  return mem;
}

PPCFunc* find_function(uint32_t address) {
  for (const PPCFuncMapping* m = PPCFuncMappings; m->host; ++m)
    if (m->guest == address) return m->host;
  return nullptr;
}

// Runs the recompiled function at `address` with `in` as the initial context and returns the final context.
PPCContext run_case(uint32_t address, PPCContext in, lr::runtime::GuestMemory& mem) {
  PPCFunc* fn = find_function(address);
  REQUIRE(fn != nullptr);
  in.fpscr.loadFromHost();  // valid FP environment, as XenonRecomp's own harness does
  fn(in, mem.base());
  return in;
}

uint32_t case_address(const char* name) {
  for (const auto& c : recomp_cases())
    if (std::strcmp(c.name, name) == 0) return c.address;
  FAIL("unknown case " << name);
  return 0;
}

}  // namespace

TEST_CASE("addi baseline", "[unit][recomp]") {
  PPCContext in{};
  in.r3.u64 = 7;
  const PPCContext out = run_case(case_address("addi baseline"), in, guest_memory());
  CHECK(out.r3.u64 == 12);
}

namespace {

// Branch fixtures return r3 = 1 on fall-through and r3 = 2 when the branch is taken.
uint64_t run_branch(const char* name, const PPCContext& in) {
  return run_case(case_address(name), in, guest_memory()).r3.u64;
}

}  // namespace

TEST_CASE("bdzf takes branch when ctr hits zero and bit clear", "[unit][recomp]") {
  PPCContext in{};
  in.ctr.u64 = 1;  // decrements to 0
  CHECK(run_branch("bdzf cr0.lt", in) == 2);
  CHECK(run_branch("bdzf cr0.gt", in) == 2);
  CHECK(run_branch("bdzf cr0.eq", in) == 2);
  CHECK(run_branch("bdzf cr0.so", in) == 2);
  CHECK(run_branch("bdzf cr2.gt", in) == 2);
}

TEST_CASE("bdzf falls through when bit set", "[unit][recomp]") {
  PPCContext in{};
  in.ctr.u64 = 1;
  PPCContext a = in;
  a.cr0.lt = 1;
  CHECK(run_branch("bdzf cr0.lt", a) == 1);
  PPCContext b = in;
  b.cr0.gt = 1;
  CHECK(run_branch("bdzf cr0.gt", b) == 1);
  PPCContext c = in;
  c.cr0.eq = 1;
  CHECK(run_branch("bdzf cr0.eq", c) == 1);
  PPCContext d = in;
  d.cr0.so = 1;
  CHECK(run_branch("bdzf cr0.so", d) == 1);
  PPCContext e = in;
  e.cr2.gt = 1;
  CHECK(run_branch("bdzf cr2.gt", e) == 1);
}

TEST_CASE("bdzf falls through when ctr is not zero after decrement", "[unit][recomp]") {
  PPCContext in{};
  in.ctr.u64 = 2;
  CHECK(run_branch("bdzf cr0.lt", in) == 1);
}

TEST_CASE("bdnzf uses the requested bit (gt)", "[unit][recomp]") {
  PPCContext in{};
  in.ctr.u64 = 2;  // stays non-zero
  CHECK(run_branch("bdnzf cr0.gt", in) == 2);
  PPCContext gt = in;
  gt.cr0.gt = 1;  // gt set, eq clear: an eq-only shortcut would wrongly branch
  CHECK(run_branch("bdnzf cr0.gt", gt) == 1);
  PPCContext eq = in;
  eq.cr0.eq = 1;  // eq set but gt clear: must still branch
  CHECK(run_branch("bdnzf cr0.gt", eq) == 2);
  CHECK(run_branch("bdnzf cr0.eq", eq) == 1);
  PPCContext zero{};
  zero.ctr.u64 = 1;  // decrements to 0: no branch
  CHECK(run_branch("bdnzf cr0.gt", zero) == 1);
}

TEST_CASE("bso branches on the summary-overflow bit", "[unit][recomp]") {
  PPCContext in{};
  CHECK(run_branch("bso cr0", in) == 1);
  PPCContext so = in;
  so.cr0.so = 1;
  CHECK(run_branch("bso cr0", so) == 2);
  CHECK(run_branch("bso cr1", so) == 1);
  PPCContext so1 = in;
  so1.cr1.so = 1;
  CHECK(run_branch("bso cr1", so1) == 2);
}

TEST_CASE("bso after fcmpu with NaN branches", "[unit][recomp]") {
  PPCContext in{};
  in.f1.f64 = std::numeric_limits<double>::quiet_NaN();
  in.f2.f64 = 1.0;
  CHECK(run_branch("fcmpu then bso", in) == 2);
}

TEST_CASE("bso after fcmpu of 1.0,2.0 falls through", "[unit][recomp]") {
  PPCContext in{};
  in.f1.f64 = 1.0;
  in.f2.f64 = 2.0;
  CHECK(run_branch("fcmpu then bso", in) == 1);
}

namespace {

uint8_t* guest_ptr(uint32_t addr) { return guest_memory().base() + addr; }

void zero_guest(uint32_t addr, size_t n) { std::memset(guest_ptr(addr), 0, n); }

}  // namespace

TEST_CASE("lfsu loads a float and updates rA", "[unit][recomp]") {
  zero_guest(0x10000, 0x20);
  const uint8_t be[4] = {0x3F, 0xC0, 0x00, 0x00};  // 1.5f
  std::memcpy(guest_ptr(0x10008), be, 4);
  PPCContext in{};
  in.r3.u64 = 0x10000;
  const PPCContext out = run_case(case_address("lfsu"), in, guest_memory());
  CHECK(out.f1.f64 == 1.5);
  CHECK(out.r3.u32 == 0x10008);
}

TEST_CASE("lfsux loads a float and updates rA", "[unit][recomp]") {
  zero_guest(0x10000, 0x20);
  const uint8_t be[4] = {0xBF, 0xC0, 0x00, 0x00};  // -1.5f
  std::memcpy(guest_ptr(0x10010), be, 4);
  PPCContext in{};
  in.r3.u64 = 0x10000;
  in.r4.u64 = 0x10;
  const PPCContext out = run_case(case_address("lfsux"), in, guest_memory());
  CHECK(out.f1.f64 == -1.5);
  CHECK(out.r3.u32 == 0x10010);
  CHECK(out.r4.u32 == 0x10);
}

TEST_CASE("stfsu stores a float and updates rA", "[unit][recomp]") {
  zero_guest(0xFF0, 0x20);
  PPCContext in{};
  in.r1.u64 = 0x1000;
  in.f1.f64 = 1.5;
  const PPCContext out = run_case(case_address("stfsu"), in, guest_memory());
  CHECK(guest_ptr(0xFF8)[0] == 0x3F);
  CHECK(guest_ptr(0xFF8)[1] == 0xC0);
  CHECK(guest_ptr(0xFF8)[2] == 0x00);
  CHECK(guest_ptr(0xFF8)[3] == 0x00);
  CHECK(guest_ptr(0xFFC)[0] == 0x00);  // nothing written past the word
  CHECK(out.r1.u32 == 0xFF8);
}

TEST_CASE("stfsux stores a float and updates rA", "[unit][recomp]") {
  zero_guest(0x10000, 0x20);
  PPCContext in{};
  in.r3.u64 = 0x10000;
  in.r4.u64 = 0x8;
  in.f1.f64 = -1.5;
  const PPCContext out = run_case(case_address("stfsux"), in, guest_memory());
  CHECK(guest_ptr(0x10008)[0] == 0xBF);
  CHECK(guest_ptr(0x10008)[1] == 0xC0);
  CHECK(guest_ptr(0x10008)[2] == 0x00);
  CHECK(guest_ptr(0x10008)[3] == 0x00);
  CHECK(out.r3.u32 == 0x10008);
  CHECK(out.r4.u32 == 0x8);
}

TEST_CASE("lhzu zero-extends a halfword and updates rA", "[unit][recomp]") {
  zero_guest(0x10000, 0x20);
  const uint8_t be[2] = {0xFF, 0x80};
  std::memcpy(guest_ptr(0x10002), be, 2);
  PPCContext in{};
  in.r5.u64 = 0x10000;
  in.r4.u64 = 0xFFFFFFFFFFFFFFFFull;
  const PPCContext out = run_case(case_address("lhzu"), in, guest_memory());
  CHECK(out.r4.u64 == 0xFF80);
  CHECK(out.r5.u32 == 0x10002);
}

TEST_CASE("sthu stores the low halfword and updates rA", "[unit][recomp]") {
  zero_guest(0x10000, 0x20);
  PPCContext in{};
  in.r5.u64 = 0x10004;
  in.r4.u64 = 0x1234ABCD;
  const PPCContext out = run_case(case_address("sthu"), in, guest_memory());
  CHECK(guest_ptr(0x10002)[0] == 0xAB);
  CHECK(guest_ptr(0x10002)[1] == 0xCD);
  CHECK(guest_ptr(0x10004)[0] == 0x00);  // only two bytes written
  CHECK(out.r5.u32 == 0x10002);
}

TEST_CASE("eqv of equal values is all ones", "[unit][recomp]") {
  PPCContext in{};
  in.r4.u64 = 0x123456789ABCDEF0ull;
  in.r5.u64 = 0x123456789ABCDEF0ull;
  const PPCContext out = run_case(case_address("eqv"), in, guest_memory());
  CHECK(out.r3.u64 == 0xFFFFFFFFFFFFFFFFull);
  CHECK(out.r4.u64 == 0x123456789ABCDEF0ull);
  CHECK(out.r5.u64 == 0x123456789ABCDEF0ull);
}

TEST_CASE("eqv is the complement of xor", "[unit][recomp]") {
  PPCContext in{};
  in.r4.u64 = 0xF0F0F0F0F0F0F0F0ull;
  in.r5.u64 = 0xFF00FF00FF00FF00ull;
  const PPCContext out = run_case(case_address("eqv"), in, guest_memory());
  CHECK(out.r3.u64 == 0xF00FF00FF00FF00Full);
}

TEST_CASE("eqv. sets cr0.lt for negative result", "[unit][recomp]") {
  PPCContext in{};
  in.r4.u64 = 0xFFFFFFFF;
  in.r5.u64 = 0xFFFFFFFE;  // ~(x ^ y) = ~1 = 0xFFFFFFFE (negative in 32 bits)
  in.xer.so = 1;
  const PPCContext out = run_case(case_address("eqv."), in, guest_memory());
  CHECK(out.r3.u32 == 0xFFFFFFFE);
  CHECK(out.cr0.lt == 1);
  CHECK(out.cr0.gt == 0);
  CHECK(out.cr0.eq == 0);
  CHECK(out.cr0.so == 1);
}

TEST_CASE("eqv. sets cr0.gt for positive and cr0.eq for zero results", "[unit][recomp]") {
  PPCContext a{};
  a.r4.u64 = 0;
  a.r5.u64 = 0x80000000;  // ~0x80000000 = 0x7FFFFFFF in the low word
  const PPCContext pos = run_case(case_address("eqv."), a, guest_memory());
  CHECK(pos.cr0.lt == 0);
  CHECK(pos.cr0.gt == 1);
  CHECK(pos.cr0.eq == 0);

  PPCContext b{};
  b.r4.u64 = 0;
  b.r5.u64 = 0xFFFFFFFF;  // ~(0 ^ 0xFFFFFFFF) = 0 in the low word
  const PPCContext zero = run_case(case_address("eqv."), b, guest_memory());
  CHECK(zero.cr0.lt == 0);
  CHECK(zero.cr0.gt == 0);
  CHECK(zero.cr0.eq == 1);
}

TEST_CASE("subfze with CA=1 on 0 gives 0 and CA=1", "[unit][recomp]") {
  PPCContext in{};
  in.r4.u64 = 0;
  in.xer.ca = 1;
  in.r3.u64 = 0x55;  // must be overwritten
  const PPCContext out = run_case(case_address("subfze"), in, guest_memory());
  CHECK(out.r3.u64 == 0);  // ~0 + 1 = 0 with carry out
  CHECK(out.xer.ca == 1);
  CHECK(out.r4.u64 == 0);
}

TEST_CASE("subfze with CA=0 on 0 gives -1 and CA=0", "[unit][recomp]") {
  PPCContext in{};
  in.r4.u64 = 0;
  in.xer.ca = 0;
  const PPCContext out = run_case(case_address("subfze"), in, guest_memory());
  CHECK(out.r3.u64 == 0xFFFFFFFFFFFFFFFFull);
  CHECK(out.xer.ca == 0);
}

TEST_CASE("subfze computes ~rA + CA and clears CA when there is no carry", "[unit][recomp]") {
  PPCContext in{};
  in.r4.u64 = 5;
  in.xer.ca = 1;
  const PPCContext out = run_case(case_address("subfze"), in, guest_memory());
  CHECK(out.r3.u32 == 0xFFFFFFFB);  // ~5 + 1 = -5
  CHECK(out.xer.ca == 0);
  CHECK(out.r4.u64 == 5);
}

TEST_CASE("subfze. sets cr0", "[unit][recomp]") {
  PPCContext neg{};
  neg.r4.u64 = 5;
  neg.xer.ca = 1;
  const PPCContext n = run_case(case_address("subfze."), neg, guest_memory());
  CHECK(n.r3.u32 == 0xFFFFFFFB);
  CHECK(n.cr0.lt == 1);
  CHECK(n.cr0.gt == 0);
  CHECK(n.cr0.eq == 0);

  PPCContext zero{};
  zero.r4.u64 = 0;
  zero.xer.ca = 1;
  const PPCContext z = run_case(case_address("subfze."), zero, guest_memory());
  CHECK(z.r3.u64 == 0);
  CHECK(z.cr0.lt == 0);
  CHECK(z.cr0.gt == 0);
  CHECK(z.cr0.eq == 1);
}

TEST_CASE("vsel128 picks vB bits where mask set", "[unit][recomp]") {
  PPCContext in{};
  // v64 is the mask (and the destination); v1 = vA, v2 = vB.
  for (int i = 0; i < 16; i++) {
    in.v1.u8[i] = 0xAA;
    in.v2.u8[i] = 0x55;
    in.v64.u8[i] = 0xF0;
    in.v3.u8[i] = 0x77;  // neighbour register, must stay unchanged
  }
  const PPCContext out = run_case(case_address("vsel128"), in, guest_memory());
  for (int i = 0; i < 16; i++) {
    CHECK(out.v64.u8[i] == 0x5A);  // (0xAA & ~0xF0) | (0x55 & 0xF0) = 0x0A | 0x50
    CHECK(out.v1.u8[i] == 0xAA);
    CHECK(out.v2.u8[i] == 0x55);
    CHECK(out.v3.u8[i] == 0x77);
  }
}

namespace {

// XenonRecomp keeps vector registers element-reversed: guest word g (0 = leftmost, x) is u32[3 - g].
uint32_t guest_word(const PPCVRegister& v, int g) { return v.u32[3 - g]; }
void set_guest_float(PPCVRegister& v, int g, float f) { v.f32[3 - g] = f; }

constexpr uint32_t kXY = 0x3C00C000;  // half(1.0) << 16 | half(-2.0)
constexpr uint32_t kZW = 0x38004200;  // half(0.5) << 16 | half(3.0)

// Packs (x, y, z, w) = (1.0, -2.0, 0.5, 3.0) from v4 into v3, whose guest words start as 0x11111111..0x44444444,
// and returns v3's guest words.
std::array<uint32_t, 4> run_f16x4(const char* name) {
  PPCContext in{};
  set_guest_float(in.v4, 0, 1.0f);
  set_guest_float(in.v4, 1, -2.0f);
  set_guest_float(in.v4, 2, 0.5f);
  set_guest_float(in.v4, 3, 3.0f);
  for (int g = 0; g < 4; g++) in.v3.u32[3 - g] = 0x11111111u * uint32_t(g + 1);
  const PPCContext out = run_case(case_address(name), in, guest_memory());
  for (int g = 0; g < 4; g++) CHECK(guest_word(out.v4, g) == guest_word(in.v4, g));  // source untouched
  return {guest_word(out.v3, 0), guest_word(out.v3, 1), guest_word(out.v3, 2), guest_word(out.v3, 3)};
}

using Words = std::array<uint32_t, 4>;

}  // namespace

// Expected layouts follow Xenia's InstrEmit_vpkd3d128 (permute of the old vD with the packed [0, 0, XY, ZW]).
TEST_CASE("vpkd3d128 float16_4 pack 2 writes XY,ZW at words 2-shift", "[unit][recomp]") {
  CHECK(run_f16x4("f16x4 p2 s0") == Words{0x11111111, 0x22222222, kXY, kZW});
  CHECK(run_f16x4("f16x4 p2 s1") == Words{0x11111111, kXY, kZW, 0x44444444});
  CHECK(run_f16x4("f16x4 p2 s2") == Words{kXY, kZW, 0x33333333, 0x44444444});
}

TEST_CASE("vpkd3d128 float16_4 pack 2 shift 3 writes ZW into x", "[unit][recomp]") {
  CHECK(run_f16x4("f16x4 p2 s3") == Words{kZW, 0x22222222, 0x33333333, 0x44444444});
}

TEST_CASE("vpkd3d128 float16_4 pack 3", "[unit][recomp]") {
  CHECK(run_f16x4("f16x4 p3 s0") == Words{0x11111111, 0x22222222, kXY, kZW});
  CHECK(run_f16x4("f16x4 p3 s3") == Words{0x11111111, 0x22222222, 0x33333333, kXY});  // XY into w
}

TEST_CASE("vpkd3d128 float16_4 pack 1 writes ZW into word 3-shift", "[unit][recomp]") {
  CHECK(run_f16x4("f16x4 p1 s0") == Words{0x11111111, 0x22222222, 0x33333333, kZW});
  CHECK(run_f16x4("f16x4 p1 s1") == Words{0x11111111, 0x22222222, kZW, 0x44444444});
  CHECK(run_f16x4("f16x4 p1 s2") == Words{0x11111111, kZW, 0x33333333, 0x44444444});
  CHECK(run_f16x4("f16x4 p1 s3") == Words{kZW, 0x22222222, 0x33333333, 0x44444444});
}

TEST_CASE("vpkd3d128 float16_4 reads every source float before writing when vD is vB", "[unit][recomp]") {
  PPCContext in{};
  set_guest_float(in.v3, 0, 1.0f);
  set_guest_float(in.v3, 1, -2.0f);
  set_guest_float(in.v3, 2, 0.5f);
  set_guest_float(in.v3, 3, 3.0f);
  const PPCContext out = run_case(case_address("f16x4 p2 s2 same reg"), in, guest_memory());
  CHECK(guest_word(out.v3, 0) == kXY);
  CHECK(guest_word(out.v3, 1) == kZW);
  CHECK(guest_word(out.v3, 2) == 0x3F000000u);  // 0.5f, untouched
  CHECK(guest_word(out.v3, 3) == 0x40400000u);  // 3.0f, untouched
}

// ---- Integer carry and multiply ----

TEST_CASE("addc adds and sets CA from the 32-bit carry", "[unit][recomp]") {
  PPCContext in{};
  in.r4.u64 = 0xFFFFFFFF;
  in.r5.u64 = 1;
  PPCContext out = run_case(case_address("addc"), in, guest_memory());
  CHECK(out.r3.u32 == 0);
  CHECK(out.xer.ca == 1);
  in.r4.u64 = 1;
  in.r5.u64 = 2;
  in.xer.ca = 1;  // ignored as input, cleared as output
  out = run_case(case_address("addc"), in, guest_memory());
  CHECK(out.r3.u64 == 3);
  CHECK(out.xer.ca == 0);
}

TEST_CASE("addc. sets cr0", "[unit][recomp]") {
  PPCContext in{};
  in.r4.u64 = 0x7FFFFFFF;
  in.r5.u64 = 1;
  const PPCContext out = run_case(case_address("addc."), in, guest_memory());
  CHECK(out.r3.u32 == 0x80000000);
  CHECK(out.xer.ca == 0);
  CHECK(out.cr0.lt == 1);
  CHECK(out.cr0.eq == 0);
}

TEST_CASE("addme computes rA + CA - 1 with the 32-bit carry out", "[unit][recomp]") {
  PPCContext in{};
  in.r4.u64 = 5;
  in.xer.ca = 1;
  PPCContext out = run_case(case_address("addme"), in, guest_memory());
  CHECK(out.r3.u32 == 5);
  CHECK(out.xer.ca == 1);
  in.xer.ca = 0;
  out = run_case(case_address("addme"), in, guest_memory());
  CHECK(out.r3.u32 == 4);
  CHECK(out.xer.ca == 1);
  in.r4.u64 = 0;
  out = run_case(case_address("addme"), in, guest_memory());
  CHECK(out.r3.u32 == 0xFFFFFFFF);
  CHECK(out.xer.ca == 0);
}

TEST_CASE("addme. sets cr0", "[unit][recomp]") {
  PPCContext in{};
  in.r4.u64 = 0;
  in.xer.ca = 1;
  const PPCContext out = run_case(case_address("addme."), in, guest_memory());
  CHECK(out.r3.u32 == 0);
  CHECK(out.xer.ca == 1);
  CHECK(out.cr0.eq == 1);
  CHECK(out.cr0.lt == 0);
}

TEST_CASE("mulhd returns the signed high doubleword", "[unit][recomp]") {
  PPCContext in{};
  in.r4.u64 = 0x100000000ull;
  in.r5.u64 = 0x300000000ull;
  PPCContext out = run_case(case_address("mulhd"), in, guest_memory());
  CHECK(out.r3.u64 == 3);
  in.r4.s64 = -1;
  in.r5.s64 = 5;
  out = run_case(case_address("mulhd"), in, guest_memory());
  CHECK(out.r3.s64 == -1);
  in.r4.u64 = 0x8000000000000000ull;  // INT64_MIN * 2 = -2^64 -> high = -1
  in.r5.u64 = 2;
  out = run_case(case_address("mulhd"), in, guest_memory());
  CHECK(out.r3.s64 == -1);
}

// ---- Loads and stores with update, byte-reversed and element loads ----

namespace {

void put_bytes(uint32_t addr, std::initializer_list<uint8_t> bytes) {
  uint8_t* p = guest_ptr(addr);
  for (uint8_t b : bytes) *p++ = b;
}

PPCContext indexed(uint64_t ra, uint64_t rb) {
  PPCContext in{};
  in.r3.u64 = ra;
  in.r5.u64 = rb;
  in.r4.u64 = 0xFFFFFFFFFFFFFFFFull;
  return in;
}

}  // namespace

TEST_CASE("lhzux/lbzux/lwzux/ldux load and update rA", "[unit][recomp]") {
  zero_guest(0x10000, 0x40);
  put_bytes(0x10010, {0x12, 0x34});
  PPCContext out = run_case(case_address("lhzux"), indexed(0x10000, 0x10), guest_memory());
  CHECK(out.r4.u64 == 0x1234);
  CHECK(out.r3.u32 == 0x10010);
  put_bytes(0x10008, {0xAB});
  out = run_case(case_address("lbzux"), indexed(0x10000, 8), guest_memory());
  CHECK(out.r4.u64 == 0xAB);
  CHECK(out.r3.u32 == 0x10008);
  put_bytes(0x10004, {0xDE, 0xAD, 0xBE, 0xEF});
  out = run_case(case_address("lwzux"), indexed(0x10000, 4), guest_memory());
  CHECK(out.r4.u64 == 0xDEADBEEF);
  CHECK(out.r3.u32 == 0x10004);
  put_bytes(0x10020, {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF});
  out = run_case(case_address("ldux"), indexed(0x10000, 0x20), guest_memory());
  CHECK(out.r4.u64 == 0x0123456789ABCDEFull);
  CHECK(out.r3.u32 == 0x10020);
  CHECK(out.r5.u64 == 0x20);
}

TEST_CASE("lfdux and lfdu load a double and update rA", "[unit][recomp]") {
  zero_guest(0x10000, 0x40);
  put_bytes(0x10018, {0x3F, 0xF8, 0, 0, 0, 0, 0, 0});  // 1.5
  PPCContext out = run_case(case_address("lfdux"), indexed(0x10000, 0x18), guest_memory());
  CHECK(out.f4.f64 == 1.5);
  CHECK(out.r3.u32 == 0x10018);
  put_bytes(0x10008, {0xC0, 0x00, 0, 0, 0, 0, 0, 0});  // -2.0
  PPCContext in{};
  in.r3.u64 = 0x10000;
  out = run_case(case_address("lfdu"), in, guest_memory());
  CHECK(out.f1.f64 == -2.0);
  CHECK(out.r3.u32 == 0x10008);
}

TEST_CASE("stfdu stores a double and updates rA", "[unit][recomp]") {
  zero_guest(0xFF0, 0x20);
  PPCContext in{};
  in.r1.u64 = 0x1000;
  in.f1.f64 = 1.5;
  const PPCContext out = run_case(case_address("stfdu"), in, guest_memory());
  const uint8_t expected[8] = {0x3F, 0xF8, 0, 0, 0, 0, 0, 0};
  CHECK(std::memcmp(guest_ptr(0xFF8), expected, 8) == 0);
  CHECK(out.r1.u32 == 0xFF8);
}

TEST_CASE("lhau sign-extends and updates rA", "[unit][recomp]") {
  zero_guest(0x10000, 0x20);
  put_bytes(0x10002, {0xFF, 0x80});
  PPCContext in{};
  in.r5.u64 = 0x10000;
  const PPCContext out = run_case(case_address("lhau"), in, guest_memory());
  CHECK(out.r4.s64 == -128);
  CHECK(out.r5.u32 == 0x10002);
}

TEST_CASE("sthux/stbux/stdux store and update rA", "[unit][recomp]") {
  zero_guest(0x10000, 0x40);
  PPCContext in = indexed(0x10000, 0x10);
  in.r4.u64 = 0x0123456789ABCDEFull;
  PPCContext out = run_case(case_address("sthux"), in, guest_memory());
  CHECK(guest_ptr(0x10010)[0] == 0xCD);
  CHECK(guest_ptr(0x10010)[1] == 0xEF);
  CHECK(guest_ptr(0x10012)[0] == 0x00);
  CHECK(out.r3.u32 == 0x10010);
  in.r5.u64 = 0x18;
  out = run_case(case_address("stbux"), in, guest_memory());
  CHECK(guest_ptr(0x10018)[0] == 0xEF);
  CHECK(guest_ptr(0x10019)[0] == 0x00);
  CHECK(out.r3.u32 == 0x10018);
  in.r5.u64 = 0x20;
  out = run_case(case_address("stdux"), in, guest_memory());
  const uint8_t expected[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
  CHECK(std::memcmp(guest_ptr(0x10020), expected, 8) == 0);
  CHECK(out.r3.u32 == 0x10020);
}

TEST_CASE("lhbrx loads a byte-reversed halfword without update", "[unit][recomp]") {
  zero_guest(0x10000, 0x20);
  put_bytes(0x10006, {0x12, 0x34});
  const PPCContext out = run_case(case_address("lhbrx"), indexed(0x10000, 6), guest_memory());
  CHECK(out.r4.u64 == 0x3412);
  CHECK(out.r3.u32 == 0x10000);
}

TEST_CASE("lvehx puts the addressed halfword in its element", "[unit][recomp]") {
  for (uint32_t i = 0; i < 16; i++) guest_ptr(0x10000)[i] = uint8_t(i);
  const PPCContext out = run_case(case_address("lvehx"), indexed(0x10000, 6), guest_memory());
  CHECK(out.v4.u16[7 - 3] == 0x0607);  // guest halfword 3 = bytes 6, 7
}

// ---- Branches ----

TEST_CASE("bns branches when summary overflow is clear", "[unit][recomp]") {
  PPCContext in{};
  CHECK(run_branch("bns cr0", in) == 2);
  PPCContext so = in;
  so.cr0.so = 1;
  CHECK(run_branch("bns cr0", so) == 1);
  CHECK(run_branch("bns cr1", so) == 2);
  PPCContext so1 = in;
  so1.cr1.so = 1;
  CHECK(run_branch("bns cr1", so1) == 1);
}

TEST_CASE("bdnzt branches while ctr is non-zero and the bit is set", "[unit][recomp]") {
  PPCContext in{};
  in.ctr.u64 = 2;
  in.cr0.gt = 1;
  CHECK(run_branch("bdnzt cr0.gt", in) == 2);
  PPCContext clear = in;
  clear.cr0.gt = 0;
  clear.cr0.eq = 1;
  CHECK(run_branch("bdnzt cr0.gt", clear) == 1);
  PPCContext last = in;
  last.ctr.u64 = 1;  // decrements to 0
  CHECK(run_branch("bdnzt cr0.gt", last) == 1);
}

// ---- VMX integer ----
// Element-wise checks use host lanes directly: XenonRecomp reverses the whole register, so host lane i is guest
// lane n-1-i, which does not matter for lane-by-lane operations.

namespace {

PPCContext vrun(const char* name, const PPCVRegister& a, const PPCVRegister& b) {
  PPCContext in{};
  in.v4 = a;
  in.v5 = b;
  for (int i = 0; i < 16; i++) in.v3.u8[i] = 0x5A;
  return run_case(case_address(name), in, guest_memory());
}

PPCVRegister u16x8(std::array<uint16_t, 8> lanes) {
  PPCVRegister v{};
  for (int i = 0; i < 8; i++) v.u16[i] = lanes[size_t(i)];
  return v;
}

PPCVRegister u32x4(std::array<uint32_t, 4> lanes) {
  PPCVRegister v{};
  for (int i = 0; i < 4; i++) v.u32[i] = lanes[size_t(i)];
  return v;
}

std::array<uint16_t, 8> lanes16(const PPCVRegister& v) {
  std::array<uint16_t, 8> r{};
  for (int i = 0; i < 8; i++) r[size_t(i)] = v.u16[i];
  return r;
}

std::array<uint32_t, 4> lanes32(const PPCVRegister& v) { return {v.u32[0], v.u32[1], v.u32[2], v.u32[3]}; }

using H8 = std::array<uint16_t, 8>;
using W4 = std::array<uint32_t, 4>;

}  // namespace

TEST_CASE("vmaxuh and vminuh compare unsigned halfwords", "[unit][recomp]") {
  const auto a = u16x8({0x8000, 1, 0xFFFF, 0, 7, 7, 0x7FFF, 0x8001});
  const auto b = u16x8({1, 0x8000, 0, 0xFFFF, 7, 8, 0x8000, 0x8000});
  CHECK(lanes16(vrun("vmaxuh", a, b).v3) == H8{0x8000, 0x8000, 0xFFFF, 0xFFFF, 7, 8, 0x8000, 0x8001});
  CHECK(lanes16(vrun("vminuh", a, b).v3) == H8{1, 1, 0, 0, 7, 7, 0x7FFF, 0x8000});
}

TEST_CASE("vadduhs saturates unsigned halfwords", "[unit][recomp]") {
  const auto a = u16x8({0xFFF0, 1, 0x8000, 0xFFFF, 0, 2, 3, 4});
  const auto b = u16x8({0x0020, 2, 0x8000, 1, 0, 2, 3, 4});
  CHECK(lanes16(vrun("vadduhs", a, b).v3) == H8{0xFFFF, 3, 0xFFFF, 0xFFFF, 0, 4, 6, 8});
}

TEST_CASE("vsubuws saturates unsigned words at zero", "[unit][recomp]") {
  const auto a = u32x4({5, 7, 0x80000000, 0});
  const auto b = u32x4({7, 5, 1, 0xFFFFFFFF});
  CHECK(lanes32(vrun("vsubuws", a, b).v3) == W4{0, 2, 0x7FFFFFFF, 0});
}

TEST_CASE("vavguw rounds up without overflowing", "[unit][recomp]") {
  const auto a = u32x4({0xFFFFFFFF, 1, 0, 0x80000000});
  const auto b = u32x4({0xFFFFFFFF, 2, 0, 0x80000001});
  CHECK(lanes32(vrun("vavguw", a, b).v3) == W4{0xFFFFFFFF, 2, 0, 0x80000001});
}

TEST_CASE("vslh shifts each halfword by the low 4 bits of vB", "[unit][recomp]") {
  const auto a = u16x8({0x8001, 0x8001, 1, 1, 0xFFFF, 0x1234, 3, 0});
  const auto b = u16x8({1, 0x11, 15, 16, 4, 0, 0xFFF2, 9});
  CHECK(lanes16(vrun("vslh", a, b).v3) == H8{0x0002, 0x0002, 0x8000, 1, 0xFFF0, 0x1234, 12, 0});
}

TEST_CASE("vrlw128 rotates each word by the low 5 bits of vB", "[unit][recomp]") {
  const auto a = u32x4({0x80000001, 0x80000001, 0x12345678, 0xF0000000});
  const auto b = u32x4({1, 33, 0, 4});
  CHECK(lanes32(vrun("vrlw128", a, b).v3) == W4{3, 3, 0x12345678, 0x0000000F});
}

TEST_CASE("vnor128 is the complement of or", "[unit][recomp]") {
  const auto a = u32x4({0xF0F0F0F0, 0, 0xFFFFFFFF, 0x12345678});
  const auto b = u32x4({0x0F000000, 0, 0, 0x12345678});
  CHECK(lanes32(vrun("vnor128", a, b).v3) == W4{0x000F0F0F, 0xFFFFFFFF, 0, 0xEDCBA987});
}

TEST_CASE("vnor128 with vA == vB is the complement (vnot), not zero", "[unit][recomp]") {
  // vrun puts the first vector in v4, which is both sources here; the second one (v5) must be ignored.
  const auto a = u32x4({0xF0F0F0F0, 0, 0xFFFFFFFF, 0x12345678});
  const PPCContext out = vrun("vnor128 same reg", a, u32x4({0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF}));
  CHECK(lanes32(out.v3) == W4{0x0F0F0F0F, 0xFFFFFFFF, 0, 0xEDCBA987});
  CHECK(lanes32(out.v4) == lanes32(a));
}

TEST_CASE("vcmpequh sets lanes to all ones where equal", "[unit][recomp]") {
  const auto a = u16x8({1, 2, 3, 4, 5, 6, 7, 8});
  const auto b = u16x8({1, 0, 3, 0, 5, 0, 7, 0});
  CHECK(lanes16(vrun("vcmpequh", a, b).v3) == H8{0xFFFF, 0, 0xFFFF, 0, 0xFFFF, 0, 0xFFFF, 0});
}

TEST_CASE("vcmpequh. sets cr6 all/none", "[unit][recomp]") {
  const auto a = u16x8({1, 2, 3, 4, 5, 6, 7, 8});
  const PPCContext all = vrun("vcmpequh.", a, a);
  CHECK(all.cr6.lt == 1);
  CHECK(all.cr6.eq == 0);
  const PPCContext none = vrun("vcmpequh.", a, u16x8({0, 0, 0, 0, 0, 0, 0, 0}));
  CHECK(none.cr6.lt == 0);
  CHECK(none.cr6.eq == 1);
  const PPCContext some = vrun("vcmpequh.", a, u16x8({1, 0, 0, 0, 0, 0, 0, 0}));
  CHECK(some.cr6.lt == 0);
  CHECK(some.cr6.eq == 0);
}

TEST_CASE("vcmpgtuw compares unsigned words", "[unit][recomp]") {
  const auto a = u32x4({0x80000000, 1, 5, 0xFFFFFFFF});
  const auto b = u32x4({1, 0x80000000, 5, 0xFFFFFFFE});
  CHECK(lanes32(vrun("vcmpgtuw", a, b).v3) == W4{0xFFFFFFFF, 0, 0, 0xFFFFFFFF});
  const PPCContext all = vrun("vcmpgtuw.", u32x4({2, 2, 2, 2}), u32x4({1, 1, 1, 1}));
  CHECK(all.cr6.lt == 1);
  CHECK(all.cr6.eq == 0);
  const PPCContext none = vrun("vcmpgtuw.", u32x4({1, 1, 1, 1}), u32x4({2, 2, 2, 2}));
  CHECK(none.cr6.lt == 0);
  CHECK(none.cr6.eq == 1);
}

// ---- VMX whole-register shifts ----

namespace {

// Guest byte k of a vector register (0 = most significant, at the lowest address).
uint8_t gbyte(const PPCVRegister& v, int k) { return v.u8[15 - k]; }
void set_gbyte(PPCVRegister& v, int k, uint8_t b) { v.u8[15 - k] = b; }

PPCVRegister guest_bytes_ramp() {
  PPCVRegister v{};
  for (int k = 0; k < 16; k++) set_gbyte(v, k, uint8_t((0x11 * (k + 1)) ^ 0xA5));
  return v;
}

PPCVRegister splat_byte(uint8_t b) {
  PPCVRegister v{};
  for (int k = 0; k < 16; k++) v.u8[k] = b;
  return v;
}

}  // namespace

TEST_CASE("vsl shifts the whole register left by 0-7 bits", "[unit][recomp]") {
  const auto a = guest_bytes_ramp();
  for (int sh : {0, 1, 4, 7}) {
    const PPCContext out = vrun("vsl", a, splat_byte(uint8_t(sh)));
    for (int k = 0; k < 16; k++) {
      const int next = k < 15 ? gbyte(a, k + 1) : 0;
      const uint8_t expected = uint8_t((gbyte(a, k) << sh) | (next >> (8 - sh)));
      CHECK(gbyte(out.v3, k) == expected);
    }
  }
}

TEST_CASE("vslo128 shifts the whole register left by octets", "[unit][recomp]") {
  const auto a = guest_bytes_ramp();
  for (int octets : {0, 1, 3, 15}) {
    PPCVRegister b{};
    set_gbyte(b, 15, uint8_t((octets << 3) | 0x87));  // only bits 6:3 of guest byte 15 count
    set_gbyte(b, 0, 0xFF);
    const PPCContext out = vrun("vslo128", a, b);
    for (int k = 0; k < 16; k++) CHECK(gbyte(out.v3, k) == (k + octets < 16 ? gbyte(a, k + octets) : 0));
  }
}

// ---- VMX128 packs (guest element e is host lane n-1-e; vA fills the high half of vD) ----

TEST_CASE("vpkswss128 packs signed words with saturation, vA first", "[unit][recomp]") {
  PPCVRegister a{}, b{};
  const int32_t av[4] = {70000, -70000, 5, -5}, bv[4] = {1, 2, 3, 4};
  for (int e = 0; e < 4; e++) {
    a.s32[3 - e] = av[e];
    b.s32[3 - e] = bv[e];
  }
  const PPCContext out = vrun("vpkswss128", a, b);
  const int16_t expected[8] = {32767, -32768, 5, -5, 1, 2, 3, 4};
  for (int e = 0; e < 8; e++) CHECK(out.v3.s16[7 - e] == expected[e]);
}

TEST_CASE("vpkuwum128 and vpkuwus128 pack unsigned words", "[unit][recomp]") {
  PPCVRegister a{}, b{};
  const uint32_t av[4] = {0x12345678, 0x10000, 0xFFFFFFFF, 7}, bv[4] = {0xABCD0001, 2, 0xFFFF, 0x80000000};
  for (int e = 0; e < 4; e++) {
    a.u32[3 - e] = av[e];
    b.u32[3 - e] = bv[e];
  }
  const uint16_t modulo[8] = {0x5678, 0, 0xFFFF, 7, 0x0001, 2, 0xFFFF, 0};
  const uint16_t saturated[8] = {0xFFFF, 0xFFFF, 0xFFFF, 7, 0xFFFF, 2, 0xFFFF, 0xFFFF};
  const PPCContext um = vrun("vpkuwum128", a, b);
  const PPCContext us = vrun("vpkuwus128", a, b);
  for (int e = 0; e < 8; e++) {
    CHECK(um.v3.u16[7 - e] == modulo[e]);
    CHECK(us.v3.u16[7 - e] == saturated[e]);
  }
}

TEST_CASE("vpkuhum128 and vpkuhus128 pack unsigned halfwords", "[unit][recomp]") {
  PPCVRegister a{}, b{};
  const uint16_t av[8] = {0x1234, 0x00FF, 0x0100, 0, 0xFFFF, 1, 0x80, 0x8000};
  const uint16_t bv[8] = {2, 3, 0x0FFF, 4, 5, 6, 0x00FE, 0x0101};
  for (int e = 0; e < 8; e++) {
    a.u16[7 - e] = av[e];
    b.u16[7 - e] = bv[e];
  }
  const PPCContext um = vrun("vpkuhum128", a, b);
  const PPCContext us = vrun("vpkuhus128", a, b);
  for (int e = 0; e < 8; e++) {
    CHECK(um.v3.u8[15 - e] == uint8_t(av[e]));
    CHECK(um.v3.u8[15 - (8 + e)] == uint8_t(bv[e]));
    CHECK(us.v3.u8[15 - e] == (av[e] > 0xFF ? 0xFF : av[e]));
    CHECK(us.v3.u8[15 - (8 + e)] == (bv[e] > 0xFF ? 0xFF : bv[e]));
  }
}

// ---- Float to unsigned word ----

namespace {

PPCVRegister f32x4(float a, float b, float c, float d) {
  PPCVRegister v{};
  v.f32[0] = a;
  v.f32[1] = b;
  v.f32[2] = c;
  v.f32[3] = d;
  return v;
}

}  // namespace

TEST_CASE("vctuxs converts to unsigned words with saturation", "[unit][recomp]") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  CHECK(lanes32(vrun("vctuxs 0", PPCVRegister{}, f32x4(1.5f, -1.0f, nan, 5e9f)).v3) == W4{1, 0, 0, 0xFFFFFFFF});
  const auto scaled = f32x4(1.5f, 0.25f, 1073741824.0f, 3.0f);  // * 4
  CHECK(lanes32(vrun("vctuxs 2", PPCVRegister{}, scaled).v3) == W4{6, 1, 0xFFFFFFFF, 12});
}

TEST_CASE("vcfpuxws128 converts like vctuxs", "[unit][recomp]") {
  const auto scaled = f32x4(1.5f, -0.25f, 1073741824.0f, 3.0f);
  CHECK(lanes32(vrun("vcfpuxws128 2", PPCVRegister{}, scaled).v3) == W4{6, 0, 0xFFFFFFFF, 12});
}

// ---- Cache-block ops (the memory-fill hypothesis of Task 22) ----

namespace {

// Fills guest [addr, addr + n) with 0xAB, runs `name` with r3/r4 set, and returns which bytes became zero as
// [first, last) relative to addr (first == last when none did; the zeroed bytes must be contiguous).
std::pair<uint32_t, uint32_t> zeroed_span(const char* name, uint32_t addr, uint32_t n, uint64_t r3, uint64_t r4) {
  std::memset(guest_ptr(addr), 0xAB, n);
  PPCContext in{};
  in.r3.u64 = r3;
  in.r4.u64 = r4;
  run_case(case_address(name), in, guest_memory());
  uint32_t first = n, last = 0;
  for (uint32_t i = 0; i < n; ++i)
    if (guest_ptr(addr)[i] == 0) {
      first = std::min(first, i);
      last = i + 1;
    }
  if (first == n) return {0, 0};
  for (uint32_t i = first; i < last; ++i) REQUIRE(guest_ptr(addr)[i] == 0);
  return {first, last};
}

}  // namespace

TEST_CASE("dcbz zeroes the 32-byte block containing rA + rB", "[unit][recomp]") {
  // Window 0x20000..0x20200; EA = 0x20000 + 0x47 lies in the block 0x20040..0x20060.
  CHECK(zeroed_span("dcbz", 0x20000, 0x200, 0x20000, 0x47) == std::pair<uint32_t, uint32_t>{0x40, 0x60});
  CHECK(zeroed_span("dcbz rA=0", 0x20000, 0x200, 0xFFFF0000, 0x20047) ==
        std::pair<uint32_t, uint32_t>{0x40, 0x60});
}

TEST_CASE("dcbz128 zeroes the 128-byte line containing rA + rB", "[unit][recomp]") {
  // EA = 0x20000 + 0x1C7 lies in the line 0x20180..0x20200.
  CHECK(zeroed_span("dcbz128", 0x20000, 0x300, 0x20000, 0x1C7) == std::pair<uint32_t, uint32_t>{0x180, 0x200});
  CHECK(zeroed_span("dcbz128", 0x20000, 0x300, 0x20100, 0x80) == std::pair<uint32_t, uint32_t>{0x180, 0x200});
  CHECK(zeroed_span("dcbz128 rA=0", 0x20000, 0x300, 0xFFFF0000, 0x20080) ==
        std::pair<uint32_t, uint32_t>{0x80, 0x100});
}

TEST_CASE("dcbt, dcbtst and dcbf change no memory and no register", "[unit][recomp]") {
  std::memset(guest_ptr(0x20000), 0xAB, 0x100);
  PPCContext in{};
  in.r3.u64 = 0x20000;
  in.r4.u64 = 0x40;
  const PPCContext out = run_case(case_address("cache hints"), in, guest_memory());
  for (uint32_t i = 0; i < 0x100; ++i) REQUIRE(guest_ptr(0x20000)[i] == 0xAB);
  CHECK(out.r3.u64 == 0x20000);
  CHECK(out.r4.u64 == 0x40);
}

TEST_CASE("a dcbz128 line copy leaves the copied bytes and its neighbours intact", "[unit][recomp]") {
  std::memset(guest_ptr(0x30000), 0xCD, 0x180);
  for (uint32_t i = 0; i < 0x80; ++i) guest_ptr(0x31000)[i] = uint8_t(i + 1);  // source, no zero byte
  PPCContext in{};
  in.r3.u64 = 0x30080;
  in.r4.u64 = 0x31000;
  run_case(case_address("dcbz128 then line store"), in, guest_memory());
  for (uint32_t i = 0; i < 0x80; ++i) {
    REQUIRE(guest_ptr(0x30000)[i] == 0xCD);  // line before
    REQUIRE(guest_ptr(0x30080)[i] == uint8_t(i + 1));
    REQUIRE(guest_ptr(0x30100)[i] == 0xCD);  // line after
  }
}
