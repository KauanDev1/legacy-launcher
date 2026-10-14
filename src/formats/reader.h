// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include "common/result.h"

namespace lr::formats {

// Random-access byte source. Reads past size() fail with ErrorCode::OutOfRange.
struct IReader {
  virtual ~IReader();
  virtual Result<void> read(uint64_t offset, std::span<std::byte> dst) = 0;
  virtual uint64_t size() const = 0;
};

// File-backed reader using pread.
class FileReader : public IReader {
 public:
  static Result<std::unique_ptr<FileReader>> open(const std::filesystem::path& path);
  ~FileReader() override;
  FileReader(const FileReader&) = delete;
  FileReader& operator=(const FileReader&) = delete;
  Result<void> read(uint64_t offset, std::span<std::byte> dst) override;
  uint64_t size() const override { return size_; }

 private:
  FileReader(int fd, uint64_t size) : fd_(fd), size_(size) {}
  int fd_;
  uint64_t size_;
};

// Reader over caller-owned memory (must outlive the reader).
class SpanReader : public IReader {
 public:
  explicit SpanReader(std::span<const std::byte> data) : data_(data) {}
  Result<void> read(uint64_t offset, std::span<std::byte> dst) override;
  uint64_t size() const override { return data_.size(); }

 private:
  std::span<const std::byte> data_;
};

// A window [offset, offset+size) of a parent reader. Borrows the parent: the caller must keep it
// alive for as long as the SubReader is used. Construction does not validate the window; reads do.
class SubReader : public IReader {
 public:
  SubReader(IReader& parent, uint64_t offset, uint64_t size)
      : parent_(&parent), offset_(offset), size_(size) {}
  Result<void> read(uint64_t offset, std::span<std::byte> dst) override;
  uint64_t size() const override { return size_; }

 private:
  IReader* parent_;
  uint64_t offset_;
  uint64_t size_;
};

Result<std::vector<std::byte>> read_all(IReader& reader);

}  // namespace lr::formats
