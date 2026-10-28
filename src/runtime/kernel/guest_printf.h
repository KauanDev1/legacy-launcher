// SPDX-License-Identifier: MIT
// A printf interpreter over guest arguments. The Xbox 360 printf family never sees a host va_list: variadic
// arguments come from guest registers and stack (CallArgs) or from a guest va_list in guest memory (VaListArgs).
//
// Conventions, checked against Xenia's xboxkrnl_strings.cc (semantic reference only, no code copied):
//  * Every variadic argument takes exactly one 8-byte slot, whatever its width. Doubles are read from the same
//    integer slots (r3..r10, then the stack), never from FPRs.
//  * A guest va_list is a pointer to a flat array of 8-byte big-endian slots; a 32-bit value is the low half of its
//    slot (+4), a 64-bit value or a double the whole slot. The va_list pointer itself is never advanced in guest
//    memory (VaListArgs keeps its own index).
//  * %s / %S: a narrow function reads %s as guest char* and %S (%ls, %ws) as guest BE UTF-16; a wide function
//    reads %s as wide and %S (%hs) as narrow (MSVC semantics). Same rule for %c / %C.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "runtime/kernel/abi.h"

namespace lr::kernel {

class GuestArgs {
 public:
  virtual ~GuestArgs() = default;
  // The next integer argument: wide64 reads the whole 64-bit slot, otherwise the low 32 bits (zero-extended).
  virtual uint64_t next_int(bool wide64) = 0;
  virtual double next_double() = 0;
};

// Variadic arguments of a kernel call starting at argument position `first_index` (0 = r3): r3..r10, then the
// guest stack slots (32-bit at r1+0x54+(i-8)*8, 64-bit at r1+0x50+(i-8)*8, as marshal.h).
class CallArgs : public GuestArgs {
 public:
  CallArgs(const GuestCall& call, int first_index) : call_(call), index_(first_index) {}
  uint64_t next_int(bool wide64) override;
  double next_double() override;

 private:
  const GuestCall& call_;
  int index_;
};

// Arguments read from the guest va_list at address `va_list` (8-byte slots, see above). An invalid address
// (null page, or past 4 GiB) reads zeros.
class VaListArgs : public GuestArgs {
 public:
  VaListArgs(uint8_t* base, uint32_t va_list) : base_(base), va_list_(va_list) {}
  uint64_t next_int(bool wide64) override;
  double next_double() override;

 private:
  uint8_t* base_;
  uint32_t va_list_;
  uint32_t index_ = 0;
};

// Formats `fmt` (host text; wide text is host-order UTF-16) reading guest strings through `base`. Output is capped
// at 1 MiB of characters. Unknown conversions are copied through verbatim; %n is ignored (it never writes).
std::string guest_format(uint8_t* base, std::string_view fmt, GuestArgs& args);
std::u16string guest_format_wide(uint8_t* base, std::u16string_view fmt, GuestArgs& args);

// Bounded readers for NUL-terminated guest strings (stop at the terminator, `max_chars`, or the end of the 4 GiB
// space). A null or null-page pointer yields an empty string.
std::string read_guest_string(const uint8_t* base, uint32_t addr, uint32_t max_chars = 1u << 20);
std::u16string read_guest_wstring(const uint8_t* base, uint32_t addr, uint32_t max_chars = 1u << 20);

}  // namespace lr::kernel
