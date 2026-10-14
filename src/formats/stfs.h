// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/result.h"
#include "formats/reader.h"

namespace lr::formats {

struct StfsHeader {
  std::string magic;  // "CON ", "LIVE" or "PIRS"
  uint32_t content_type;
  uint32_t title_id;
  std::u16string display_name;
  bool read_only;  // read-only format: one backing block per hash table instead of two
};

struct StfsEntry {
  std::string path;  // full path, '/'-separated
  bool is_dir;
  uint32_t size;
  uint32_t start_block;
  bool consecutive;
};

// Read-only view of an STFS package. Borrows the IReader passed to open(): the caller must keep it
// alive for the lifetime of the package and of any reader from open_file(). Only files whose data
// blocks are consecutive are supported.
class StfsPackage {
 public:
  static Result<StfsPackage> open(IReader& reader);

  const StfsHeader& header() const { return header_; }
  const std::vector<StfsEntry>& entries() const { return entries_; }

  // Case-insensitive lookup. The returned reader borrows the package's IReader and maps reads
  // block by block (hash-table blocks interleave with data blocks).
  Result<std::unique_ptr<IReader>> open_file(std::string_view path) const;

  // Byte offset of data block `block` from the package start.
  static uint64_t block_to_offset(uint32_t block, uint32_t header_size, bool read_only);

 private:
  StfsPackage(IReader* reader, StfsHeader header, uint32_t header_size)
      : reader_(reader), header_(std::move(header)), header_size_(header_size) {}

  IReader* reader_;
  StfsHeader header_;
  uint32_t header_size_;
  std::vector<StfsEntry> entries_;
};

}  // namespace lr::formats
