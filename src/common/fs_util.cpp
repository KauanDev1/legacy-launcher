// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/fs_util.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <string>

namespace lr {
namespace fs = std::filesystem;

namespace {
Error io_error(const std::string& what, const fs::path& p, int err) {
  return Error{ErrorCode::IoError, what + ": " + p.string() + ": " + std::strerror(err), ""};
}

Result<void> fsync_path(const fs::path& p) {
  int fd = ::open(p.c_str(), O_RDONLY);
  if (fd < 0) return io_error("cannot open for fsync", p, errno);
  int rc = ::fsync(fd);
  int err = errno;
  ::close(fd);
  if (rc != 0) return io_error("fsync failed", p, err);
  return {};
}

Result<void> prepare(const fs::path& dst) {
  if (dst.has_parent_path()) {
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    if (ec) return Error{ErrorCode::IoError, "cannot create directory " +
                                                 dst.parent_path().string() + ": " + ec.message(), ""};
  }
  return {};
}

Result<void> commit(const fs::path& tmp, const fs::path& dst) {
  auto s = fsync_path(tmp);
  if (!s) { std::error_code ec; fs::remove(tmp, ec); return s; }
  std::error_code ec;
  fs::rename(tmp, dst, ec);
  if (ec) {
    std::error_code ec2;
    fs::remove(tmp, ec2);
    return Error{ErrorCode::IoError, "rename failed: " + dst.string() + ": " + ec.message(), ""};
  }
  return {};
}
}  // namespace

Result<void> atomic_write(const fs::path& path, std::span<const std::byte> data) {
  return atomic_copy_from(path, [&](std::ostream& os) -> Result<void> {
    os.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return {};
  });
}

Result<void> atomic_copy_from(const fs::path& dst,
                              const std::function<Result<void>(std::ostream&)>& writer) {
  LR_TRY(prepare(dst));
  fs::path tmp = dst;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return io_error("cannot create", tmp, errno);
    auto r = writer(out);
    if (r) {
      out.flush();
      if (!out) r = io_error("write failed", tmp, errno);
    }
    if (!r) {
      out.close();
      std::error_code ec;
      fs::remove(tmp, ec);
      return r;
    }
    out.close();
    if (out.fail()) {
      std::error_code ec;
      fs::remove(tmp, ec);
      return io_error("close failed", tmp, errno);
    }
  }
  return commit(tmp, dst);
}

}  // namespace lr
