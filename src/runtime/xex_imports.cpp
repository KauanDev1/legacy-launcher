// SPDX-License-Identifier: MIT
#include "runtime/xex_imports.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "image.h"

// Ordinal -> "__imp__<name>" tables defined (without a header declaration) in XenonUtils' xex.cpp; ParseImage
// names its import symbols from them.
extern std::unordered_map<size_t, const char*> XboxKernelExports;
extern std::unordered_map<size_t, const char*> XamExports;

namespace lr::runtime {

namespace {
std::string hex(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08X", v);
  return b;
}
}  // namespace

std::string_view kernel_export_name(std::string_view library, uint32_t ordinal) {
  const std::unordered_map<size_t, const char*>* table = nullptr;
  if (library == "xboxkrnl.exe") table = &XboxKernelExports;
  else if (library == "xam.xex") table = &XamExports;
  if (table == nullptr) return {};
  const auto it = table->find(ordinal);
  if (it == table->end()) return {};
  std::string_view n = it->second;
  constexpr std::string_view kPrefix = "__imp__";
  if (n.substr(0, kPrefix.size()) == kPrefix) n.remove_prefix(kPrefix.size());
  return n;
}

bool is_import_thunk(const uint8_t* p) {
  // nop (ori r0,r0,0 = 0x60000000) x3, blr (0x4E800020), big-endian in guest memory.
  static constexpr uint8_t kThunk[16] = {0x60, 0, 0, 0, 0x60, 0, 0, 0, 0x60, 0, 0, 0, 0x4E, 0x80, 0x00, 0x20};
  return std::memcmp(p, kThunk, sizeof kThunk) == 0;
}

Result<std::vector<ImportRecord>> classify_imports(const formats::Xex2File& xex, const ImageReader& read) {
  std::vector<ImportRecord> out;
  for (const auto& lib : xex.imports) {
    auto thunk_at = [&](uint32_t addr) {
      uint8_t b[16];
      return read(addr, b, sizeof b) && is_import_thunk(b);
    };
    for (size_t i = 0; i < lib.records.size(); ++i) {
      const uint32_t slot = lib.records[i];
      if (thunk_at(slot)) continue;  // a type-1 record, paired with the type-0 record before it
      uint32_t v = 0;
      if (!read(slot, &v, sizeof v)) {
        return Error{ErrorCode::InvalidFormat, "import record " + hex(slot) + " of " + lib.name + " is outside the image", ""};
      }
      // Host order: ParseImage already byte-swapped the slot (see the header comment).
      if ((v >> 24) != 0) {
        return Error{ErrorCode::InvalidFormat,
                     "import record " + hex(slot) + " of " + lib.name + " has type " + std::to_string(v >> 24), ""};
      }
      const auto ordinal = static_cast<uint16_t>(v & 0xFFFF);
      const bool paired = i + 1 < lib.records.size() && thunk_at(lib.records[i + 1]);
      out.push_back({lib.name, slot, ordinal, std::string(kernel_export_name(lib.name, ordinal)),
                     paired ? lib.records[i + 1] : 0});
    }
  }
  return out;
}

Result<std::vector<ImportRecord>> list_imports(std::span<const std::byte> xex) {
  if (xex.size() < 4 || xex[0] != std::byte{'X'} || xex[1] != std::byte{'E'} || xex[2] != std::byte{'X'} ||
      xex[3] != std::byte{'2'})
    return Error{ErrorCode::InvalidFormat, "not an XEX2 file", ""};
  LR_TRY_ASSIGN(header, formats::parse_xex2(xex));
  const Image image = Image::ParseImage(reinterpret_cast<const uint8_t*>(xex.data()), xex.size());
  if (!image.data || image.sections.empty())
    return Error{ErrorCode::InvalidFormat, "XenonUtils could not parse the image", "expected a patched, decrypted XEX2"};
  const ImageReader read = [&image](uint32_t addr, void* dst, size_t n) {
    for (const Section& s : image.sections) {
      if (addr >= s.base && uint64_t{addr} + n <= uint64_t{s.base} + s.size && s.data != nullptr) {
        std::memcpy(dst, s.data + (addr - s.base), n);
        return true;
      }
    }
    return false;
  };
  return classify_imports(header, read);
}

}  // namespace lr::runtime
