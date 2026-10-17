// SPDX-License-Identifier: MIT
// Test fixture for kernel imports: a fresh guest address space with the process-wide Kernel bound to it, and
// helpers that call a registered import exactly as a generated thunk would (GuestCall + current guest frame).
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <type_traits>
#include <utility>

#include "runtime/be.h"
#include "runtime/guest_memory.h"
#include "runtime/kernel/abi.h"
#include "runtime/kernel/glue.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/registry.h"

namespace lr::testing {

struct KernelFixture {
  static constexpr uint32_t kStackPointer = 0x30000000;  // r1 of every call()
  static constexpr uint32_t kHeapBase = 0x20000000;      // alloc() bump pointer start

  // Each fixture instance replaces the process-wide Kernel (reset_kernel_for_tests + init_kernel), so several
  // fixtures in one process (Catch2 sections, several test cases) each get a kernel bound to their own memory.
  KernelFixture() : mem(reserve()) {
    lr::kernel::reset_kernel_for_tests();
    auto r = lr::kernel::init_kernel(mem, {});
    REQUIRE(r);
  }
  ~KernelFixture() { lr::kernel::reset_kernel_for_tests(); }
  KernelFixture(const KernelFixture&) = delete;
  KernelFixture& operator=(const KernelFixture&) = delete;

  lr::runtime::GuestMemory mem;

  // Calls the registered import `import` with integer arguments (positions 0..7 in r3..r10, the rest on the
  // guest stack via put_stack_arg) and returns r3. Records the current guest frame like a thunk does.
  template <class... A>
  uint64_t call(const char* import, A... args) {
    const lr::kernel::ImportEntry e = lr::kernel::find_import(import);
    REQUIRE(e.fn != nullptr);
    lr::kernel::GuestCall c;
    c.base = mem.base();
    c.r1 = kStackPointer;
    int i = 0;
    auto put = [&](uint64_t v) {
      if (i < 8) c.gpr[i] = v;
      else put_stack_arg(i, v);
      ++i;
    };
    (put(to_reg(args)), ...);
    lr::kernel::trace(import);
    lr::kernel::set_current_frame(c.r1, c.r13);
    e.fn(c);
    return c.ret_gpr;
  }

  // Bump allocator for test data in guest memory, 16-byte aligned, zero-filled (fresh mapping).
  uint32_t alloc(uint32_t bytes) {
    const uint32_t a = next_;
    next_ = (next_ + bytes + 0xF) & ~0xFu;
    return a;
  }

  // Writes argument position `index` (>= 8) to its 8-byte big-endian stack slot at r1 + 0x50 + (index - 8) * 8,
  // where marshal reads it (32-bit values from the low half at +4, 64-bit from the whole slot).
  void put_stack_arg(int index, uint64_t v) {
    REQUIRE(index >= 8);
    *mem.translate<lr::runtime::be<uint64_t>>(kStackPointer + 0x50 + static_cast<uint32_t>(index - 8) * 8) = v;
  }

 private:
  static lr::runtime::GuestMemory reserve() {
    auto m = lr::runtime::GuestMemory::reserve(0x1'0000'0000ull);
    REQUIRE(m);
    return std::move(m.value());
  }
  template <class T>
  static uint64_t to_reg(T v) {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "KernelFixture::call takes integer arguments");
    if constexpr (std::is_enum_v<T>) return static_cast<uint64_t>(static_cast<std::underlying_type_t<T>>(v));
    else return static_cast<uint64_t>(v);  // signed values sign-extend, as in a guest register
  }

  uint32_t next_ = kHeapBase;
};

}  // namespace lr::testing
