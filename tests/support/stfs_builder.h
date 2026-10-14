// SPDX-License-Identifier: MIT
// Test helper: builds an in-memory STFS package with consecutive files. Hash-table blocks are left
// zeroed (the reader never looks at them); block placement uses its own level-0/1 formula so it is
// independent of StfsPackage::block_to_offset.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace lr::testing {

class StfsBuilder {
 public:
  StfsBuilder& magic(const char* m) { std::memcpy(magic_, m, 4); return *this; }
  StfsBuilder& read_only(bool v) { read_only_ = v; return *this; }
  StfsBuilder& content_type(uint32_t v) { content_type_ = v; return *this; }
  StfsBuilder& title_id(uint32_t v) { title_id_ = v; return *this; }
  StfsBuilder& display_name(const std::u16string& v) { name_ = v; return *this; }
  StfsBuilder& add_dir(const std::string& name, int parent = -1) {
    entries_.push_back({name, true, parent, {}});
    return *this;
  }
  // Returns the entry index (usable as parent).
  int add_file(const std::string& name, std::vector<std::byte> data, int parent = -1) {
    entries_.push_back({name, false, parent, std::move(data)});
    return static_cast<int>(entries_.size()) - 1;
  }
  // Test hook: overwrite the parent index of the last entry written (raw, may be invalid).
  StfsBuilder& raw_parent_of(size_t index, int16_t parent) {
    entries_.at(index).parent = parent;
    return *this;
  }

  std::vector<std::byte> build() const {
    constexpr uint32_t kHeaderSize = 0x971A;
    const uint64_t base = 0xA000;  // round_up(kHeaderSize, 0x1000)
    const uint64_t mult = read_only_ ? 1 : 2;
    auto offset_of = [&](uint64_t b) {
      uint64_t blk = b + ((b + 170) / 170) * mult;       // level 0 hash tables
      if (b >= 170) blk += ((b + 28900) / 28900) * mult;  // level 1 hash tables
      return base + blk * 0x1000;
    };
    const uint32_t table_blocks = static_cast<uint32_t>((entries_.size() * 0x40 + 0xFFF) / 0x1000);
    const uint32_t table_count = table_blocks == 0 ? 1 : table_blocks;
    std::vector<uint32_t> starts;
    uint32_t next = table_count;
    for (auto& e : entries_) {
      starts.push_back(e.is_dir ? 0 : next);
      if (!e.is_dir) next += static_cast<uint32_t>((e.data.size() + 0xFFF) / 0x1000);
    }
    std::vector<std::byte> out(offset_of(next) + 0x1000, std::byte{0});
    auto put = [&](size_t at, const void* p, size_t n) { std::memcpy(&out[at], p, n); };
    auto be32 = [&](size_t at, uint32_t v) {
      for (int i = 0; i < 4; ++i) out[at + i] = std::byte(v >> (24 - 8 * i));
    };
    put(0, magic_, 4);
    be32(0x340, kHeaderSize);
    be32(0x344, content_type_);
    be32(0x360, title_id_);
    out[0x379] = std::byte{0x24};
    out[0x37A] = std::byte{3};
    out[0x37B] = std::byte(read_only_ ? 1 : 0);
    out[0x37C] = std::byte(table_count & 0xFF);
    out[0x37D] = std::byte(table_count >> 8);
    for (int i = 0; i < 3; ++i) out[0x37E + i] = std::byte{0};  // file table at block 0
    for (size_t i = 0; i < name_.size() && i < 64; ++i) {
      out[0x411 + 2 * i] = std::byte(name_[i] >> 8);
      out[0x411 + 2 * i + 1] = std::byte(name_[i] & 0xFF);
    }
    for (size_t i = 0; i < entries_.size(); ++i) {
      const auto& e = entries_[i];
      size_t block = i / 64, within = i % 64;
      size_t at = offset_of(block) + within * 0x40;
      put(at, e.name.data(), e.name.size());
      uint8_t flags = static_cast<uint8_t>(e.name.size()) | 0x40 | (e.is_dir ? 0x80 : 0);
      out[at + 0x28] = std::byte(flags);
      uint32_t nblocks = e.is_dir ? 0 : static_cast<uint32_t>((e.data.size() + 0xFFF) / 0x1000);
      for (int k = 0; k < 3; ++k) {
        out[at + 0x29 + k] = std::byte(nblocks >> (8 * k));
        out[at + 0x2F + k] = std::byte(starts[i] >> (8 * k));
      }
      uint16_t p = static_cast<uint16_t>(static_cast<int16_t>(e.parent));
      out[at + 0x32] = std::byte(p >> 8);
      out[at + 0x33] = std::byte(p & 0xFF);
      be32(at + 0x34, e.is_dir ? 0 : static_cast<uint32_t>(e.data.size()));
      if (!e.is_dir) {
        for (size_t k = 0; k < e.data.size(); k += 0x1000) {
          size_t n = std::min<size_t>(0x1000, e.data.size() - k);
          std::memcpy(&out[offset_of(starts[i] + k / 0x1000)], &e.data[k], n);
        }
      }
    }
    return out;
  }

 private:
  struct Entry {
    std::string name;
    bool is_dir;
    int parent;
    std::vector<std::byte> data;
  };
  char magic_[4] = {'L', 'I', 'V', 'E'};
  bool read_only_ = true;
  uint32_t content_type_ = 0x000D0000;
  uint32_t title_id_ = 0x584111F7;
  std::u16string name_ = u"Minecraft";
  std::vector<Entry> entries_;
};

}  // namespace lr::testing
