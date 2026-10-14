// SPDX-License-Identifier: MIT
// Test helper: builds an in-memory XEX2 header with optional headers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lr::testing {

class Xex2Builder {
 public:
  Xex2Builder& magic(const std::string& m) { magic_ = m; return *this; }
  Xex2Builder& module_flags(uint32_t v) { flags_ = v; return *this; }
  Xex2Builder& pe_data_offset(uint32_t v) { pe_ = v; return *this; }
  Xex2Builder& entry_point(uint32_t v) { inline_.push_back({0x00010100, v}); return *this; }
  Xex2Builder& image_base(uint32_t v) { inline_.push_back({0x00010201, v}); return *this; }
  Xex2Builder& execution(uint32_t media, uint32_t version, uint32_t base_version, uint32_t title,
                         uint8_t platform, uint8_t type, uint8_t disc, uint8_t discs) {
    exec_ = {media, version, base_version, title};
    exec_bytes_ = {platform, type, disc, discs};
    has_exec_ = true;
    return *this;
  }
  Xex2Builder& libraries(std::vector<std::string> v) { libs_ = std::move(v); has_libs_ = true; return *this; }
  // Raw extra pair appended verbatim (key, value) with no data.
  Xex2Builder& raw_pair(uint32_t key, uint32_t value) { inline_.push_back({key, value}); return *this; }
  // Overrides the optional header count written in the header.
  Xex2Builder& claimed_count(uint32_t n) { claimed_ = n; has_claimed_ = true; return *this; }

  std::vector<std::byte> build() const {
    size_t n_pairs = inline_.size() + (has_exec_ ? 1 : 0) + (has_libs_ ? 1 : 0);
    size_t data_off = 0x18 + n_pairs * 8;
    std::vector<std::byte> data;
    std::vector<std::pair<uint32_t, uint32_t>> pairs = inline_;
    if (has_exec_) {
      pairs.push_back({0x00040006, static_cast<uint32_t>(data_off + data.size())});
      for (uint32_t v : exec_) put32(data, v);
      for (uint8_t v : exec_bytes_) data.push_back(std::byte{v});
    }
    if (has_libs_) {
      pairs.push_back({0x000103FF, static_cast<uint32_t>(data_off + data.size())});
      std::vector<std::byte> table;
      for (auto& s : libs_) {
        for (char c : s) table.push_back(std::byte(c));
        table.push_back(std::byte{0});
        while (table.size() % 4) table.push_back(std::byte{0});
      }
      put32(data, static_cast<uint32_t>(12 + table.size()));
      put32(data, static_cast<uint32_t>(table.size()));
      put32(data, static_cast<uint32_t>(libs_.size()));
      data.insert(data.end(), table.begin(), table.end());
    }
    std::vector<std::byte> out;
    for (char c : magic_) out.push_back(std::byte(c));
    put32(out, flags_);
    put32(out, pe_);
    put32(out, 0);  // reserved
    put32(out, 0);  // security info offset
    put32(out, has_claimed_ ? claimed_ : static_cast<uint32_t>(pairs.size()));
    for (auto& p : pairs) { put32(out, p.first); put32(out, p.second); }
    out.insert(out.end(), data.begin(), data.end());
    return out;
  }

 private:
  static void put32(std::vector<std::byte>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(std::byte((x >> s) & 0xFF));
  }
  std::string magic_ = "XEX2";
  uint32_t flags_ = 1, pe_ = 0x2000, claimed_ = 0;
  bool has_claimed_ = false, has_exec_ = false, has_libs_ = false;
  std::vector<std::pair<uint32_t, uint32_t>> inline_;
  std::vector<uint32_t> exec_;
  std::vector<uint8_t> exec_bytes_;
  std::vector<std::string> libs_;
};

}  // namespace lr::testing
