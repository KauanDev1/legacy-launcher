// SPDX-License-Identifier: MIT
// Rtl strings, Rtl memory helpers and the guest printf family (exports/rtl.cpp, guest_printf.cpp).
#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstdint>
#include <cstring>
#include <string>

#include "runtime/be.h"
#include "runtime/kernel/guest_printf.h"
#include "runtime/kernel/guest_structs.h"
#include "runtime/kernel/status.h"
#include "support/kernel_fixture.h"

using namespace lr::kernel;
using lr::runtime::be;
using lr::testing::KernelFixture;

namespace {

uint32_t put_str(KernelFixture& f, const std::string& s) {
  const uint32_t a = f.alloc(static_cast<uint32_t>(s.size()) + 1);
  std::memcpy(f.mem.translate<char>(a), s.c_str(), s.size() + 1);
  return a;
}

uint32_t put_wstr(KernelFixture& f, const std::u16string& s) {
  const uint32_t a = f.alloc(static_cast<uint32_t>(s.size() + 1) * 2);
  auto* p = f.mem.translate<be<uint16_t>>(a);
  for (size_t i = 0; i < s.size(); ++i) p[i] = static_cast<uint16_t>(s[i]);
  return a;
}

std::string get_str(KernelFixture& f, uint32_t a) { return std::string(f.mem.translate<char>(a)); }

std::u16string get_wstr(KernelFixture& f, uint32_t a) {
  std::u16string s;
  const auto* p = f.mem.translate<be<uint16_t>>(a);
  for (size_t i = 0; p[i] != 0; ++i) s.push_back(static_cast<char16_t>(static_cast<uint16_t>(p[i])));
  return s;
}

uint64_t bits(double d) { return std::bit_cast<uint64_t>(d); }

// A guest va_list of 8-byte slots.
uint32_t put_va(KernelFixture& f, std::initializer_list<uint64_t> slots) {
  const uint32_t a = f.alloc(static_cast<uint32_t>(slots.size()) * 8 + 8);
  auto* p = f.mem.translate<be<uint64_t>>(a);
  size_t i = 0;
  for (uint64_t v : slots) p[i++] = v;
  return a;
}

int32_t as_i32(uint64_t r) { return static_cast<int32_t>(r); }

}  // namespace

TEST_CASE("format integers with width and padding", "[unit][kernel][rtl]") {
  KernelFixture f;
  const uint32_t buf = f.alloc(64);
  CHECK(as_i32(f.call("sprintf", buf, put_str(f, "%08X"), 0x1Au)) == 8);
  CHECK(get_str(f, buf) == "0000001A");
  f.call("sprintf", buf, put_str(f, "[%5d][%-5d][%+d][%u][%x][%o]"), 42u, 42u, 7u, 0xFFFFFFFFu, 255u, 8u);
  CHECK(get_str(f, buf) == "[   42][42   ][+7][4294967295][ff][10]");
  f.call("sprintf", buf, put_str(f, "%d %hd %lld %I64d %%"), 0xFFFFFFFFu, 0x12345u, 0x100000000ull, 0xFFFFFFFFFFull);
  CHECK(get_str(f, buf) == "-1 9029 4294967296 1099511627775 %");
  f.call("sprintf", buf, put_str(f, "%p"), 0x1234u);
  CHECK(get_str(f, buf) == "00001234");
}

TEST_CASE("format guest string and wide string", "[unit][kernel][rtl]") {
  KernelFixture f;
  const uint32_t buf = f.alloc(64);
  const uint32_t narrow = put_str(f, "abc");
  const uint32_t wide = put_wstr(f, u"xyz");
  f.call("sprintf", buf, put_str(f, "%s|%S|%ls|%6s|%.2s|%c"), narrow, wide, wide, narrow, narrow, uint32_t{'Q'});
  CHECK(get_str(f, buf) == "abc|xyz|xyz|   abc|ab|Q");
  f.call("sprintf", buf, put_str(f, "%s"), 0u);
  CHECK(get_str(f, buf) == "(null)");
  // In wide functions %s is wide and %S narrow.
  const uint32_t wbuf = f.alloc(128);
  CHECK(as_i32(f.call("swprintf", wbuf, put_wstr(f, u"%s-%S"), wide, narrow)) == 7);
  CHECK(get_wstr(f, wbuf) == u"xyz-abc");
}

