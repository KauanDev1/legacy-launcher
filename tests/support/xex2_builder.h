// SPDX-License-Identifier: MIT
// Test helper: builds an in-memory XEX2 header with optional headers. With pe_image() it builds a whole loadable
// XEX2 (no encryption, no compression, plain PE image) that XenonUtils' Image::ParseImage accepts, so tests run
// the real import handling (record byte-swap, thunk replacement).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace lr::testing {

// One PE section of a synthetic image: `rva` relative to the image base, `data` is its whole content.
struct PeSection {
  std::string name;
  uint32_t rva;
  std::vector<uint8_t> data;
  bool code;
};

// Writes `v` big-endian at `off` of `data` (for import record values inside section contents).
inline void put_be32(std::vector<uint8_t>& data, size_t off, uint32_t v) {
  for (int i = 0; i < 4; ++i) data[off + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (24 - 8 * i));
}

class Xex2Builder {
 public:
  Xex2Builder& magic(const std::string& m) { magic_ = m; return *this; }
  Xex2Builder& module_flags(uint32_t v) { flags_ = v; return *this; }
  Xex2Builder& pe_data_offset(uint32_t v) { pe_ = v; return *this; }
  Xex2Builder& entry_point(uint32_t v) { inline_.push_back({0x00010100, v}); return *this; }
  Xex2Builder& image_base(uint32_t v) { inline_.push_back({0x00010201, v}); return *this; }
  Xex2Builder& stack_size(uint32_t v) { inline_.push_back({0x00020200, v}); return *this; }
  Xex2Builder& execution(uint32_t media, uint32_t version, uint32_t base_version, uint32_t title,
                         uint8_t platform, uint8_t type, uint8_t disc, uint8_t discs) {
    exec_ = {media, version, base_version, title};
    exec_bytes_ = {platform, type, disc, discs};
    has_exec_ = true;
    return *this;
  }
  Xex2Builder& tls(uint32_t slot_count, uint32_t raw_data_address, uint32_t data_size, uint32_t raw_data_size) {
    tls_ = {slot_count, raw_data_address, data_size, raw_data_size};
    has_tls_ = true;
    return *this;
  }
  // Import libraries without records.
  Xex2Builder& libraries(const std::vector<std::string>& v) {
    for (const auto& n : v) import_library(n, {});
    return *this;
  }
  // One import library; `records` are the guest addresses of its record slots, in order.
  Xex2Builder& import_library(std::string name, std::vector<uint32_t> records) {
    libs_.push_back({std::move(name), std::move(records)});
    has_libs_ = true;
    return *this;
  }
  // Makes the file a whole loadable XEX2: file format info (no encryption/compression), security info (image size,
  // load address) and, at the PE data offset, a PE image of `image_size` bytes holding `sections`.
  Xex2Builder& pe_image(uint32_t load_address, uint32_t image_size, std::vector<PeSection> sections) {
    load_address_ = load_address;
    image_size_ = image_size;
    sections_ = std::move(sections);
    has_pe_ = true;
    return *this;
  }
  // Raw extra pair appended verbatim (key, value) with no data.
  Xex2Builder& raw_pair(uint32_t key, uint32_t value) { inline_.push_back({key, value}); return *this; }
  // Overrides the optional header count written in the header.
  Xex2Builder& claimed_count(uint32_t n) { claimed_ = n; has_claimed_ = true; return *this; }

