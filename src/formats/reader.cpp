// SPDX-License-Identifier: GPL-3.0-or-later
#include "formats/reader.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace lr::formats {

namespace {
bool in_range(uint64_t offset, size_t len, uint64_t size) {
  return offset <= size && len <= size - offset;
}
Error out_of_range(uint64_t offset, size_t len, uint64_t size) {
  return Error{ErrorCode::OutOfRange,
               "read of " + std::to_string(len) + " bytes at offset " + std::to_string(offset) +
                   " exceeds size " + std::to_string(size),
               "the file is truncated or corrupt"};
}
}  // namespace

IReader::~IReader() = default;

Result<std::unique_ptr<FileReader>> FileReader::open(const std::filesystem::path& path) {
  int fd;
  do {
    fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) {
    int e = errno;
    return Error{e == ENOENT ? ErrorCode::NotFound : ErrorCode::IoError,
                 "cannot open " + path.string() + ": " + std::strerror(e), "check the path and permissions"};
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    int e = errno;
    ::close(fd);
    return Error{ErrorCode::IoError, "cannot stat " + path.string() + ": " + std::strerror(e), ""};
  }
  return std::unique_ptr<FileReader>(new FileReader(fd, static_cast<uint64_t>(st.st_size)));
}

FileReader::~FileReader() {
  if (fd_ >= 0) ::close(fd_);
}

Result<void> FileReader::read(uint64_t offset, std::span<std::byte> dst) {
  if (!in_range(offset, dst.size(), size_)) return out_of_range(offset, dst.size(), size_);
  size_t done = 0;
  while (done < dst.size()) {
    ssize_t n = ::pread(fd_, dst.data() + done, dst.size() - done, static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      return Error{ErrorCode::IoError, std::string("read failed: ") + std::strerror(errno), ""};
    }
    if (n == 0) return out_of_range(offset, dst.size(), offset + done);  // EOF before dst filled
    done += static_cast<size_t>(n);
  }
  return {};
}

Result<void> SpanReader::read(uint64_t offset, std::span<std::byte> dst) {
  if (!in_range(offset, dst.size(), data_.size())) return out_of_range(offset, dst.size(), data_.size());
  if (!dst.empty()) std::memcpy(dst.data(), data_.data() + offset, dst.size());
  return {};
}

Result<void> SubReader::read(uint64_t offset, std::span<std::byte> dst) {
  if (!in_range(offset, dst.size(), size_)) return out_of_range(offset, dst.size(), size_);
  return parent_->read(offset_ + offset, dst);
}

Result<std::vector<std::byte>> read_all(IReader& reader) {
  uint64_t n = reader.size();
  if (n > (uint64_t{1} << 32))
    return Error{ErrorCode::InsufficientResources, "refusing to read " + std::to_string(n) + " bytes into memory", ""};
  std::vector<std::byte> out(static_cast<size_t>(n));
  LR_TRY(reader.read(0, out));
  return out;
}

}  // namespace lr::formats
