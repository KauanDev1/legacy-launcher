// SPDX-License-Identifier: MIT
#include "formats/xex2.h"

#include <new>

namespace lr::formats {

namespace {
constexpr size_t kFixedHeader = 0x18;  // magic .. optional header count
constexpr uint32_t kKeyEntryPoint = 0x00010100;
constexpr uint32_t kKeyImageBase = 0x00010201;
constexpr uint32_t kKeyImportLibraries = 0x000103FF;
constexpr uint32_t kKeyExecutionInfo = 0x00040006;
constexpr uint32_t kKeyTlsInfo = 0x00020104;           // 4 u32 (xex2_opt_tls_info)
constexpr uint32_t kKeyDefaultStackSize = 0x00020200;  // inline
constexpr size_t kExecutionInfoSize = 24;

uint32_t be32(const std::byte* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
Error invalid(std::string msg) {
  return Error{ErrorCode::InvalidFormat, std::move(msg), "the XEX2 header is corrupt"};
}
Error truncated(std::string msg) {
  return Error{ErrorCode::OutOfRange, std::move(msg), "the XEX2 header is truncated"};
}

Result<Xex2ExecutionInfo> parse_execution(const std::byte* p) {
  Xex2ExecutionInfo e{};
  e.media_id = be32(p);
  e.version = be32(p + 4);
  e.base_version = be32(p + 8);
  e.title_id = be32(p + 12);
  e.platform = static_cast<uint8_t>(p[16]);
  e.executable_type = static_cast<uint8_t>(p[17]);
  e.disc_number = static_cast<uint8_t>(p[18]);
  e.disc_count = static_cast<uint8_t>(p[19]);
  return e;
}

uint16_t be16(const std::byte* p) {
  return static_cast<uint16_t>((static_cast<uint32_t>(p[0]) << 8) | static_cast<uint32_t>(p[1]));
}

// Import library record header (xex2_import_library in Xenia's xex2_info.h): size, next import digest[0x14], id,
// version, min version, u16 name index, u16 record count, then `count` u32 record addresses.
constexpr size_t kImportLibraryHeader = 0x28;

// `block` spans the whole import-libraries block (already bounds-checked against the buffer): a 12-byte header
// (block size, string table size, library count), the string table, then the library records back to back.
Result<void> parse_imports(std::span<const std::byte> block, Xex2Info& info) {
  if (block.size() < 12) return truncated("import libraries block too small");
  const uint32_t table_size = be32(block.data() + 4);
  const uint32_t count = be32(block.data() + 8);
  if (table_size > block.size() - 12) return truncated("import string table runs past its block");
  const std::byte* table = block.data() + 12;
  std::vector<std::string> names;
  std::vector<Xex2Info::ImportLibrary> libs;
  try {
    size_t pos = 0;
    while (names.size() < count && pos < table_size) {
      size_t end = pos;
      while (end < table_size && table[end] != std::byte{0}) ++end;
      if (end == table_size) return invalid("unterminated import library name");
      names.emplace_back(reinterpret_cast<const char*>(table + pos), end - pos);
      pos = (end + 1 + 3) & ~size_t{3};  // NUL, then pad to 4 bytes
    }
    // Library records: read while a whole header fits (header-only test inputs may carry none).
    size_t off = 12 + size_t{table_size};
    while (libs.size() < count && block.size() - off >= kImportLibraryHeader) {
      const std::byte* l = block.data() + off;
      const uint16_t name_index = be16(l + 0x24);
      const uint16_t n = be16(l + 0x26);
      const size_t size = kImportLibraryHeader + size_t{n} * 4;
      if (size > block.size() - off) return truncated("import library records run past their block");
      if (name_index >= names.size()) return invalid("import library name index out of range");
      Xex2Info::ImportLibrary lib{names[name_index], {}};
      lib.records.reserve(n);
      for (size_t i = 0; i < n; ++i) lib.records.push_back(be32(l + kImportLibraryHeader + i * 4));
      libs.push_back(std::move(lib));
      // The declared size covers the records; never step back inside them.
      const size_t declared = be32(l);
      off += declared > size && declared <= block.size() - off ? declared : size;
    }
  } catch (const std::bad_alloc&) {
    return Error{ErrorCode::InsufficientResources, "out of memory reading import libraries", ""};
  }
  info.import_libraries = std::move(names);
  info.imports = std::move(libs);
  return {};
}
}  // namespace

std::string format_version(uint32_t v) {
  return std::to_string(v >> 28) + "." + std::to_string((v >> 24) & 0xF) + "." +
         std::to_string((v >> 8) & 0xFFFF) + "." + std::to_string(v & 0xFF);
}

Result<Xex2Info> parse_xex2(std::span<const std::byte> bytes) {
  if (bytes.size() < kFixedHeader) return truncated("XEX2 header shorter than fixed fields");
  const std::byte* b = bytes.data();
  if (be32(b) != 0x58455832u) return invalid("bad XEX2 magic");

  Xex2Info info{};
  info.module_flags = be32(b + 4);
  info.pe_data_offset = be32(b + 8);
  const uint32_t count = be32(b + 0x14);
  if (count > (bytes.size() - kFixedHeader) / 8)
    return truncated("optional header count exceeds available bytes");

  for (uint32_t i = 0; i < count; ++i) {
    const std::byte* e = b + kFixedHeader + size_t{i} * 8;
    const uint32_t key = be32(e);
    const uint32_t value = be32(e + 4);
    const uint32_t low = key & 0xFF;

    if (low <= 0x01) {  // value is inline
      if (key == kKeyEntryPoint) info.entry_point = value;
      else if (key == kKeyImageBase) info.image_base = value;
      else if (key == kKeyDefaultStackSize) info.default_stack_size = value;
      continue;
    }
    if (key != kKeyImportLibraries && key != kKeyExecutionInfo && key != kKeyTlsInfo) continue;  // unknown: ignored

    const size_t off = value;
    size_t len;
    if (low == 0xFF) {
      if (off > bytes.size() || bytes.size() - off < 4)
        return truncated("optional header block offset out of range");
      len = be32(b + off);
      if (len < 4) return invalid("optional header block size too small");
    } else {
      len = size_t{low} * 4;
    }
    if (off > bytes.size() || len > bytes.size() - off)
      return truncated("optional header data runs past the buffer");

    if (key == kKeyExecutionInfo) {
      if (len < kExecutionInfoSize) return invalid("execution info too small");
      LR_TRY_ASSIGN(ex, parse_execution(b + off));
      info.execution = ex;
    } else if (key == kKeyTlsInfo) {
      const std::byte* t = b + off;  // len == 0x10: four u32 (low byte 0x04)
      info.tls = TlsInfo{be32(t), be32(t + 4), be32(t + 8), be32(t + 12)};
    } else {
      LR_TRY(parse_imports(bytes.subspan(off, len), info));
    }
  }
  return info;
}

}  // namespace lr::formats
