// SPDX-License-Identifier: GPL-3.0-or-later
#include "formats/xdvdfs.h"

#include <array>
#include <cstring>
#include <new>

namespace lr::formats {

namespace {
constexpr uint64_t kSector = 2048;
constexpr uint64_t kPartitions[] = {0x0, 0xFD90000, 0x2080000, 0x18300000};
constexpr char kMagic[20] = {'M', 'I', 'C', 'R', 'O', 'S', 'O', 'F', 'T', '*',
                             'X', 'B', 'O', 'X', '*', 'M', 'E', 'D', 'I', 'A'};
constexpr uint32_t kMaxDirSize = 64u << 20;

uint16_t u16(const std::byte* p) {
  return static_cast<uint16_t>(static_cast<unsigned>(p[0]) | (static_cast<unsigned>(p[1]) << 8));
}
uint32_t u32(const std::byte* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }
bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (lower(a[i]) != lower(b[i])) return false;
  return true;
}
Error invalid(std::string msg) { return Error{ErrorCode::InvalidFormat, std::move(msg), "the disc image is corrupt"}; }
}  // namespace

Result<XdvdfsImage> XdvdfsImage::open(IReader& reader) {
  for (uint64_t part : kPartitions) {
    uint64_t at = part + 32 * kSector;
    if (at + kSector > reader.size()) continue;
    std::array<std::byte, 0x1C> hdr{};
    LR_TRY(reader.read(at, hdr));
    if (std::memcmp(hdr.data(), kMagic, sizeof kMagic) != 0) continue;
    return XdvdfsImage(&reader, part, u32(&hdr[0x14]), u32(&hdr[0x18]));
  }
  return Error{ErrorCode::InvalidFormat, "no XDVDFS volume descriptor found at any known partition offset",
               "not an Xbox 360 disc image (expected an XISO/XGD1/XGD2/XGD3 game disc)"};
}

Result<std::vector<XdvdfsEntry>> XdvdfsImage::read_dir(uint32_t sector, uint32_t size) const {
  try {
    return read_dir_impl(sector, size);
  } catch (const std::bad_alloc&) {
    return Error{ErrorCode::InsufficientResources,
                 "out of memory while reading directory (" + std::to_string(size) + " bytes)",
                 "the disc image is corrupt or the system is low on memory"};
  }
}

Result<std::vector<XdvdfsEntry>> XdvdfsImage::read_dir_impl(uint32_t sector, uint32_t size) const {
  std::vector<XdvdfsEntry> out;
  if (size == 0) return out;
  if (size > kMaxDirSize) return invalid("directory size " + std::to_string(size) + " is implausible");
  uint64_t base = partition_ + static_cast<uint64_t>(sector) * kSector;
  if (base > reader_->size() || size > reader_->size() - base)
    return Error{ErrorCode::OutOfRange, "directory extends past end of image", "the image is truncated"};
  std::vector<std::byte> d(size);
  LR_TRY(reader_->read(base, d));

  std::vector<bool> visited(size / 4 + 1, false);
  std::vector<uint32_t> stack;
  uint32_t cur = 0;
  bool have_cur = true;
  while (have_cur || !stack.empty()) {
    while (have_cur) {
      uint64_t b = static_cast<uint64_t>(cur) * 4;
      if (b + 14 > size) return invalid("directory entry offset out of bounds");
      if (visited[cur]) return invalid("cycle in directory tree");
      visited[cur] = true;
      if (u16(&d[b]) == 0xFFFF) {  // padding / empty directory
        have_cur = false;
        break;
      }
      stack.push_back(cur);
      uint16_t left = u16(&d[b]);
      if (left == 0) {
        have_cur = false;
      } else {
        cur = left;
      }
    }
    if (stack.empty()) break;
    uint32_t node = stack.back();
    stack.pop_back();
    uint64_t b = static_cast<uint64_t>(node) * 4;
    uint8_t name_len = static_cast<uint8_t>(d[b + 13]);
    if (b + 14 + name_len > size) return invalid("directory entry name out of bounds");
    XdvdfsEntry e;
    e.sector = u32(&d[b + 4]);
    e.size = u32(&d[b + 8]);
    e.is_dir = (static_cast<uint8_t>(d[b + 12]) & 0x10) != 0;
    e.name.assign(reinterpret_cast<const char*>(&d[b + 14]), name_len);
    out.push_back(std::move(e));
    uint16_t right = u16(&d[b + 2]);
    if (right != 0) {
      cur = right;
      have_cur = true;
    }
  }
  return out;
}

Result<std::vector<XdvdfsEntry>> XdvdfsImage::list(std::string_view dir_path) const {
  uint32_t sector = root_sector_, size = root_size_;
  if (dir_path.find_first_not_of('/') != std::string_view::npos) {
    LR_TRY_ASSIGN(e, find(dir_path));
    if (!e.is_dir) return Error{ErrorCode::NotFound, std::string(dir_path) + " is not a directory", ""};
    sector = e.sector;
    size = e.size;
  }
  return read_dir(sector, size);
}

Result<XdvdfsEntry> XdvdfsImage::find(std::string_view path) const {
  uint32_t sector = root_sector_, size = root_size_;
  XdvdfsEntry found{"", true, sector, size};
  size_t pos = 0;
  while (pos < path.size()) {
    size_t slash = path.find('/', pos);
    if (slash == std::string_view::npos) slash = path.size();
    std::string_view part = path.substr(pos, slash - pos);
    pos = slash + 1;
    if (part.empty()) continue;
    if (!found.is_dir) return Error{ErrorCode::NotFound, "not found: " + std::string(path), ""};
    LR_TRY_ASSIGN(entries, read_dir(found.sector, found.size));
    bool hit = false;
    for (auto& e : entries) {
      if (iequals(e.name, part)) {
        found = e;
        hit = true;
        break;
      }
    }
    if (!hit) return Error{ErrorCode::NotFound, "not found in disc image: " + std::string(path), "check the path"};
  }
  return found;
}

Result<std::unique_ptr<IReader>> XdvdfsImage::open_file(std::string_view path) const {
  LR_TRY_ASSIGN(e, find(path));
  if (e.is_dir) return Error{ErrorCode::NotFound, std::string(path) + " is a directory", ""};
  uint64_t off = partition_ + static_cast<uint64_t>(e.sector) * kSector;
  if (off > reader_->size() || e.size > reader_->size() - off)
    return Error{ErrorCode::OutOfRange, "file extends past end of image: " + std::string(path),
                 "the image is truncated"};
  return std::unique_ptr<IReader>(new SubReader(*reader_, off, e.size));
}

}  // namespace lr::formats
