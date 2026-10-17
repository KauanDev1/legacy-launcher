// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

#include "runtime/be.h"
#include "runtime/kernel/abi.h"

namespace lr::kernel {

// A 32-bit guest address typed by what it points at.
template <class T>
struct GuestPtr {
  uint32_t addr;

  T* get(uint8_t* base) const { return addr == 0 ? nullptr : reinterpret_cast<T*>(base + addr); }
  explicit operator bool() const { return addr != 0; }
  // False for the null page (< 0x10000) or a range that runs past 4 GiB.
  bool valid_range(size_t bytes) const {
    return addr >= 0x10000 && static_cast<uint64_t>(addr) + static_cast<uint64_t>(bytes) <= (uint64_t{1} << 32);
  }
};

namespace detail {

template <class T>
struct IsGuestPtr : std::false_type {};
template <class T>
struct IsGuestPtr<GuestPtr<T>> : std::true_type {};

template <class T>
constexpr bool is_float_v = std::is_same_v<T, float> || std::is_same_v<T, double>;

template <class T>
constexpr bool is_int_arg_v = (std::is_integral_v<T> && sizeof(T) <= 8) || IsGuestPtr<T>::value;

template <class F>
struct FnTraits;
template <class R, class... A>
struct FnTraits<R (*)(A...)> {
  using Ret = R;
  using Args = std::tuple<A...>;
};

// Optional leading GuestCall& (does not consume a position).
template <class Tuple>
struct SkipCall {
  static constexpr bool has_call = false;
  using Rest = Tuple;
};
template <class... A>
struct SkipCall<std::tuple<GuestCall&, A...>> {
  static constexpr bool has_call = true;
  using Rest = std::tuple<A...>;
};

// Stack argument slots (PPC32 Xbox 360 ABI): 8 bytes each starting at r1 + 0x50. A 32-bit value is the low
// (second) half of its big-endian slot, hence +0x54; a 64-bit value occupies the whole slot at +0x50.
//
// Checked against xenia-project/xenia src/xenia/kernel/util/shim_utils.h: Xenia also starts stack
// arguments at r1 + 0x54 with an 8-byte stride (ordinal i >= 8 -> r1 + 0x54 + (i-8)*8) and lets float and
// double parameters consume an integer ordinal. Xenia uses 0x54 for every width (a 64-bit load there
// straddles two slots), so for 64-bit values we follow the brief and the ABI (0x50) instead. Xenia also
// indexes the first float parameter at f[2]; we use f1 as the ABI says.
template <class T>
T read_int_arg(const GuestCall& c, size_t pos) {
  using Raw = std::conditional_t<IsGuestPtr<T>::value, uint32_t, T>;
  uint64_t v;
  if (pos < 8) {
    v = c.gpr[pos];
  } else {
    const uint32_t slot = c.r1 + 0x50 + static_cast<uint32_t>(pos - 8) * 8;
    if constexpr (sizeof(Raw) == 8) {
      v = *reinterpret_cast<const lr::runtime::be<uint64_t>*>(c.base + slot);
    } else {
      v = *reinterpret_cast<const lr::runtime::be<uint32_t>*>(c.base + slot + 4);
    }
  }
  if constexpr (IsGuestPtr<T>::value) {
    return T{static_cast<uint32_t>(v)};
  } else if constexpr (std::is_same_v<T, bool>) {
    return (v & 0xFF) != 0;
  } else {
    return static_cast<T>(v);
  }
}

template <class T>
T read_arg(const GuestCall& c, size_t pos, size_t fpos) {
  if constexpr (is_float_v<T>) {
    return static_cast<T>(c.fpr[fpos]);
  } else {
    static_assert(is_int_arg_v<T>, "unsupported kernel argument type");
    return read_int_arg<T>(c, pos);
  }
}

// Number of floating-point parameters before index I.
template <class Tuple, size_t I>
constexpr size_t fpos_of() {
  size_t n = 0;
  [&]<size_t... J>(std::index_sequence<J...>) {
    ((n += (J < I && is_float_v<std::tuple_element_t<J, Tuple>>) ? 1 : 0), ...);
  }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
  return n;
}

template <class R>
void write_ret(GuestCall& c, R v) {
  if constexpr (is_float_v<R>) {
    c.ret_fpr = static_cast<double>(v);
    c.has_fpr_return = true;
  } else if constexpr (IsGuestPtr<R>::value) {
    c.ret_gpr = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v.addr)));
  } else {
    static_assert(std::is_integral_v<R> && sizeof(R) <= 8, "unsupported kernel return type");
    if constexpr (sizeof(R) == 8) {
      c.ret_gpr = static_cast<uint64_t>(v);
    } else {
      c.ret_gpr = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(v))));
    }
  }
}

}  // namespace detail

// Adapts a typed kernel function to the KernelFn signature. Arguments are built by position: integers and
// GuestPtr from r3..r10 then the stack; float/double from f1.. (and they still consume an integer
// position). An optional FIRST parameter `GuestCall&` is passed the raw call (for `base`, `lr`, ...) and does
// not consume a position. Returns: void -> nothing; <= 32-bit integers sign-extend from 32 bits into r3;
// 64-bit verbatim; float/double -> f1.
template <auto Fn>
void marshal(GuestCall& c) {
  using Tr = detail::FnTraits<decltype(Fn)>;
  using Skip = detail::SkipCall<typename Tr::Args>;
  using Rest = typename Skip::Rest;
  constexpr size_t N = std::tuple_size_v<Rest>;
  [&]<size_t... I>(std::index_sequence<I...>) {
    auto invoke = [&](auto&&... pre) {
      if constexpr (std::is_void_v<typename Tr::Ret>) {
        Fn(pre..., detail::read_arg<std::tuple_element_t<I, Rest>>(c, I, detail::fpos_of<Rest, I>())...);
      } else {
        detail::write_ret(
            c, Fn(pre..., detail::read_arg<std::tuple_element_t<I, Rest>>(c, I, detail::fpos_of<Rest, I>())...));
      }
    };
    if constexpr (Skip::has_call) {
      invoke(c);
    } else {
      invoke();
    }
  }(std::make_index_sequence<N>{});
}

}  // namespace lr::kernel
