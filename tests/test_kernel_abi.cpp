// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <vector>

#include "runtime/kernel/abi.h"
#include "runtime/kernel/guest_structs.h"
#include "runtime/kernel/marshal.h"
#include "runtime/kernel/status.h"

using namespace lr::kernel;

namespace {

uint64_t sum9(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g, uint32_t h,
              uint32_t i) {
  return uint64_t{a} + b + c + d + e + f + g + h + i;
}
uint32_t status_fn() { return STATUS_OBJECT_NAME_NOT_FOUND; }
int32_t neg_fn() { return -2; }
uint64_t wide(uint64_t a, int64_t b, uint64_t stack64) { return a + static_cast<uint64_t>(b) + stack64; }
uint64_t wide9(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint64_t s) {
  return s;
}
int mixed(int a, double b, int c) {
  (void)a;
  (void)b;
  return c;
}
double dsum(double a, double b) { return a + b; }
float fret(float a) { return a * 2.0f; }
double second_double(int, double, float x) { return x; }
uint32_t ptr_fn(GuestPtr<uint32_t> p) { return p.addr; }
uint32_t with_call(GuestCall& c, uint32_t a, uint32_t b) { return a + b + c.lr; }
uint32_t noexcept_fn(uint32_t a) noexcept { return a + 1; }
enum class Color : uint32_t { Red = 1, Blue = 0xC0000034u };
Color enum_fn(Color c) { return c == Color::Red ? Color::Blue : Color::Red; }
uint32_t ten(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
             uint32_t j) {
  return j;
}
void void_fn(uint32_t a) { (void)a; }
int small_args(uint8_t a, int16_t b) { return a + b; }

std::vector<uint8_t> mem(0x1000);

GuestCall make_call() {
  GuestCall c;
  c.base = mem.data();
  c.r1 = 0x800;
  return c;
}
void put_be32(uint32_t addr, uint32_t v) { *reinterpret_cast<lr::runtime::be<uint32_t>*>(mem.data() + addr) = v; }
void put_be64(uint32_t addr, uint64_t v) { *reinterpret_cast<lr::runtime::be<uint64_t>*>(mem.data() + addr) = v; }

}  // namespace

TEST_CASE("marshal reads nine integer args (ninth from stack)", "[unit][kernel_abi]") {
  GuestCall c = make_call();
  for (int i = 0; i < 8; ++i) c.gpr[i] = static_cast<uint64_t>(i + 1);
  put_be32(c.r1 + 0x54, 100);
  marshal<sum9>(c);
  CHECK(c.ret_gpr == 36 + 100);
  CHECK_FALSE(c.has_fpr_return);
}

TEST_CASE("marshal sign-extends 32-bit returns", "[unit][kernel_abi]") {
  GuestCall c = make_call();
  marshal<status_fn>(c);
  CHECK(c.ret_gpr == 0xFFFFFFFFC0000034ull);
  marshal<neg_fn>(c);
  CHECK(c.ret_gpr == 0xFFFFFFFFFFFFFFFEull);
}

TEST_CASE("marshal keeps 64-bit args", "[unit][kernel_abi]") {
  GuestCall c = make_call();
  c.gpr[0] = 0x1'0000'0000ull;
  c.gpr[1] = static_cast<uint64_t>(-1);
  c.gpr[2] = 5;
  marshal<wide>(c);
  CHECK(c.ret_gpr == 0x1'0000'0000ull - 1 + 5);

  GuestCall d = make_call();
  put_be64(d.r1 + 0x50, 0x1122334455667788ull);
  marshal<wide9>(d);
  CHECK(d.ret_gpr == 0x1122334455667788ull);
}

TEST_CASE("marshal truncates narrow args", "[unit][kernel_abi]") {
  GuestCall c = make_call();
  c.gpr[0] = 0xFFFFFF01;
  c.gpr[1] = 0xFFFFFFFF;
  marshal<small_args>(c);
  CHECK(c.ret_gpr == 0);  // 1 + (-1)
}