TEST_CASE("format double", "[unit][kernel][rtl]") {
  KernelFixture f;
  const uint32_t buf = f.alloc(64);
  // Doubles travel in the integer slots, never in FPRs.
  f.call("sprintf", buf, put_str(f, "%.3f|%8.2f|%g"), bits(3.14159), bits(-2.5), bits(0.5));
  CHECK(get_str(f, buf) == "3.142|   -2.50|0.5");
}

TEST_CASE("star width from args", "[unit][kernel][rtl]") {
  KernelFixture f;
  const uint32_t buf = f.alloc(64);
  f.call("sprintf", buf, put_str(f, "[%*d][%-*d][%.*s]"), 5u, 42u, 4u, 7u, 1u, put_str(f, "hello"));
  CHECK(get_str(f, buf) == "[   42][7   ][h]");
  f.call("sprintf", buf, put_str(f, "[%*d]"), uint32_t(-4), 9u);  // a negative width left-justifies
  CHECK(get_str(f, buf) == "[9   ]");
}

TEST_CASE("arguments beyond r10 come from the guest stack", "[unit][kernel][rtl]") {
  KernelFixture f;
  const uint32_t buf = f.alloc(64);
  // sprintf(buf, fmt, a1..a8): position 2 is r5, so a7 is position 8 (first stack slot) and a8 position 9.
  f.call("sprintf", buf, put_str(f, "%d %d %d %d %d %d %d %d"), 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u);
  CHECK(get_str(f, buf) == "1 2 3 4 5 6 7 8");
}

TEST_CASE("_snprintf truncates and returns -1 when full", "[unit][kernel][rtl]") {
  KernelFixture f;
  const uint32_t buf = f.alloc(32);
  const uint32_t fmt = put_str(f, "%s");
  const uint32_t src = put_str(f, "abcdef");
  std::memset(f.mem.translate<char>(buf), 'Z', 32);
  SECTION("fits with room for the NUL") {
    CHECK(as_i32(f.call("_snprintf", buf, 10u, fmt, src)) == 6);
    CHECK(get_str(f, buf) == "abcdef");
  }
  SECTION("exactly full: the length is returned, no NUL is written") {
    CHECK(as_i32(f.call("_snprintf", buf, 6u, fmt, src)) == 6);
    CHECK(std::memcmp(f.mem.translate<char>(buf), "abcdefZ", 7) == 0);
  }
  SECTION("truncated: -1 and count bytes written") {
    CHECK(as_i32(f.call("_snprintf", buf, 4u, fmt, src)) == -1);
    CHECK(std::memcmp(f.mem.translate<char>(buf), "abcdZ", 5) == 0);
  }
  SECTION("zero count or null buffer") {
    CHECK(as_i32(f.call("_snprintf", buf, 0u, fmt, src)) == -1);
    CHECK(as_i32(f.call("_snprintf", 0u, 4u, fmt, src)) == -1);
  }
  SECTION("wide") {
    const uint32_t wbuf = f.alloc(32);
    CHECK(as_i32(f.call("_snwprintf", wbuf, 3u, put_wstr(f, u"%d"), 12345u)) == -1);
    CHECK(as_i32(f.call("_snwprintf", wbuf, 8u, put_wstr(f, u"%d"), 12345u)) == 5);
    CHECK(get_wstr(f, wbuf) == u"12345");
  }
}

TEST_CASE("sprintf into an unmapped destination fails without writing", "[unit][kernel][rtl]") {
  KernelFixture f;
  CHECK(as_i32(f.call("sprintf", 0u, put_str(f, "x"))) == -1);
  CHECK(as_i32(f.call("sprintf", 0xFFFFFFFFu, put_str(f, "xyz"))) == -1);  // would run past 4 GiB
}

