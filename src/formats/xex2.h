// SPDX-License-Identifier: MIT
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

// Thread-local storage (optional header 0x00020104; xex2_opt_tls_info in Xenia's xex2_info.h).
struct TlsInfo {
  uint32_t slot_count;
  uint32_t raw_data_address;
  uint32_t data_size;
  uint32_t raw_data_size;
};

struct Xex2Info {
  // One import library of optional header 0x000103FF: its name (string table entry at the library's name index)
  // and the guest addresses of its record slots, in file order. In the image each slot holds its record value,
  // type << 24 | ordinal (type 0 = variable/IAT slot, 1 = function thunk), until a loader rewrites it.
  struct ImportLibrary {
    std::string name;
    std::vector<uint32_t> records;
  };

  uint32_t module_flags;
  uint32_t pe_data_offset;
  std::optional<uint32_t> entry_point;
  std::optional<uint32_t> image_base;
  std::optional<Xex2ExecutionInfo> execution;
  std::vector<std::string> import_libraries;  // the import string table, in order
  std::vector<ImportLibrary> imports;          // one per library record that follows the string table
  std::optional<TlsInfo> tls;
  std::optional<uint32_t> default_stack_size;  // optional header 0x00020200 (inline)
};
using Xex2File = Xex2Info;

// Parses the XEX2 header for inspection only (no decryption/decompression). `header_bytes` holds
// the start of the file (at least the first pe_data_offset bytes, or the whole file); every
// optional-header offset is bounds-checked against its size. Unknown optional keys are ignored;
// when a key appears more than once, the last occurrence wins.
Result<Xex2Info> parse_xex2(std::span<const std::byte> header_bytes);

// "major.minor.build.qfe" from bits 31-28, 27-24, 23-8, 7-0.
std::string format_version(uint32_t version);

}  // namespace lr::formats
