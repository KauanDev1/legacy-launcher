// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/result.h"
#include "formats/xex2.h"

// Import records of a loaded XEX2, as they look AFTER XenonUtils' Image::ParseImage (which our loader uses):
// ParseImage byte-swaps every record slot in place, so a type-0 slot (variable / IAT slot) holds its record value
// in host order (type = v >> 24, ordinal = v & 0xFFFF), and replaces every type-1 record (function thunk) with
// the 16-byte thunk nop, nop, nop, blr, losing its value. Following Xenia (XexModule::SetupLibraryImports), a
// function's type-1 record directly follows the type-0 record of the same ordinal; that is how a thunk is paired
// with its import here.

namespace lr::runtime {

struct ImportRecord {
  std::string library;  // e.g. "xboxkrnl.exe"
  uint32_t slot;        // guest address of the type-0 record
  uint16_t ordinal;
  std::string name;  // export name (without "__imp__"); empty when the ordinal is not in XenonUtils' tables
  uint32_t thunk;    // function import: guest address of its thunk (type-1 record); variable import: 0
};

// Copies `n` bytes of the loaded image at guest `addr` into `out`; false when the range is not loaded.
using ImageReader = std::function<bool(uint32_t addr, void* out, size_t n)>;

// The export name of `ordinal` in `library` (xboxkrnl.exe or xam.xex) from XenonUtils' tables, or "".
std::string_view kernel_export_name(std::string_view library, uint32_t ordinal);

// True when the 16 bytes at `p` are the thunk ParseImage writes over a type-1 record.
bool is_import_thunk(const uint8_t* p);

// One ImportRecord per type-0 record, in file order. InvalidFormat when a record slot is not in the image or a
// slot that is no thunk holds a type other than 0.
Result<std::vector<ImportRecord>> classify_imports(const formats::Xex2File& xex, const ImageReader& read);

// Loads `xex` with Image::ParseImage, parses its header and classifies its imports (inspect --imports, --coverage).
Result<std::vector<ImportRecord>> list_imports(std::span<const std::byte> xex);

}  // namespace lr::runtime