TEST_CASE("vsprintf reads guest va_list", "[unit][kernel][rtl]") {
  KernelFixture f;
  const uint32_t buf = f.alloc(64);
  // 32-bit values sit in the low half (+4) of their 8-byte slot, a double fills the slot.
  const uint32_t va = put_va(f, {42, put_str(f, "str"), bits(1.5), 0x100000000ull});
  CHECK(as_i32(f.call("vsprintf", buf, put_str(f, "%d %s %.1f %lld"), va)) == 21);
  CHECK(get_str(f, buf) == "42 str 1.5 4294967296");
  CHECK(as_i32(f.call("_vsnprintf", buf, 4u, put_str(f, "%d %s"), va)) == -1);
  CHECK(as_i32(f.call("_vscwprintf", put_wstr(f, u"%d"), va)) == 2);
  const uint32_t wbuf = f.alloc(64);
  const uint32_t wva = put_va(f, {7, put_wstr(f, u"w")});
  CHECK(as_i32(f.call("vswprintf", wbuf, put_wstr(f, u"%d%s"), wva)) == 2);
  CHECK(get_wstr(f, wbuf) == u"7w");
}

TEST_CASE("guest_format direct and defensive", "[unit][kernel][rtl]") {
  KernelFixture f;
  struct NoArgs : GuestArgs {
    uint64_t next_int(bool) override { return 0; }
    double next_double() override { return 0; }
  } none;
  CHECK(guest_format(f.mem.base(), "100%", none) == "100%");
  CHECK(guest_format(f.mem.base(), "%Z", none) == "%Z");
  CHECK(guest_format(f.mem.base(), "%n!", none) == "!");
  CHECK(guest_format(f.mem.base(), "%1000000000d", none).size() == 65536);  // width is capped
  VaListArgs bad(f.mem.base(), 0);
  CHECK(guest_format(f.mem.base(), "%d", bad) == "0");  // an invalid va_list reads zeros
}

TEST_CASE("RtlInitAnsiString lengths", "[unit][kernel][rtl]") {
  KernelFixture f;
  const uint32_t s = f.alloc(sizeof(X_ANSI_STRING));
  const uint32_t src = put_str(f, "hello");
  f.call("RtlInitAnsiString", s, src);
  const auto* a = f.mem.translate<X_ANSI_STRING>(s);
  CHECK(a->length == 5);
  CHECK(a->maximum_length == 6);
  CHECK(a->buffer == src);
  f.call("RtlInitAnsiString", s, 0u);
  CHECK(a->length == 0);
  CHECK(a->maximum_length == 0);
  CHECK(a->buffer == 0);

  const uint32_t u = f.alloc(sizeof(X_UNICODE_STRING));
  const uint32_t wsrc = put_wstr(f, u"héllo");
  f.call("RtlInitUnicodeString", u, wsrc);
  const auto* w = f.mem.translate<X_UNICODE_STRING>(u);
  CHECK(w->length == 10);
  CHECK(w->maximum_length == 12);
  CHECK(w->buffer == wsrc);
}