TEST_CASE("GuestPtr zero is null", "[unit][kernel_abi]") {
  GuestPtr<uint32_t> p{0};
  CHECK(p.get(mem.data()) == nullptr);
  CHECK_FALSE(static_cast<bool>(p));
  GuestPtr<uint32_t> q{0x100};
  CHECK(static_cast<bool>(q));
  CHECK(q.get(mem.data()) == reinterpret_cast<uint32_t*>(mem.data() + 0x100));
  GuestCall c = make_call();
  c.gpr[0] = 0x1234;
  marshal<ptr_fn>(c);
  CHECK(c.ret_gpr == 0x1234);
}

TEST_CASE("GuestPtr rejects ranges past 4 GiB", "[unit][kernel_abi]") {
  CHECK_FALSE(GuestPtr<uint8_t>{0xFFFF}.valid_range(1));
  CHECK_FALSE(GuestPtr<uint8_t>{0}.valid_range(1));
  CHECK(GuestPtr<uint8_t>{0x10000}.valid_range(1));
  CHECK(GuestPtr<uint8_t>{0xFFFFFFF0u}.valid_range(0x10));
  CHECK_FALSE(GuestPtr<uint8_t>{0xFFFFFFF0u}.valid_range(0x11));
  CHECK_FALSE(GuestPtr<uint8_t>{0x20000}.valid_range(size_t{1} << 32));
}

TEST_CASE("float arg consumes a GPR position", "[unit][kernel_abi]") {
  GuestCall c = make_call();
  c.gpr[0] = 1;
  c.gpr[1] = 99;
  c.gpr[2] = 7;
  c.fpr[0] = 1.5;
  marshal<mixed>(c);
  CHECK(c.ret_gpr == 7);
}

TEST_CASE("float args read consecutive FPRs", "[unit][kernel_abi]") {
  GuestCall c = make_call();
  c.fpr[0] = 1.25;
  c.fpr[1] = 2.5;
  marshal<dsum>(c);
  CHECK(c.ret_fpr == 3.75);

  GuestCall d = make_call();
  d.fpr[0] = 9.0;
  d.fpr[1] = 4.5;  // x is the second FP arg -> f2
  marshal<second_double>(d);
  CHECK(d.ret_fpr == 4.5);
}

TEST_CASE("float return sets f1", "[unit][kernel_abi]") {
  GuestCall c = make_call();
  c.fpr[0] = 2.5;
  marshal<fret>(c);
  CHECK(c.has_fpr_return);
  CHECK(c.ret_fpr == 5.0);
}

TEST_CASE("marshal passes an optional leading GuestCall without consuming a position", "[unit][kernel_abi]") {
  GuestCall c = make_call();
  c.gpr[0] = 3;
  c.gpr[1] = 4;
  c.lr = 100;
  marshal<with_call>(c);
  CHECK(c.ret_gpr == 107);
}

TEST_CASE("marshal handles void returns", "[unit][kernel_abi]") {
  GuestCall c = make_call();
  c.ret_gpr = 77;
  marshal<void_fn>(c);
  CHECK(c.ret_gpr == 77);
}

TEST_CASE("guest structs are big-endian", "[unit][kernel_abi]") {
  X_ANSI_STRING s{};
  s.length = 0x0102;
  uint8_t raw[8];
  std::memcpy(raw, &s, 8);
  CHECK(raw[0] == 0x01);
  CHECK(raw[1] == 0x02);
  CHECK(STATUS_TIMEOUT == 0x102);
}

TEST_CASE("marshal accepts noexcept functions", "[unit][kernel_abi]") {
  GuestCall c = make_call();
  c.gpr[0] = 4;
  marshal<noexcept_fn>(c);
  CHECK(c.ret_gpr == 5);
}

TEST_CASE("marshal accepts enum parameters and returns", "[unit][kernel_abi]") {
  GuestCall c = make_call();
  c.gpr[0] = 1;
  marshal<enum_fn>(c);
  CHECK(c.ret_gpr == 0xFFFFFFFFC0000034ull);
}

TEST_CASE("stack stride is 8 bytes (10th arg at r1+0x5C)", "[unit][kernel_abi]") {
  GuestCall c = make_call();
  put_be32(c.r1 + 0x54, 111);
  put_be32(c.r1 + 0x5C, 222);
  marshal<ten>(c);
  CHECK(c.ret_gpr == 222);
}
