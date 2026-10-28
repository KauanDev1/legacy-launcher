// SPDX-License-Identifier: MIT
// XEX queries over the guest copy of the executable's header and the module table. Semantics follow Xenia
// (xboxkrnl_rtl.cc RtlImageXexHeaderField, xboxkrnl_modules.cc XexGetModuleHandle / XexCheckExecutablePrivilege /
// XexGetModuleSection / XexGetProcedureAddress, xam_info.cc XamLoader*); semantic reference only, no code copied.
//
// Deviations: XexGetProcedureAddress resolves only ordinals of the xboxkrnl.exe / xam.xex system modules, and only
// those the executable imports (the thunk exists); a by-name lookup or the executable's own exports are "not found".
// XexLoadImage only "loads" modules that are already loaded. XamLoaderLaunchTitle lives in xam_ui.cpp (guest_fatal).
#include <strings.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "runtime/be.h"
#include "runtime/kernel/guest_printf.h"
#include "runtime/kernel/guest_structs.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/marshal.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/status.h"
#include "runtime/kernel/trace.h"

namespace lr::kernel {
namespace {

using U32Ptr = GuestPtr<be<uint32_t>>;

constexpr uint32_t kXexHeaderSystemFlags = 0x00030000;
constexpr uint32_t kXexHeaderExecutionInfo = 0x00040006;
constexpr uint32_t kXexHeaderResourceInfo = 0x000002FF;
constexpr uint32_t kStatusDriverEntrypointNotFound = 0xC0000263;
constexpr uint32_t kErrorInvalidParameter = 87;
constexpr uint32_t kMaxOptionalHeaders = 256;

// XEX2 header: magic, module flags, pe data offset, reserved, security offset, then the optional header count at
// 0x14 followed by {key, value} pairs of big-endian words.
constexpr uint32_t kCountOffset = 0x14;
constexpr uint32_t kTableOffset = 0x18;

// The header field `key` of the XEX header at guest `base`: for keys whose low byte is 0x00 or 0x01 the value
// itself, otherwise a guest pointer to the data (header base + the stored offset). false when absent.
bool header_field(const GuestCall& c, uint32_t base, uint32_t key, uint32_t& out) {
  if (!GuestPtr<char>{base}.valid_range(kTableOffset)) return false;
  const uint32_t count = std::min<uint32_t>(*reinterpret_cast<const be<uint32_t>*>(c.base + base + kCountOffset),
                                            kMaxOptionalHeaders);
  if (!GuestPtr<char>{base}.valid_range(kTableOffset + uint64_t{count} * 8)) return false;
  const auto* table = reinterpret_cast<const be<uint32_t>*>(c.base + base + kTableOffset);
  for (uint32_t i = 0; i < count; ++i) {
    if (static_cast<uint32_t>(table[2 * i]) != key) continue;
    const uint32_t value = table[2 * i + 1];
    const uint32_t low = key & 0xFF;
    out = (low == 0x00 || low == 0x01) ? value : base + value;
    return true;
  }
  return false;
}

bool is_executable(uint32_t handle) {
  const uint32_t exe = kernel().executable_module();
  return exe != 0 && (handle == 0 || handle == exe);
}

void put_u32(const GuestCall& c, U32Ptr p, uint32_t v) {
  if (p.valid_range(4)) *p.get(c.base) = v;
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return s;
}

}  // namespace

namespace exports {

// RtlImageXexHeaderField(header*, key): see header_field; 0 when the key is absent.
uint32_t RtlImageXexHeaderField(GuestCall& c, uint32_t header, uint32_t key) {
  uint32_t v = 0;
  return header_field(c, header, key, v) ? v : 0;
}

// XexGetModuleHandle(name*, out*): NULL or "default.xex" is the executable; "xboxkrnl.exe" / "xam.xex" are system
// modules with fake LDR entries. Anything else: *out = 0, ERROR_NOT_FOUND.
uint32_t XexGetModuleHandle(GuestCall& c, uint32_t name, U32Ptr out) {
  uint32_t handle = 0;
  if (name == 0) {
    handle = kernel().executable_module();
  } else {
    const std::string n = lower(read_guest_string(c.base, name, 260));
    handle = n == "default.xex" ? kernel().executable_module() : kernel().system_module(n);
  }
  put_u32(c, out, handle);
  return handle != 0 ? ERROR_SUCCESS : ERROR_NOT_FOUND;
}

// XexLoadImage(name*, flags, min_version, handle_out*): every module this runtime has is already loaded (the
// executable and the xboxkrnl.exe / xam.xex system modules), so a load of one of those returns its handle, as Xenia
// does for a loaded module. Loading another image from disk is a feature this runtime lacks (M5): *handle_out = 0,
// STATUS_NO_SUCH_FILE, the status Xenia returns when the module cannot be loaded.
uint32_t XexLoadImage(GuestCall& c, uint32_t name, uint32_t, uint32_t, U32Ptr handle_out) {
  const std::string n = lower(read_guest_string(c.base, name, 260));
  const uint32_t handle = n == "default.xex" ? kernel().executable_module() : kernel().system_module(n);
  put_u32(c, handle_out, handle);
  return handle != 0 ? STATUS_SUCCESS : STATUS_NO_SUCH_FILE;
}

// XexUnloadImage(handle): loaded modules are never unloaded (system modules cannot be, and XexLoadImage only hands
// out modules that stay loaded); STATUS_INVALID_HANDLE for anything else.
uint32_t XexUnloadImage(uint32_t handle) {
  if (handle == 0) return STATUS_INVALID_HANDLE;
  return is_executable(handle) || !kernel().system_module_name(handle).empty() ? STATUS_SUCCESS
                                                                              : STATUS_INVALID_HANDLE;
}

// XexCheckExecutablePrivilege(privilege): bit `privilege` of the system flags header; 0 without the header.
uint32_t XexCheckExecutablePrivilege(GuestCall& c, uint32_t privilege) {
  if (privilege >= 32) return 0;
  uint32_t flags = 0;
  if (!header_field(c, kernel().executable_header(), kXexHeaderSystemFlags, flags)) return 0;
  return (flags >> privilege) & 1;
}

// XexGetModuleSection(module, name*, data_out*, size_out*): a resource of the executable (resource info header:
// u32 size, then {char name[8], u32 address, u32 size}). STATUS_INVALID_HANDLE for a module that is not the
// executable, STATUS_NOT_FOUND for an unknown section.
uint32_t XexGetModuleSection(GuestCall& c, uint32_t module, uint32_t name, U32Ptr data_out, U32Ptr size_out) {
  if (module == 0 || !is_executable(module)) return STATUS_INVALID_HANDLE;
  uint32_t res = 0;
  if (!header_field(c, kernel().executable_header(), kXexHeaderResourceInfo, res) ||
      !GuestPtr<char>{res}.valid_range(4))
    return STATUS_NOT_FOUND;
  const std::string wanted = read_guest_string(c.base, name, 8);
  const uint32_t size = *reinterpret_cast<const be<uint32_t>*>(c.base + res);
  const uint32_t entries = size >= 4 ? (size - 4) / 16 : 0;
  if (!GuestPtr<char>{res}.valid_range(4 + uint64_t{entries} * 16)) return STATUS_NOT_FOUND;
  for (uint32_t i = 0; i < entries; ++i) {
    const uint8_t* e = c.base + res + 4 + i * 16;
    const std::string n(reinterpret_cast<const char*>(e), strnlen(reinterpret_cast<const char*>(e), 8));
    if (n != wanted) continue;
    put_u32(c, data_out, *reinterpret_cast<const be<uint32_t>*>(e + 8));
    put_u32(c, size_out, *reinterpret_cast<const be<uint32_t>*>(e + 12));
    return STATUS_SUCCESS;
  }
  return STATUS_NOT_FOUND;
}

// XexGetProcedureAddress(module, ordinal, out*): the thunk of `ordinal` in a system module (see the file note).
uint32_t XexGetProcedureAddress(GuestCall& c, uint32_t module, uint32_t ordinal, U32Ptr out) {
  const std::string lib = kernel().system_module_name(module);
  if (lib.empty()) {
    if (module == 0 || is_executable(module)) {
      put_u32(c, out, 0);
      return kStatusDriverEntrypointNotFound;
    }
    return STATUS_INVALID_HANDLE;
  }
  uint32_t addr = 0;
  if ((ordinal >> 16) == 0) {
    const auto it = kernel().import_thunks().find({lib, static_cast<uint16_t>(ordinal)});
    if (it != kernel().import_thunks().end()) addr = it->second;
  }
  put_u32(c, out, addr);
  return addr != 0 ? STATUS_SUCCESS : kStatusDriverEntrypointNotFound;
}

// XamGetExecutionId(out*): pointer to the execution-info header data in the guest header copy.
uint32_t XamGetExecutionId(GuestCall& c, U32Ptr out) {
  uint32_t p = 0;
  if (!header_field(c, kernel().executable_header(), kXexHeaderExecutionInfo, p)) return STATUS_NOT_FOUND;
  put_u32(c, out, p);
  return STATUS_SUCCESS;
}

// XamLoaderGetLaunchDataSize(size_out*): there is never launch data (a plain disc launch), as Xenia without one.
uint32_t XamLoaderGetLaunchDataSize(GuestCall& c, U32Ptr size_out) {
  if (!size_out) return kErrorInvalidParameter;
  put_u32(c, size_out, 0);
  return ERROR_NOT_FOUND;
}

uint32_t XamLoaderGetLaunchData(uint32_t, uint32_t) { return ERROR_NOT_FOUND; }

// XamLoaderTerminateTitle(): the title asks to end; exit code 0 (the import trace is flushed first).
void XamLoaderTerminateTitle() {
  std::fflush(nullptr);
  flush_at_exit();
  _exit(0);
}

}  // namespace exports

LR_KERNEL(RtlImageXexHeaderField);
LR_KERNEL(XexGetModuleHandle);
LR_KERNEL(XexCheckExecutablePrivilege);
LR_KERNEL(XexGetModuleSection);
LR_KERNEL(XexGetProcedureAddress);
LR_KERNEL(XexLoadImage);
LR_KERNEL(XexUnloadImage);
LR_KERNEL(XamGetExecutionId);
LR_KERNEL(XamLoaderGetLaunchDataSize);
LR_KERNEL(XamLoaderGetLaunchData);
LR_KERNEL(XamLoaderTerminateTitle);

}  // namespace lr::kernel