TEST_CASE("unicode to multibyte round trip", "[unit][kernel][rtl]") {
  KernelFixture f;
  const uint32_t wide = put_wstr(f, u"Hello");
  const uint32_t narrow = f.alloc(16);
  const uint32_t out = f.alloc(4);
  CHECK(f.call("RtlUnicodeToMultiByteN", narrow, 16u, out, wide, 10u) == 0);
  CHECK(*f.mem.translate<be<uint32_t>>(out) == 5);
  CHECK(get_str(f, narrow) == "Hello");
  const uint32_t back = f.alloc(32);
  CHECK(f.call("RtlMultiByteToUnicodeN", back, 32u, out, narrow, 5u) == 0);
  CHECK(*f.mem.translate<be<uint32_t>>(out) == 10);
  CHECK(get_wstr(f, back) == u"Hello");
  // The destination limit truncates (and a null out pointer is fine).
  CHECK(f.call("RtlMultiByteToUnicodeN", back, 4u, 0u, narrow, 5u) == 0);

  SECTION("RtlUnicodeStringToAnsiString allocates and RtlFreeAnsiString frees") {
    const uint32_t us = f.alloc(sizeof(X_UNICODE_STRING));
    const uint32_t as = f.alloc(sizeof(X_ANSI_STRING));
    f.call("RtlInitUnicodeString", us, wide);
    CHECK(f.call("RtlUnicodeStringToAnsiString", as, us, 1u) == STATUS_SUCCESS);
    const auto* a = f.mem.translate<X_ANSI_STRING>(as);
    CHECK(a->length == 5);
    CHECK(a->maximum_length == 6);
    REQUIRE(a->buffer != 0);
    CHECK(get_str(f, a->buffer) == "Hello");
    f.call("RtlFreeAnsiString", as);
    CHECK(a->buffer == 0);
    CHECK(a->length == 0);
  }
  SECTION("a caller buffer that is too small overflows") {
    const uint32_t us = f.alloc(sizeof(X_UNICODE_STRING));
    const uint32_t as = f.alloc(sizeof(X_ANSI_STRING));
    f.call("RtlInitUnicodeString", us, wide);
    auto* a = f.mem.translate<X_ANSI_STRING>(as);
    a->maximum_length = 3;
    a->buffer = narrow;
    CHECK(static_cast<uint32_t>(f.call("RtlUnicodeStringToAnsiString", as, us, 0u)) == STATUS_BUFFER_OVERFLOW);
    a->maximum_length = 5;  // exactly the text: no NUL
    CHECK(f.call("RtlUnicodeStringToAnsiString", as, us, 0u) == STATUS_SUCCESS);
    CHECK(a->length == 5);
  }
}

TEST_CASE("Rtl memory helpers", "[unit][kernel][rtl]") {
  KernelFixture f;
  const uint32_t a = put_str(f, "abcdef");
  const uint32_t b = put_str(f, "abcxef");
  CHECK(f.call("RtlCompareMemory", a, b, 6u) == 3);
  CHECK(f.call("RtlCompareMemory", a, a, 6u) == 6);
  const uint32_t m = f.alloc(32);
  f.call("RtlFillMemoryUlong", m, 14u, 0x11223344u);  // 3 whole words
  auto* w = f.mem.translate<be<uint32_t>>(m);
  CHECK(static_cast<uint32_t>(w[0]) == 0x11223344u);
  CHECK(static_cast<uint32_t>(w[2]) == 0x11223344u);
  CHECK(static_cast<uint32_t>(w[3]) == 0);
  CHECK(f.call("RtlCompareMemoryUlong", m, 16u, 0x11223344u) == 12);
  CHECK(f.call("RtlCompareMemoryUlong", m, 16u, 0x55u) == 0);
}

TEST_CASE("RtlNtStatusToDosError(STATUS_OBJECT_NAME_NOT_FOUND) == 2", "[unit][kernel][rtl]") {
  KernelFixture f;
  CHECK(f.call("RtlNtStatusToDosError", STATUS_OBJECT_NAME_NOT_FOUND) == 2);
  CHECK(f.call("RtlNtStatusToDosError", STATUS_SUCCESS) == 0);
  CHECK(f.call("RtlNtStatusToDosError", STATUS_ACCESS_DENIED) == 5);
  CHECK(f.call("RtlNtStatusToDosError", STATUS_INVALID_PARAMETER) == 87);
  CHECK(f.call("RtlNtStatusToDosError", STATUS_PENDING) == 997);
  CHECK(f.call("RtlNtStatusToDosError", STATUS_NO_MORE_FILES) == 18);
  CHECK(f.call("RtlNtStatusToDosError", 0xC0FFEE00u) == 317);  // unmapped
}
