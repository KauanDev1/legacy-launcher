// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <bit>
#include <cstdint>
#include <type_traits>

namespace lr::runtime {

namespace detail {
// std::byteswap is C++23 and unavailable in -std=c++20; use compiler builtins.
constexpr uint8_t bswap(uint8_t v) { return v; }
constexpr uint16_t bswap(uint16_t v) { return __builtin_bswap16(v); }
constexpr uint32_t bswap(uint32_t v) { return __builtin_bswap32(v); }
constexpr uint64_t bswap(uint64_t v) { return __builtin_bswap64(v); }

template <size_t N>
struct UIntOf;
template <> struct UIntOf<1> { using type = uint8_t; };
template <> struct UIntOf<2> { using type = uint16_t; };
template <> struct UIntOf<4> { using type = uint32_t; };
template <> struct UIntOf<8> { using type = uint64_t; };
}  // namespace detail

// Big-endian storage of an integral/float/double value, as laid out in guest memory.
template <class T>
struct be {
  static_assert(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>, "be<T> supports integral, float and double");
  using Raw = typename detail::UIntOf<sizeof(T)>::type;

  be() = default;
  constexpr be(T v) : raw_(detail::bswap(std::bit_cast<Raw>(v))) {}  // NOLINT: implicit by design
  constexpr be& operator=(T v) {
    raw_ = detail::bswap(std::bit_cast<Raw>(v));
    return *this;
  }
  constexpr operator T() const { return std::bit_cast<T>(detail::bswap(raw_)); }  // NOLINT

 private:
  Raw raw_;
};

static_assert(sizeof(be<uint32_t>) == 4);
static_assert(std::is_trivially_copyable_v<be<uint32_t>>);

}  // namespace lr::runtime
