// SPDX-License-Identifier: MIT
// Test helper: builds an in-memory XDVDFS image at an XGD2 partition offset.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "formats/reader.h"

namespace lr::testing {

constexpr uint64_t kSector = 2048;
constexpr uint64_t kXgd2Offset = 0xFD90000;

struct BuiltImage {
  uint64_t partition_offset = kXgd2Offset;
  // Bytes starting at partition_offset (sector 0 of the partition).
  std::vector<std::byte> data;
  uint32_t root_sector = 33;

  // Reader whose bytes below partition_offset read as zero; size = offset + data.size().
  class Reader : public formats::IReader {
   public:
    explicit Reader(const BuiltImage& img) : img_(img) {}
    Result<void> read(uint64_t offset, std::span<std::byte> dst) override {
      if (offset > size() || dst.size() > size() - offset)
        return Error{ErrorCode::OutOfRange, "read past end", ""};
      for (size_t i = 0; i < dst.size(); ++i) {
        uint64_t p = offset + i;
        dst[i] = p < img_.partition_offset ? std::byte{0} : img_.data[p - img_.partition_offset];
      }
      return {};
    }
    uint64_t size() const override { return img_.partition_offset + img_.data.size(); }

   private:
    const BuiltImage& img_;
  };
  std::unique_ptr<Reader> reader() const { return std::make_unique<Reader>(*this); }
};

class XdvdfsBuilder {
 public:
  XdvdfsBuilder& add(const std::string& path, std::vector<std::byte> bytes) {
    Node* cur = &root_;
    size_t pos = 0;
    while (true) {
      size_t slash = path.find('/', pos);
      std::string part = path.substr(pos, slash == std::string::npos ? slash : slash - pos);
      Node* next = nullptr;
      for (auto& c : cur->children)
        if (c->name == part) next = c.get();
      if (!next) {
        cur->children.push_back(std::make_unique<Node>());
        next = cur->children.back().get();
        next->name = part;
      }
      cur = next;
      if (slash == std::string::npos) break;
      cur->is_dir = true;
      pos = slash + 1;
    }
    cur->bytes = std::move(bytes);
    return *this;
  }

  BuiltImage build() {
    BuiltImage img;
    next_sector_ = 33;
    layout(root_);
    img.root_sector = root_.sector;
    img.data.assign(static_cast<size_t>(next_sector_) * kSector, std::byte{0});
    static constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
    std::memcpy(&img.data[32 * kSector], kMagic, 20);
    put32(img.data, 32 * kSector + 0x14, root_.sector);
    put32(img.data, 32 * kSector + 0x18, root_.dir_size);
    emit(img.data, root_);
    return img;
  }

 private:
  struct Node {
    std::string name;
    bool is_dir = false;
    std::vector<std::byte> bytes;
    std::vector<std::unique_ptr<Node>> children;
    uint32_t sector = 0;
    uint32_t dir_size = 0;
  };

  static void put16(std::vector<std::byte>& d, size_t at, uint16_t v) {
    d[at] = std::byte(v & 0xFF);
    d[at + 1] = std::byte(v >> 8);
  }
  static void put32(std::vector<std::byte>& d, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) d[at + i] = std::byte((v >> (8 * i)) & 0xFF);
  }
  static uint64_t entry_len(const Node& n) { return (14 + n.name.size() + 3) & ~uint64_t{3}; }

  // Assigns each child an offset within the directory (never crossing a sector boundary).
  static uint32_t place(const Node& dir, std::vector<uint32_t>& offsets) {
    uint64_t pos = 0;
    for (auto& c : dir.children) {
      uint64_t len = entry_len(*c);
      if (pos % kSector + len > kSector) pos = (pos / kSector + 1) * kSector;
      offsets.push_back(static_cast<uint32_t>(pos));
      pos += len;
    }
    return static_cast<uint32_t>((pos + kSector - 1) / kSector * kSector);
  }

  void layout(Node& n) {
    if (n.is_dir || &n == &root_) {
      std::vector<uint32_t> offs;
      n.dir_size = place(n, offs);
    }
    n.sector = next_sector_;
    uint64_t bytes = n.is_dir || &n == &root_ ? n.dir_size : n.bytes.size();
    next_sector_ += static_cast<uint32_t>((bytes + kSector - 1) / kSector);
    for (auto& c : n.children) layout(*c);
  }

  void emit(std::vector<std::byte>& d, Node& n) {
    if (n.is_dir || &n == &root_) {
      std::vector<uint32_t> offs;
      place(n, offs);
      size_t base = static_cast<size_t>(n.sector) * kSector;
      std::fill(d.begin() + base, d.begin() + base + n.dir_size, std::byte{0xFF});
      for (size_t i = 0; i < n.children.size(); ++i) {
        Node& c = *n.children[i];
        size_t at = base + offs[i];
        uint32_t csize = c.is_dir ? c.dir_size : static_cast<uint32_t>(c.bytes.size());
        put16(d, at, 0);
        put16(d, at + 2, i + 1 < n.children.size() ? static_cast<uint16_t>(offs[i + 1] / 4) : 0);
        put32(d, at + 4, c.sector);
        put32(d, at + 8, csize);
        d[at + 12] = std::byte(c.is_dir ? 0x10 : 0x20);
        d[at + 13] = std::byte(c.name.size());
        std::memcpy(&d[at + 14], c.name.data(), c.name.size());
        for (size_t k = 14 + c.name.size(); k < entry_len(c); ++k) d[at + k] = std::byte{0xFF};
        emit(d, c);
      }
    } else if (!n.bytes.empty()) {
      std::memcpy(&d[static_cast<size_t>(n.sector) * kSector], n.bytes.data(), n.bytes.size());
    }
  }

  Node root_;
  uint32_t next_sector_ = 33;
};

}  // namespace lr::testing
