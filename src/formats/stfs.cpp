// SPDX-License-Identifier: MIT
#include "formats/stfs.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <new>

namespace lr::formats {

namespace {
constexpr uint64_t kBlock = 0x1000;
constexpr uint32_t kBlocksPerHashTable = 0xAA;
constexpr size_t kEntrySize = 0x40;
constexpr size_t kNameMax = 0x28;
constexpr uint32_t kMaxFileTableBlocks = 0x1000;  // 4096 blocks = 64Ki entries
constexpr size_t kHeaderBytes = 0x4A0;            // covers every field we read
constexpr const char* kNonConsecutive = "non-consecutive STFS files are not supported yet";

uint32_t be32(const std::byte* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
uint16_t be16(const std::byte* p) {
  return static_cast<uint16_t>((static_cast<unsigned>(p[0]) << 8) | static_cast<unsigned>(p[1]));
}
uint32_t le24(const std::byte* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16);
}
Error invalid(std::string msg) {
  return Error{ErrorCode::InvalidFormat, std::move(msg), "the STFS package is corrupt"};
}
Error truncated(std::string msg) {
  return Error{ErrorCode::OutOfRange, std::move(msg), "the STFS package is truncated"};
}
char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }
bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (lower(a[i]) != lower(b[i])) return false;
  return true;
}

uint64_t data_start(uint32_t header_size) {
  return (static_cast<uint64_t>(header_size) + kBlock - 1) / kBlock * kBlock;
}

// Maps reads onto the data blocks of one consecutive file, one block at a time, because hash-table
// blocks sit between data blocks every 0xAA blocks.
class StfsFileReader : public IReader {
 public:
  StfsFileReader(IReader* parent, uint32_t start_block, uint32_t size, uint32_t header_size,
                 bool read_only)
      : parent_(parent), start_(start_block), size_(size), header_size_(header_size),
        read_only_(read_only) {}
  uint64_t size() const override { return size_; }
  Result<void> read(uint64_t offset, std::span<std::byte> dst) override {
    if (offset > size_ || dst.size() > size_ - offset)
      return Error{ErrorCode::OutOfRange, "read past end of STFS file", ""};
    size_t done = 0;
    while (done < dst.size()) {
      uint64_t pos = offset + done;
      uint64_t blk = pos / kBlock, within = pos % kBlock;
      size_t n = static_cast<size_t>(std::min<uint64_t>(kBlock - within, dst.size() - done));
      uint64_t at = StfsPackage::block_to_offset(static_cast<uint32_t>(start_ + blk), header_size_,
                                                 read_only_) + within;
      LR_TRY(parent_->read(at, dst.subspan(done, n)));
      done += n;
    }
    return {};
  }

 private:
  IReader* parent_;
  uint32_t start_;
  uint32_t size_;
  uint32_t header_size_;
  bool read_only_;
};
}  // namespace

// Mirrors Xenia's StfsContainerDevice::BlockToOffsetSTFS (src/xenia/vfs/devices/
// stfs_container_device.cc): every level adds one hash table (two when not read-only).
uint64_t StfsPackage::block_to_offset(uint32_t block, uint32_t header_size, bool read_only) {
  const uint64_t per_table = read_only ? 1 : 2;
  uint64_t base = kBlocksPerHashTable;
  uint64_t blk = block;
  for (int level = 0; level < 3; ++level) {
    blk += ((static_cast<uint64_t>(block) + base) / base) * per_table;
    if (block < base) break;
    base *= kBlocksPerHashTable;
  }
  return data_start(header_size) + blk * kBlock;
}

