// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/result.h"

namespace lr::formats {

struct Xex2ExecutionInfo {
  uint32_t media_id;
  uint32_t version;
  uint32_t base_version;
  uint32_t title_id;
  uint8_t platform;
  uint8_t executable_type;
  uint8_t disc_number;
  uint8_t disc_count;
};

struct Xex2Info {
  uint32_t module_flags;
  uint32_t pe_data_offset;
  std::optional<uint32_t> entry_point;
  std::optional<uint32_t> image_base;
  std::optional<Xex2ExecutionInfo> execution;
  std::vector<std::string> import_libraries;
};

// Parses the XEX2 header for inspection only (no decryption/decompression). `header_bytes` holds
// the start of the file (at least the first pe_data_offset bytes, or the whole file); every
// optional-header offset is bounds-checked against its size. Unknown optional keys are ignored;
// when a key appears more than once, the last occurrence wins.
Result<Xex2Info> parse_xex2(std::span<const std::byte> header_bytes);

// "major.minor.build.qfe" from bits 31-28, 27-24, 23-8, 7-0.
std::string format_version(uint32_t version);

}  // namespace lr::formats
