// SPDX-License-Identifier: MIT
#include "runtime/kernel/guest_printf.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "runtime/be.h"

namespace lr::kernel {
namespace {

using lr::runtime::be;

constexpr size_t kMaxOutput = 1u << 20;
constexpr int kMaxFieldWidth = 1 << 16;  // a hostile "%99999999d" must not allocate gigabytes

bool valid_addr(uint32_t addr, uint64_t bytes) {
  return addr >= 0x10000 && static_cast<uint64_t>(addr) + bytes <= (uint64_t{1} << 32);
}

// Text is carried as 16-bit code units internally so one engine serves narrow and wide formats.
using Units = std::u16string;

struct Spec {
  bool left = false, plus = false, space = false, alt = false, zero = false;
  int width = 0;
  int prec = -1;
  bool is_long = false, is_ll = false, is_short = false, is_wide_flag = false;
};

Units from_ascii(std::string_view s) {
  Units u;
  u.reserve(s.size());
  for (char c : s) u.push_back(static_cast<unsigned char>(c));
  return u;
}

void pad_and_append(Units& out, const Units& body, const Spec& sp) {
  const size_t w = static_cast<size_t>(sp.width);
  const size_t pad = body.size() < w ? w - body.size() : 0;
  if (!sp.left) out.append(pad, u' ');
  out += body;
  if (sp.left) out.append(pad, u' ');
}

template <class V>
std::string c_format(const std::string& spec, V v) {
  const int n = std::snprintf(nullptr, 0, spec.c_str(), v);
  const size_t len = static_cast<size_t>(std::max(n, 0));
  std::string s(len + 1, '\0');
  std::snprintf(s.data(), s.size(), spec.c_str(), v);
  s.resize(len);
  return s;
}

// "%" + flags + width + precision, without length or conversion.
std::string spec_prefix(const Spec& sp) {
  std::string s = "%";
  if (sp.left) s += '-';
  if (sp.plus) s += '+';
  if (sp.space) s += ' ';
  if (sp.alt) s += '#';
  if (sp.zero) s += '0';
  if (sp.width > 0) s += std::to_string(sp.width);
  if (sp.prec >= 0) s += "." + std::to_string(sp.prec);
  return s;
}

// Guest strings as code units, bounded by `limit` (-1 = the global cap).
Units narrow_units(const uint8_t* base, uint32_t addr, int limit) {
  Units u;
  if (!valid_addr(addr, 1)) return from_ascii("(null)");
  const uint64_t room = (uint64_t{1} << 32) - addr;
  const uint64_t cap = std::min<uint64_t>(room, limit >= 0 ? static_cast<uint64_t>(limit) : kMaxOutput);
  for (uint64_t i = 0; i < cap; ++i) {
    const uint8_t b = base[addr + i];
    if (b == 0) break;
    u.push_back(b);
  }
  return u;
}

Units wide_units(const uint8_t* base, uint32_t addr, int limit) {
  Units u;
  if (!valid_addr(addr, 2)) return from_ascii("(null)");
  const uint64_t room = ((uint64_t{1} << 32) - addr) / 2;
  const uint64_t cap = std::min<uint64_t>(room, limit >= 0 ? static_cast<uint64_t>(limit) : kMaxOutput);
  for (uint64_t i = 0; i < cap; ++i) {
    const char16_t c = static_cast<char16_t>(*reinterpret_cast<const be<uint16_t>*>(base + addr + i * 2));
    if (c == 0) break;
    u.push_back(c);
  }
  return u;
}

// The shared engine. `wide_default` is true for the wide functions (%s / %c are wide there).
Units format_units(uint8_t* base, const Units& fmt, GuestArgs& args, bool wide_default) {
  Units out;
  size_t i = 0;
  const size_t n = fmt.size();
  while (i < n && out.size() < kMaxOutput) {
    if (fmt[i] != u'%') {
      out.push_back(fmt[i++]);
      continue;
    }
    const size_t start = i++;
    if (i >= n) {
      out.append(fmt, start, Units::npos);
      break;
    }
    Spec sp;
    for (; i < n; ++i) {
      const char16_t c = fmt[i];
      if (c == u'-') sp.left = true;
      else if (c == u'+') sp.plus = true;
      else if (c == u' ') sp.space = true;
      else if (c == u'#') sp.alt = true;
      else if (c == u'0') sp.zero = true;
      else break;
    }
    auto number = [&](int& dst) {
      long v = 0;
      while (i < n && fmt[i] >= u'0' && fmt[i] <= u'9') {
        v = std::min<long>(v * 10 + (fmt[i] - u'0'), kMaxFieldWidth);
        ++i;
      }
      dst = static_cast<int>(v);
    };
    if (i < n && fmt[i] == u'*') {
      ++i;
      int w = static_cast<int32_t>(args.next_int(false));
      if (w < 0) {
        sp.left = true;
        w = (w == INT32_MIN) ? kMaxFieldWidth : -w;
      }
      sp.width = std::min(w, kMaxFieldWidth);
    } else {
      number(sp.width);
    }
    if (i < n && fmt[i] == u'.') {
      ++i;
      if (i < n && fmt[i] == u'*') {
        ++i;
        const int p = static_cast<int32_t>(args.next_int(false));
        sp.prec = p < 0 ? -1 : std::min(p, kMaxFieldWidth);
      } else {
        sp.prec = 0;
        number(sp.prec);
      }
    }
    // Length modifiers (Xbox 360 long is 32-bit).
    for (; i < n; ++i) {
      const char16_t c = fmt[i];
      if (c == u'l') {
        if (sp.is_long) sp.is_ll = true;
        sp.is_long = true;
      } else if (c == u'h') {
        sp.is_short = true;
      } else if (c == u'w') {
        sp.is_wide_flag = true;
      } else if (c == u'L' || c == u'z' || c == u't' || c == u'j') {
        // accepted, no effect (Xenia also ignores L)
      } else if (c == u'I') {
        if (i + 2 < n && fmt[i + 1] == u'6' && fmt[i + 2] == u'4') {
          sp.is_ll = true;
          i += 2;
        } else if (i + 2 < n && fmt[i + 1] == u'3' && fmt[i + 2] == u'2') {
          i += 2;
        }
      } else {
        break;
      }
    }
    if (i >= n) {  // truncated spec: copy it through
      out.append(fmt, start, Units::npos);
      break;
    }
    const char16_t conv = fmt[i++];
    switch (conv) {
      case u'%':
        out.push_back(u'%');
        break;
      case u'd':
      case u'i':
      case u'u':
      case u'x':
      case u'X':
      case u'o':
      case u'p': {
        const bool is_ptr = conv == u'p';
        const bool wide64 = sp.is_ll && !is_ptr;
        const uint64_t raw = args.next_int(wide64);
        const bool is_signed = conv == u'd' || conv == u'i';
        long long v;
        if (is_signed) {
          if (wide64) v = static_cast<int64_t>(raw);
          else if (sp.is_short) v = static_cast<int16_t>(raw);
          else v = static_cast<int32_t>(raw);
        } else {
          if (wide64) v = static_cast<long long>(raw);
          else if (sp.is_short) v = static_cast<uint16_t>(raw);
          else v = static_cast<uint32_t>(raw);
        }
        Spec ps = sp;
        char conv_c = static_cast<char>(conv);
        if (is_ptr) {  // %p: upper-case hex, at least 8 digits (Xenia)
          ps.prec = std::max(ps.prec, 8);
          conv_c = 'X';
        }
        out += from_ascii(c_format(spec_prefix(ps) + "ll" + conv_c, v));
        break;
      }
      case u'e':
      case u'E':
      case u'f':
      case u'F':
      case u'g':
      case u'G':
      case u'a':
      case u'A': {
        const double d = args.next_double();
        out += from_ascii(c_format(spec_prefix(sp) + static_cast<char>(conv), d));
        break;
      }
      case u'c':
      case u'C': {
        bool wide = wide_default;
        if (conv == u'C') wide = !wide;
        if (sp.is_long || sp.is_wide_flag) wide = true;
        if (sp.is_short) wide = false;
        const uint64_t v = args.next_int(false);
        const Units body(1, wide ? static_cast<char16_t>(v & 0xFFFF) : static_cast<char16_t>(v & 0xFF));
        pad_and_append(out, body, sp);
        break;
      }
      case u's':
      case u'S': {
        bool wide = wide_default;
        if (conv == u'S') wide = !wide;
        if (sp.is_long || sp.is_wide_flag) wide = true;
        if (sp.is_short) wide = false;
        const uint32_t addr = static_cast<uint32_t>(args.next_int(false));
        Units body = wide ? wide_units(base, addr, sp.prec) : narrow_units(base, addr, sp.prec);
        pad_and_append(out, body, sp);
        break;
      }
      case u'n':
        (void)args.next_int(false);  // consumed, never written
        break;
      default:  // unknown conversion: copy the spec through
        out.append(fmt, start, i - start);
        break;
    }
  }
  if (out.size() > kMaxOutput) out.resize(kMaxOutput);
  return out;
}

}  // namespace

uint64_t CallArgs::next_int(bool wide64) {
  const int i = index_++;
  if (i < 0) return 0;
  if (i < 8) {
    const uint64_t v = call_.gpr[i];
    return wide64 ? v : (v & 0xFFFFFFFFull);
  }
  const uint32_t slot = call_.r1 + 0x50 + static_cast<uint32_t>(i - 8) * 8;
  if (wide64) return *reinterpret_cast<const be<uint64_t>*>(call_.base + slot);
  return *reinterpret_cast<const be<uint32_t>*>(call_.base + slot + 4);
}

double CallArgs::next_double() { return std::bit_cast<double>(next_int(true)); }

uint64_t VaListArgs::next_int(bool wide64) {
  const uint64_t addr = static_cast<uint64_t>(va_list_) + static_cast<uint64_t>(index_++) * 8;
  if (va_list_ < 0x10000 || addr + 8 > (uint64_t{1} << 32)) return 0;
  const uint8_t* p = base_ + addr;
  if (wide64) return *reinterpret_cast<const be<uint64_t>*>(p);
  return *reinterpret_cast<const be<uint32_t>*>(p + 4);
}

double VaListArgs::next_double() { return std::bit_cast<double>(next_int(true)); }

std::string guest_format(uint8_t* base, std::string_view fmt, GuestArgs& args) {
  const Units u = format_units(base, from_ascii(fmt), args, false);
  std::string s;
  s.reserve(u.size());
  // Narrow output: a wide character that does not fit one byte becomes '?'.
  for (char16_t c : u) s.push_back(c < 0x100 ? static_cast<char>(c) : '?');
  return s;
}

std::u16string guest_format_wide(uint8_t* base, std::u16string_view fmt, GuestArgs& args) {
  return format_units(base, Units(fmt), args, true);
}

std::string read_guest_string(const uint8_t* base, uint32_t addr, uint32_t max_chars) {
  if (!valid_addr(addr, 1)) return {};
  const uint64_t room = (uint64_t{1} << 32) - addr;
  const char* p = reinterpret_cast<const char*>(base + addr);
  return std::string(p, ::strnlen(p, static_cast<size_t>(std::min<uint64_t>(room, max_chars))));
}

std::u16string read_guest_wstring(const uint8_t* base, uint32_t addr, uint32_t max_chars) {
  std::u16string s;
  if (!valid_addr(addr, 2)) return s;
  const uint64_t cap = std::min<uint64_t>(((uint64_t{1} << 32) - addr) / 2, max_chars);
  for (uint64_t i = 0; i < cap; ++i) {
    const char16_t c = static_cast<char16_t>(*reinterpret_cast<const be<uint16_t>*>(base + addr + i * 2));
    if (c == 0) break;
    s.push_back(c);
  }
  return s;
}

}  // namespace lr::kernel
