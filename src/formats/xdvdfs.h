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

struct XdvdfsEntry {
  std::string name;
  bool is_dir;
  uint32_t sector;  // relative to the partition start
  uint32_t size;
};

// Read-only view of an Xbox 360 XDVDFS disc image. Borrows the IReader passed to open(): the
// caller must keep it alive for the lifetime of the image and of any reader from open_file().
class XdvdfsImage {
 public:
  static Result<XdvdfsImage> open(IReader& reader);

  uint64_t partition_offset() const { return partition_; }

  // Paths use '/', lookup is case-insensitive; "" and "/" are the root.
  Result<std::vector<XdvdfsEntry>> list(std::string_view dir_path) const;
  // The returned reader borrows this image's IReader.
  Result<std::unique_ptr<IReader>> open_file(std::string_view path) const;

 private:
  XdvdfsImage(IReader* reader, uint64_t partition, uint32_t root_sector, uint32_t root_size)
      : reader_(reader), partition_(partition), root_sector_(root_sector), root_size_(root_size) {}
  Result<std::vector<XdvdfsEntry>> read_dir(uint32_t sector, uint32_t size) const;
  Result<XdvdfsEntry> find(std::string_view path) const;

  IReader* reader_;
  uint64_t partition_;
  uint32_t root_sector_;
  uint32_t root_size_;
};

}  // namespace lr::formats
