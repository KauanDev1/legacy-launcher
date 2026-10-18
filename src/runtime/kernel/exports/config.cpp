// SPDX-License-Identifier: MIT
// Console configuration queries. Values follow Xenia (xboxkrnl_xconfig.cc ExGetXConfigSetting, xam_info.cc
// XGetGameRegion / XamGetSystemVersion); semantic reference only, no code copied. The fixed values are documented in
// docs/architecture.md. Deviation: the language comes from the host locale (KernelConfig::language) instead of
// Xenia's cvar.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "runtime/be.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/marshal.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/status.h"

namespace lr::kernel {
namespace {

using U32Ptr = GuestPtr<be<uint32_t>>;

constexpr uint32_t kStatusInvalidParameter1 = 0xC00000EF;
constexpr uint32_t kStatusInvalidParameter2 = 0xC00000F0;
constexpr uint32_t kStatusInvalidParameter3 = 0xC00000F1;

constexpr uint32_t kCategorySecured = 0x0002;
constexpr uint32_t kCategoryUser = 0x0003;
constexpr uint32_t kTitleId = 0x584111F7;  // Minecraft: Xbox 360 Edition

// "pt_BR.UTF-8" -> XLanguage (en 1, ja 2, de 3, fr 4, es 5, it 6, ko 7, zh-Hant 8, pt 9, zh-Hans 10, pl 11,
// ru 12); anything unknown is English.
uint32_t language_id(std::string_view locale) {
  std::string l(locale);
  std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const size_t cut = l.find_first_of("_-.@");
  const std::string lang = l.substr(0, cut);
  if (lang == "zh") {
    std::string region;
    if (cut != std::string::npos && (l[cut] == '_' || l[cut] == '-')) {
      const size_t end = l.find_first_of(".@", cut + 1);
      region = l.substr(cut + 1, end == std::string::npos ? end : end - cut - 1);
    }
    return (region == "tw" || region == "hk" || region == "hant") ? 8 : 10;
  }
  if (lang == "en") return 1;
  if (lang == "ja") return 2;
  if (lang == "de") return 3;
  if (lang == "fr") return 4;
  if (lang == "es") return 5;
  if (lang == "it") return 6;
  if (lang == "ko") return 7;
  if (lang == "pt") return 9;
  if (lang == "pl") return 11;
  if (lang == "ru") return 12;
  return 1;
}

}  // namespace

namespace exports {

uint32_t XGetLanguage() { return language_id(kernel().config().language); }

// Xenia: a stub returning 0xFFFF (region free).
uint32_t XGetGameRegion() { return 0xFFFF; }

uint32_t XamGetCurrentTitleId() { return kTitleId; }

// Xenia: a stub returning 0. The title only checks that the call succeeds.
uint32_t XamGetSystemVersion() { return 0; }

// ExGetXConfigSetting(category, setting, buffer*, buffer_size, required_size*): Xenia's table. A null buffer is a
// size query (buffer_size must then be 0). *required_size is written on success only (0 otherwise, as Xenia).
uint32_t ExGetXConfigSetting(GuestCall& c, uint16_t category, uint16_t setting, uint32_t buffer, uint16_t buffer_size,
                             U32Ptr required_size) {
  uint8_t value[4] = {0, 0, 0, 0};
  uint16_t size = 0;
  auto put32 = [&](uint32_t v) {
    value[0] = static_cast<uint8_t>(v >> 24);
    value[1] = static_cast<uint8_t>(v >> 16);
    value[2] = static_cast<uint8_t>(v >> 8);
    value[3] = static_cast<uint8_t>(v);
    size = 4;
  };
  uint32_t status = STATUS_SUCCESS;
  if (category == kCategorySecured) {
    if (setting == 0x0002) put32(0x00001000);  // AV region
    else status = kStatusInvalidParameter2;
  } else if (category == kCategoryUser) {
    switch (setting) {
      case 0x0001: case 0x0002: case 0x0003: case 0x0004: case 0x0005: case 0x0006: case 0x0007:  // time zone
      case 0x000C:                                                                                // retail flags
        put32(0);
        break;
      case 0x0009: put32(language_id(kernel().config().language)); break;  // USER_LANGUAGE
      case 0x000A: put32(0x00040000); break;                                // USER_VIDEO_FLAGS
      case 0x000E: value[0] = 103; size = 1; break;                         // USER_COUNTRY
      default: status = kStatusInvalidParameter2;
    }
  } else {
    status = kStatusInvalidParameter1;
  }
  uint32_t required = 0;
  if (status == STATUS_SUCCESS) {
    if (buffer != 0) {
      if (buffer_size < size) status = STATUS_BUFFER_TOO_SMALL;
      else if (!GuestPtr<char>{buffer}.valid_range(size)) status = kStatusInvalidParameter3;
      else std::memcpy(c.base + buffer, value, size);
    } else if (buffer_size != 0) {
      status = kStatusInvalidParameter3;
    }
    if (status == STATUS_SUCCESS) required = size;
  }
  if (required_size.valid_range(4)) *required_size.get(c.base) = required;
  return status;
}

}  // namespace exports

LR_KERNEL(XGetLanguage);
LR_KERNEL(XGetGameRegion);
LR_KERNEL(XamGetCurrentTitleId);
LR_KERNEL(XamGetSystemVersion);
LR_KERNEL(ExGetXConfigSetting);

}  // namespace lr::kernel