Result<StfsPackage> StfsPackage::open(IReader& reader) {
  try {
    if (reader.size() < kHeaderBytes)
      return invalid("file too small to be an STFS package");
    std::array<std::byte, kHeaderBytes> h{};
    LR_TRY(reader.read(0, h));
    StfsHeader hdr;
    hdr.magic.assign(reinterpret_cast<const char*>(h.data()), 4);
    if (hdr.magic != "CON " && hdr.magic != "LIVE" && hdr.magic != "PIRS")
      return invalid("bad STFS magic");
    uint32_t header_size = be32(&h[0x340]);
    hdr.content_type = be32(&h[0x344]);
    hdr.title_id = be32(&h[0x360]);
    hdr.read_only = (static_cast<uint8_t>(h[0x37B]) & 1) != 0;
    for (size_t i = 0; i < 0x40; ++i) {
      char16_t c = static_cast<char16_t>(be16(&h[0x411 + 2 * i]));
      if (c == 0) break;
      hdr.display_name.push_back(c);
    }
    uint32_t table_blocks = static_cast<uint16_t>(static_cast<unsigned>(h[0x37C]) |
                                                  (static_cast<unsigned>(h[0x37D]) << 8));
    uint32_t table_first = le24(&h[0x37E]);
    if (table_blocks == 0 || table_blocks > kMaxFileTableBlocks)
      return invalid("implausible STFS file table size: " + std::to_string(table_blocks) + " blocks");

    StfsPackage pkg(&reader, std::move(hdr), header_size);
    const bool ro = pkg.header_.read_only;

    struct Raw {
      std::string name;
      bool is_dir, consecutive;
      uint32_t size, start;
      int32_t parent;
    };
    std::vector<Raw> raw;
    std::vector<std::byte> blockbuf(kBlock);
    bool done = false;
    // The file table is read as consecutive blocks (the volume descriptor has no flag for it).
    for (uint32_t b = 0; b < table_blocks && !done; ++b) {
      uint64_t off = block_to_offset(table_first + b, header_size, ro);
      if (off > reader.size() || kBlock > reader.size() - off)
        return truncated("STFS file table extends past end of package");
      LR_TRY(reader.read(off, blockbuf));
      for (size_t m = 0; m < kBlock / kEntrySize; ++m) {
        const std::byte* e = &blockbuf[m * kEntrySize];
        uint8_t flags = static_cast<uint8_t>(e[0x28]);
        size_t len = flags & 0x3F;
        if (len == 0) {
          done = true;
          break;
        }
        if (len > kNameMax) return invalid("STFS entry name too long");
        Raw r;
        r.name.assign(reinterpret_cast<const char*>(e), len);
        r.consecutive = (flags & 0x40) != 0;
        r.is_dir = (flags & 0x80) != 0;
        r.start = le24(e + 0x2F);
        uint16_t parent = be16(e + 0x32);
        r.parent = parent == 0xFFFF ? -1 : parent;
        r.size = be32(e + 0x34);
        raw.push_back(std::move(r));
      }
    }
    for (const auto& r : raw)
      if (r.parent >= 0 && static_cast<size_t>(r.parent) >= raw.size())
        return invalid("STFS entry parent index " + std::to_string(r.parent) + " is past the file table");

    pkg.entries_.reserve(raw.size());
    for (const auto& r : raw) {
      std::string path = r.name;
      int32_t p = r.parent;
      size_t depth = 0;
      while (p >= 0) {
        if (++depth > raw.size()) return invalid("cycle in STFS directory parents");
        if (!raw[static_cast<size_t>(p)].is_dir) return invalid("STFS entry parent is not a directory");
        path = raw[static_cast<size_t>(p)].name + "/" + path;
        p = raw[static_cast<size_t>(p)].parent;
      }
      pkg.entries_.push_back(StfsEntry{std::move(path), r.is_dir, r.size, r.start, r.consecutive});
    }
    return pkg;
  } catch (const std::bad_alloc&) {
    return Error{ErrorCode::InsufficientResources, "out of memory while reading STFS package",
                 "the package is corrupt or the system is low on memory"};
  }
}

Result<std::unique_ptr<IReader>> StfsPackage::open_file(std::string_view path) const {
  while (!path.empty() && path.front() == '/') path.remove_prefix(1);
  const StfsEntry* hit = nullptr;
  for (const auto& e : entries_)
    if (iequals(e.path, path)) {
      hit = &e;
      break;
    }
  if (!hit) return Error{ErrorCode::NotFound, "not found in STFS package: " + std::string(path), "check the path"};
  if (hit->is_dir) return Error{ErrorCode::NotFound, std::string(path) + " is a directory", ""};
  if (!hit->consecutive) return Error{ErrorCode::InvalidFormat, kNonConsecutive, "see docs/formats/stfs.md"};
  if (hit->size > 0) {
    uint64_t nblocks = (static_cast<uint64_t>(hit->size) + kBlock - 1) / kBlock;
    uint64_t last = static_cast<uint64_t>(hit->start_block) + nblocks - 1;
    if (last > 0xFFFFFFFFull) return invalid("STFS file block range overflows");
    // Offsets grow with the block number, so checking the first and last block is enough.
    uint64_t first_off = block_to_offset(hit->start_block, header_size_, header_.read_only);
    uint64_t last_off = block_to_offset(static_cast<uint32_t>(last), header_size_, header_.read_only);
    uint64_t tail = hit->size - (nblocks - 1) * kBlock;
    if (first_off > reader_->size() || last_off > reader_->size() || tail > reader_->size() - last_off)
      return truncated("file extends past end of STFS package: " + std::string(path));
  }
  try {
    return std::unique_ptr<IReader>(
        new StfsFileReader(reader_, hit->start_block, hit->size, header_size_, header_.read_only));
  } catch (const std::bad_alloc&) {
    return Error{ErrorCode::InsufficientResources, "out of memory", ""};
  }
}

}  // namespace lr::formats
