// SPDX-License-Identifier: MIT
// Rtl string and memory helpers and the printf family. Argument orders and result conventions follow Xenia's
// xboxkrnl_rtl.cc and xboxkrnl_strings.cc (semantic reference only, no code copied); the printf argument sources and
// the guest va_list layout are documented in guest_printf.h.
//
// Deviations from Xenia, on purpose:
//  * _vsnprintf / _vsnwprintf return -1 on truncation like MSVC (Xenia returns the would-be length). _vsnwprintf is
//    not exported by this plan; _vsnprintf is.
//  * swprintf / vswprintf read %s as wide (MSVC); Xenia's swprintf reads it as narrow.
//  * A destination that is not mapped makes sprintf-style functions return -1 without writing.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "runtime/be.h"
#include "runtime/kernel/guest_printf.h"
#include "runtime/kernel/guest_structs.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/marshal.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/status.h"

namespace lr::kernel {
namespace {

using U32Ptr = GuestPtr<be<uint32_t>>;

constexpr uint32_t kMaxStringChars = 0xFFFE;  // UNICODE_STRING / ANSI_STRING lengths are 16-bit

struct StatusDos {
  uint32_t status;
  uint32_t dos;
};

// NT status -> Win32 error, the standard mapping (RtlNtStatusToDosError) for every status in status.h.
constexpr StatusDos kStatusToDos[] = {
    {STATUS_SUCCESS, 0},
    {STATUS_ABANDONED_WAIT_0, 735},         // ERROR_ABANDONED_WAIT_0
    {STATUS_USER_APC, 192},                 // WAIT_IO_COMPLETION
    {STATUS_ALERTED, 0},                    // informational: no Win32 error
    {STATUS_TIMEOUT, 258},                  // WAIT_TIMEOUT
    {STATUS_PENDING, 997},                  // ERROR_IO_PENDING
    {STATUS_TIMER_RESUME_IGNORED, 1316},    // ERROR_TIMER_RESUME_IGNORED
    {STATUS_BUFFER_OVERFLOW, 234},          // ERROR_MORE_DATA
    {STATUS_NO_MORE_FILES, 18},             // ERROR_NO_MORE_FILES
    {STATUS_UNSUCCESSFUL, 31},              // ERROR_GEN_FAILURE
    {STATUS_NOT_IMPLEMENTED, 1},            // ERROR_INVALID_FUNCTION
    {STATUS_INFO_LENGTH_MISMATCH, 24},      // ERROR_BAD_LENGTH
    {STATUS_ACCESS_VIOLATION, 998},         // ERROR_NOACCESS
    {STATUS_INVALID_HANDLE, 6},             // ERROR_INVALID_HANDLE
    {STATUS_INVALID_PARAMETER, 87},         // ERROR_INVALID_PARAMETER
    {STATUS_NO_SUCH_FILE, 2},               // ERROR_FILE_NOT_FOUND
    {STATUS_INVALID_DEVICE_REQUEST, 1},     // ERROR_INVALID_FUNCTION
    {STATUS_END_OF_FILE, 38},               // ERROR_HANDLE_EOF
    {STATUS_NO_MEMORY, 8},                  // ERROR_NOT_ENOUGH_MEMORY
    {STATUS_CONFLICTING_ADDRESSES, 487},    // ERROR_INVALID_ADDRESS
    {STATUS_ACCESS_DENIED, 5},              // ERROR_ACCESS_DENIED
    {STATUS_BUFFER_TOO_SMALL, 122},         // ERROR_INSUFFICIENT_BUFFER
    {STATUS_OBJECT_TYPE_MISMATCH, 6},       // ERROR_INVALID_HANDLE
    {STATUS_OBJECT_NAME_INVALID, 123},      // ERROR_INVALID_NAME
    {STATUS_OBJECT_NAME_NOT_FOUND, 2},      // ERROR_FILE_NOT_FOUND
    {STATUS_OBJECT_NAME_COLLISION, 183},    // ERROR_ALREADY_EXISTS
    {STATUS_OBJECT_PATH_NOT_FOUND, 3},      // ERROR_PATH_NOT_FOUND
    {STATUS_NOT_FOUND, 1168},               // ERROR_NOT_FOUND
    {STATUS_INVALID_PAGE_PROTECTION, 87},   // ERROR_INVALID_PARAMETER
    {STATUS_MUTANT_NOT_OWNED, 288},         // ERROR_NOT_OWNER
    {STATUS_SEMAPHORE_LIMIT_EXCEEDED, 298}, // ERROR_TOO_MANY_POSTS
    {STATUS_DISK_FULL, 112},                // ERROR_DISK_FULL
    {STATUS_MEMORY_NOT_ALLOCATED, 487},     // ERROR_INVALID_ADDRESS
    {STATUS_FILE_IS_A_DIRECTORY, 5},        // ERROR_ACCESS_DENIED
    {STATUS_NOT_A_DIRECTORY, 267},          // ERROR_DIRECTORY
};
constexpr uint32_t kErrorMrMidNotFound = 317;  // what Windows returns for a status without a mapping

// A UTF-16 unit as one narrow byte: Latin-1 passes through, anything else is '?' (the guest has no code page here).
char narrow_char(char16_t c) { return c < 0x100 ? static_cast<char>(c) : '?'; }

// Writes `text` plus NUL at guest `dst` when the whole range is mapped. Returns false (nothing written) otherwise.
bool put_narrow(uint8_t* base, uint32_t dst, const std::string& text) {
  if (!GuestPtr<char>{dst}.valid_range(text.size() + 1)) return false;
  std::memcpy(base + dst, text.data(), text.size());
  base[dst + text.size()] = 0;
  return true;
}

bool put_wide(uint8_t* base, uint32_t dst, const std::u16string& text) {
  if (!GuestPtr<char>{dst}.valid_range((text.size() + 1) * 2)) return false;
  auto* p = reinterpret_cast<be<uint16_t>*>(base + dst);
  for (size_t i = 0; i < text.size(); ++i) p[i] = static_cast<uint16_t>(text[i]);
  p[text.size()] = 0;
  return true;
}

// Bounded copy for the _sn* family: writes up to `count` units, a NUL only when there is room (MSVC semantics).
// Returns the length when it fits (len <= count) and -1 on truncation.
template <class Put>
int32_t put_counted(size_t len, uint32_t count, Put&& put_n) {
  const size_t n = std::min<size_t>(len, count);
  put_n(n, len < count);
  return len <= count ? static_cast<int32_t>(len) : -1;
}

int32_t write_counted_narrow(uint8_t* base, uint32_t dst, uint32_t count, const std::string& s) {
  if (!GuestPtr<char>{dst}.valid_range(std::min<size_t>(count, s.size() + 1)) || count == 0) return -1;
  return put_counted(s.size(), count, [&](size_t n, bool nul) {
    std::memcpy(base + dst, s.data(), n);
    if (nul) base[dst + n] = 0;
  });
}

int32_t write_counted_wide(uint8_t* base, uint32_t dst, uint32_t count, const std::u16string& s) {
  if (!GuestPtr<char>{dst}.valid_range(std::min<size_t>(count, s.size() + 1) * 2) || count == 0) return -1;
  return put_counted(s.size(), count, [&](size_t n, bool nul) {
    auto* p = reinterpret_cast<be<uint16_t>*>(base + dst);
    for (size_t i = 0; i < n; ++i) p[i] = static_cast<uint16_t>(s[i]);
    if (nul) p[n] = 0;
  });
}

}  // namespace

namespace exports {

// --- Strings ----------------------------------------------------------------------------------------------------

// RtlInitAnsiString(dest*, src*): length = strlen (clamped to 16 bits), maximum = length + 1; a null source gives
// an all-zero string.
void RtlInitAnsiString(GuestCall& c, GuestPtr<X_ANSI_STRING> dest, uint32_t src) {
  if (!dest.valid_range(sizeof(X_ANSI_STRING))) return;
  auto* d = dest.get(c.base);
  if (src == 0) {
    d->length = 0;
    d->maximum_length = 0;
    d->buffer = 0;
    return;
  }
  const uint32_t len = static_cast<uint32_t>(read_guest_string(c.base, src, kMaxStringChars).size());
  d->length = static_cast<uint16_t>(len);
  d->maximum_length = static_cast<uint16_t>(std::min<uint32_t>(len + 1, 0xFFFF));
  d->buffer = src;
}

// RtlInitUnicodeString(dest*, src*): lengths in bytes (2 per unit), maximum = length + 2.
void RtlInitUnicodeString(GuestCall& c, GuestPtr<X_UNICODE_STRING> dest, uint32_t src) {
  if (!dest.valid_range(sizeof(X_UNICODE_STRING))) return;
  auto* d = dest.get(c.base);
  if (src == 0) {
    d->length = 0;
    d->maximum_length = 0;
    d->buffer = 0;
    return;
  }
  const uint32_t len = static_cast<uint32_t>(read_guest_wstring(c.base, src, kMaxStringChars / 2).size()) * 2;
  d->length = static_cast<uint16_t>(len);
  d->maximum_length = static_cast<uint16_t>(std::min<uint32_t>(len + 2, 0xFFFF));
  d->buffer = src;
}

// RtlFreeAnsiString(str*): frees a buffer RtlUnicodeStringToAnsiString allocated; the string is cleared.
void RtlFreeAnsiString(GuestCall& c, GuestPtr<X_ANSI_STRING> str) {
  if (!str.valid_range(sizeof(X_ANSI_STRING))) return;
  auto* s = str.get(c.base);
  if (s->buffer != 0) kernel().pool_free(s->buffer);
  s->length = 0;
  s->maximum_length = 0;
  s->buffer = 0;
}

// RtlUnicodeStringToAnsiString(dest*, src*, allocate): length = src.length / 2 bytes. With `allocate` a fresh
// pool buffer of length + 1 is created; otherwise the destination buffer must hold it (STATUS_BUFFER_OVERFLOW).
uint32_t RtlUnicodeStringToAnsiString(GuestCall& c, GuestPtr<X_ANSI_STRING> dest, GuestPtr<X_UNICODE_STRING> src,
                                      uint32_t allocate) {
  if (!dest.valid_range(sizeof(X_ANSI_STRING)) || !src.valid_range(sizeof(X_UNICODE_STRING)))
    return STATUS_INVALID_PARAMETER;
  auto* d = dest.get(c.base);
  const auto* s = src.get(c.base);
  const uint32_t units = static_cast<uint16_t>(s->length) / 2;
  if (units + 1 > 0xFFFF) return STATUS_INVALID_PARAMETER;
  uint32_t buf = d->buffer;
  if (allocate != 0) {
    buf = kernel().pool_alloc(units + 1, 0);
    if (buf == 0) return STATUS_NO_MEMORY;
    d->maximum_length = static_cast<uint16_t>(units + 1);
    d->buffer = buf;
  } else if (static_cast<uint16_t>(d->maximum_length) < units) {
    return STATUS_BUFFER_OVERFLOW;
  }
  const std::u16string text = read_guest_wstring(c.base, s->buffer, units);
  std::string narrow;
  for (size_t i = 0; i < units; ++i)
    narrow.push_back(i < text.size() ? narrow_char(text[i]) : '\0');
  if (!GuestPtr<char>{buf}.valid_range(units + 1)) return STATUS_INVALID_PARAMETER;
  std::memcpy(c.base + buf, narrow.data(), units);
  if (static_cast<uint16_t>(d->maximum_length) > units) c.base[buf + units] = 0;
  d->length = static_cast<uint16_t>(units);
  return STATUS_SUCCESS;
}

// RtlMultiByteToUnicodeN(dest*, dest_max_bytes, out_bytes*, src*, src_bytes): converts min(src_bytes, max / 2) bytes
// (one unit each, Latin-1) and stores the byte count written when `out_bytes` is non-null.
uint32_t RtlMultiByteToUnicodeN(GuestCall& c, uint32_t dest, uint32_t dest_max, U32Ptr out_bytes, uint32_t src,
                                uint32_t src_bytes) {
  const uint32_t n = std::min(src_bytes, dest_max / 2);
  if (n != 0) {
    if (!GuestPtr<char>{dest}.valid_range(n * 2ull) || !GuestPtr<char>{src}.valid_range(n)) return STATUS_INVALID_PARAMETER;
    auto* d = reinterpret_cast<be<uint16_t>*>(c.base + dest);
    for (uint32_t i = 0; i < n; ++i) d[i] = static_cast<uint16_t>(c.base[src + i]);
  }
  if (out_bytes.valid_range(4)) *out_bytes.get(c.base) = n * 2;
  return STATUS_SUCCESS;
}

// RtlUnicodeToMultiByteN(dest*, dest_max, out_bytes*, src*, src_bytes): converts min(dest_max, src_bytes / 2) units.
uint32_t RtlUnicodeToMultiByteN(GuestCall& c, uint32_t dest, uint32_t dest_max, U32Ptr out_bytes, uint32_t src,
                                uint32_t src_bytes) {
  const uint32_t n = std::min(dest_max, src_bytes / 2);
  if (n != 0) {
    if (!GuestPtr<char>{dest}.valid_range(n) || !GuestPtr<char>{src}.valid_range(n * 2ull)) return STATUS_INVALID_PARAMETER;
    const auto* s = reinterpret_cast<const be<uint16_t>*>(c.base + src);
    for (uint32_t i = 0; i < n; ++i) c.base[dest + i] = static_cast<uint8_t>(narrow_char(static_cast<char16_t>(s[i])));
  }
  if (out_bytes.valid_range(4)) *out_bytes.get(c.base) = n;
  return STATUS_SUCCESS;
}

// --- Memory -----------------------------------------------------------------------------------------------------

// RtlCompareMemory(a, b, length) -> the number of leading bytes that match.
uint32_t RtlCompareMemory(GuestCall& c, uint32_t a, uint32_t b, uint32_t length) {
  if (length == 0 || !GuestPtr<char>{a}.valid_range(length) || !GuestPtr<char>{b}.valid_range(length)) return 0;
  uint32_t i = 0;
  while (i < length && c.base[a + i] == c.base[b + i]) ++i;
  return i;
}

// RtlCompareMemoryUlong(source, length, pattern) -> bytes (a multiple of 4) at the start that equal the pattern.
uint32_t RtlCompareMemoryUlong(GuestCall& c, uint32_t source, uint32_t length, uint32_t pattern) {
  const uint32_t words = length / 4;
  if (words == 0 || !GuestPtr<char>{source}.valid_range(words * 4ull)) return 0;
  const auto* p = reinterpret_cast<const be<uint32_t>*>(c.base + source);
  uint32_t i = 0;
  while (i < words && static_cast<uint32_t>(p[i]) == pattern) ++i;
  return i * 4;
}

// RtlFillMemoryUlong(destination, length, pattern): length / 4 big-endian words.
void RtlFillMemoryUlong(GuestCall& c, uint32_t dest, uint32_t length, uint32_t pattern) {
  const uint32_t words = length / 4;
  if (words == 0 || !GuestPtr<char>{dest}.valid_range(words * 4ull)) return;
  auto* p = reinterpret_cast<be<uint32_t>*>(c.base + dest);
  for (uint32_t i = 0; i < words; ++i) p[i] = pattern;
}

uint32_t RtlNtStatusToDosError(uint32_t status) {
  for (const auto& e : kStatusToDos)
    if (e.status == status) return e.dos;
  return kErrorMrMidNotFound;
}

// --- printf family ----------------------------------------------------------------------------------------------

// sprintf(buf, fmt, ...): unbounded, returns the length (-1 when the destination is not mapped).
int32_t sprintf(GuestCall& c, uint32_t buf, uint32_t fmt) {
  CallArgs args(c, 2);
  const std::string s = guest_format(c.base, read_guest_string(c.base, fmt), args);
  return put_narrow(c.base, buf, s) ? static_cast<int32_t>(s.size()) : -1;
}

// _snprintf(buf, count, fmt, ...): at most `count` bytes; -1 when truncated, no NUL when exactly full.
int32_t _snprintf(GuestCall& c, uint32_t buf, uint32_t count, uint32_t fmt) {
  CallArgs args(c, 3);
  return write_counted_narrow(c.base, buf, count, guest_format(c.base, read_guest_string(c.base, fmt), args));
}

int32_t vsprintf(GuestCall& c, uint32_t buf, uint32_t fmt, uint32_t va_list) {
  VaListArgs args(c.base, va_list);
  const std::string s = guest_format(c.base, read_guest_string(c.base, fmt), args);
  return put_narrow(c.base, buf, s) ? static_cast<int32_t>(s.size()) : -1;
}

int32_t _vsnprintf(GuestCall& c, uint32_t buf, uint32_t count, uint32_t fmt, uint32_t va_list) {
  VaListArgs args(c.base, va_list);
  return write_counted_narrow(c.base, buf, count, guest_format(c.base, read_guest_string(c.base, fmt), args));
}

// swprintf(buf, fmt, ...): the 360 variant has no count; %s is wide.
int32_t swprintf(GuestCall& c, uint32_t buf, uint32_t fmt) {
  CallArgs args(c, 2);
  const std::u16string s = guest_format_wide(c.base, read_guest_wstring(c.base, fmt), args);
  return put_wide(c.base, buf, s) ? static_cast<int32_t>(s.size()) : -1;
}

// _snwprintf(buf, count, fmt, ...): count in UTF-16 units.
int32_t _snwprintf(GuestCall& c, uint32_t buf, uint32_t count, uint32_t fmt) {
  CallArgs args(c, 3);
  return write_counted_wide(c.base, buf, count, guest_format_wide(c.base, read_guest_wstring(c.base, fmt), args));
}

int32_t vswprintf(GuestCall& c, uint32_t buf, uint32_t fmt, uint32_t va_list) {
  VaListArgs args(c.base, va_list);
  const std::u16string s = guest_format_wide(c.base, read_guest_wstring(c.base, fmt), args);
  return put_wide(c.base, buf, s) ? static_cast<int32_t>(s.size()) : -1;
}

// _vscwprintf(fmt, va_list): the formatted length in units, nothing written.
int32_t _vscwprintf(GuestCall& c, uint32_t fmt, uint32_t va_list) {
  VaListArgs args(c.base, va_list);
  return static_cast<int32_t>(guest_format_wide(c.base, read_guest_wstring(c.base, fmt), args).size());
}

}  // namespace exports
}  // namespace lr::kernel

LR_KERNEL(RtlInitAnsiString);
LR_KERNEL(RtlInitUnicodeString);
LR_KERNEL(RtlFreeAnsiString);
LR_KERNEL(RtlUnicodeStringToAnsiString);
LR_KERNEL(RtlMultiByteToUnicodeN);
LR_KERNEL(RtlUnicodeToMultiByteN);
LR_KERNEL(RtlCompareMemory);
LR_KERNEL(RtlCompareMemoryUlong);
LR_KERNEL(RtlFillMemoryUlong);
LR_KERNEL(RtlNtStatusToDosError);
LR_KERNEL(sprintf);
LR_KERNEL(_snprintf);
LR_KERNEL(vsprintf);
LR_KERNEL(_vsnprintf);
LR_KERNEL(swprintf);
LR_KERNEL(_snwprintf);
LR_KERNEL(vswprintf);
LR_KERNEL(_vscwprintf);