  std::vector<std::byte> build() const {
    size_t n_pairs = inline_.size() + (has_exec_ ? 1 : 0) + (has_libs_ ? 1 : 0) + (has_tls_ ? 1 : 0) +
                     (has_pe_ ? 1 : 0);
    size_t data_off = 0x18 + n_pairs * 8;
    std::vector<std::byte> data;
    std::vector<std::pair<uint32_t, uint32_t>> pairs = inline_;
    if (has_exec_) {
      pairs.push_back({0x00040006, static_cast<uint32_t>(data_off + data.size())});
      for (uint32_t v : exec_) put32(data, v);
      for (uint8_t v : exec_bytes_) data.push_back(std::byte{v});
    }
    if (has_tls_) {
      pairs.push_back({0x00020104, static_cast<uint32_t>(data_off + data.size())});
      for (uint32_t v : tls_) put32(data, v);
    }
    if (has_libs_) {
      pairs.push_back({0x000103FF, static_cast<uint32_t>(data_off + data.size())});
      std::vector<std::byte> table;
      for (auto& l : libs_) {
        for (char c : l.first) table.push_back(std::byte(c));
        table.push_back(std::byte{0});
        while (table.size() % 4) table.push_back(std::byte{0});
      }
      std::vector<std::byte> libs;
      for (size_t i = 0; i < libs_.size(); ++i) {
        const auto& recs = libs_[i].second;
        put32(libs, static_cast<uint32_t>(0x28 + 4 * recs.size()));  // size
        for (int k = 0; k < 0x14; ++k) libs.push_back(std::byte{0});  // next import digest
        put32(libs, 0);                                               // id
        put32(libs, 0x20000000);                                      // version
        put32(libs, 0x20000000);                                      // min version
        put16(libs, static_cast<uint16_t>(i));                        // name index
        put16(libs, static_cast<uint16_t>(recs.size()));              // count
        for (uint32_t r : recs) put32(libs, r);
      }
      put32(data, static_cast<uint32_t>(12 + table.size() + libs.size()));
      put32(data, static_cast<uint32_t>(table.size()));
      put32(data, static_cast<uint32_t>(libs_.size()));
      data.insert(data.end(), table.begin(), table.end());
      data.insert(data.end(), libs.begin(), libs.end());
    }
    uint32_t security_off = 0;
    if (has_pe_) {
      pairs.push_back({0x000003FF, static_cast<uint32_t>(data_off + data.size())});
      put32(data, 8);  // info size
      put16(data, 0);  // encryption: none
      put16(data, 0);  // compression: none
      security_off = static_cast<uint32_t>(data_off + data.size());
      std::vector<std::byte> sec(0x180, std::byte{0});
      put32_at(sec, 0x000, 0x180);         // header size
      put32_at(sec, 0x004, image_size_);   // image size
      put32_at(sec, 0x110, load_address_); // load address
      data.insert(data.end(), sec.begin(), sec.end());
    }
    std::vector<std::byte> out;
    for (char c : magic_) out.push_back(std::byte(c));
    put32(out, flags_);
    const uint32_t header_end = static_cast<uint32_t>(data_off + data.size());
    const uint32_t pe = has_pe_ ? ((header_end + 0xFFF) & ~0xFFFu) : pe_;
    put32(out, pe);
    put32(out, 0);  // reserved
    put32(out, security_off);
    put32(out, has_claimed_ ? claimed_ : static_cast<uint32_t>(pairs.size()));
    for (auto& p : pairs) { put32(out, p.first); put32(out, p.second); }
    out.insert(out.end(), data.begin(), data.end());
    if (has_pe_) {
      out.resize(pe, std::byte{0});
      auto img = pe_bytes();
      for (uint8_t b : img) out.push_back(std::byte{b});
    }
    return out;
  }

 private:
  static void put32(std::vector<std::byte>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(std::byte((x >> s) & 0xFF));
  }
  static void put16(std::vector<std::byte>& v, uint16_t x) {
    v.push_back(std::byte(x >> 8));
    v.push_back(std::byte(x & 0xFF));
  }
  static void put32_at(std::vector<std::byte>& v, size_t off, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[off + static_cast<size_t>(i)] = std::byte((x >> (24 - 8 * i)) & 0xFF);
  }
  static void le16(std::vector<uint8_t>& v, size_t off, uint16_t x) {
    v[off] = static_cast<uint8_t>(x);
    v[off + 1] = static_cast<uint8_t>(x >> 8);
  }
  static void le32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[off + static_cast<size_t>(i)] = static_cast<uint8_t>(x >> (8 * i));
  }
  // Minimal little-endian PE: DOS header (e_lfanew = 0x80), NT headers, section table at 0x178, section data at RVA.
  std::vector<uint8_t> pe_bytes() const {
    std::vector<uint8_t> img(image_size_, 0);
    img[0] = 'M';
    img[1] = 'Z';
    le32(img, 0x3C, 0x80);
    le32(img, 0x80, 0x00004550);  // "PE\0\0"
    le16(img, 0x84, 0x01F2);      // machine: PowerPC big-endian
    le16(img, 0x86, static_cast<uint16_t>(sections_.size()));
    le16(img, 0x94, 0xE0);  // size of optional header
    for (size_t i = 0; i < sections_.size(); ++i) {
      const auto& s = sections_[i];
      const size_t h = 0x178 + i * 40;
      std::memcpy(img.data() + h, s.name.data(), s.name.size() < 8 ? s.name.size() : 8);
      le32(img, h + 8, static_cast<uint32_t>(s.data.size()));  // virtual size
      le32(img, h + 12, s.rva);
      le32(img, h + 16, static_cast<uint32_t>(s.data.size()));
      le32(img, h + 20, s.rva);
      le32(img, h + 36, s.code ? 0x60000020u : 0xC0000040u);
      std::memcpy(img.data() + s.rva, s.data.data(), s.data.size());
    }
    return img;
  }

  std::string magic_ = "XEX2";
  uint32_t flags_ = 1, pe_ = 0x2000, claimed_ = 0;
  bool has_claimed_ = false, has_exec_ = false, has_libs_ = false, has_tls_ = false, has_pe_ = false;
  std::vector<std::pair<uint32_t, uint32_t>> inline_;
  std::vector<uint32_t> exec_;
  std::vector<uint8_t> exec_bytes_;
  std::vector<uint32_t> tls_;
  std::vector<std::pair<std::string, std::vector<uint32_t>>> libs_;
  uint32_t load_address_ = 0, image_size_ = 0;
  std::vector<PeSection> sections_;
};

}  // namespace lr::testing
