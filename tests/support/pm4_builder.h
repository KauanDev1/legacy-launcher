// SPDX-License-Identifier: MIT
// Test helper: builds PM4 packet streams (host-order dwords) and stores them big-endian into a guest buffer.
#pragma once

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace lr::testing {

class Pm4Builder {
 public:
  // Type 0: `values.size()` dwords to consecutive registers from `base` (or all to `base` when one_reg).
  Pm4Builder& type0(uint32_t base, std::initializer_list<uint32_t> values, bool one_reg = false) {
    dwords_.push_back((0u << 30) | ((static_cast<uint32_t>(values.size()) - 1) & 0x3FFF) << 16 |
                      (one_reg ? 0x8000u : 0u) | (base & 0x7FFF));
    dwords_.insert(dwords_.end(), values);
    return *this;
  }
  Pm4Builder& type2() {
    dwords_.push_back(2u << 30);
    return *this;
  }
  // Type 3: header with count = payload.size() (encoded as count - 1), then the payload.
  Pm4Builder& type3(uint32_t opcode, std::initializer_list<uint32_t> payload, bool predicate = false) {
    return type3(opcode, std::vector<uint32_t>(payload), predicate);
  }
  Pm4Builder& type3(uint32_t opcode, const std::vector<uint32_t>& payload, bool predicate = false) {
    dwords_.push_back(type3_header(opcode, static_cast<uint32_t>(payload.size()), predicate));
    dwords_.insert(dwords_.end(), payload.begin(), payload.end());
    return *this;
  }
  // A raw dword (hand-made headers for malformed-packet tests).
  Pm4Builder& raw(uint32_t dword) {
    dwords_.push_back(dword);
    return *this;
  }

  static uint32_t type3_header(uint32_t opcode, uint32_t count, bool predicate = false) {
    return (3u << 30) | ((count - 1) & 0x3FFF) << 16 | (opcode & 0x7F) << 8 | (predicate ? 1u : 0u);
  }

  const std::vector<uint32_t>& dwords() const { return dwords_; }
  uint32_t size() const { return static_cast<uint32_t>(dwords_.size()); }

  // Stores the stream big-endian at `dst` (host pointer into guest memory).
  void store(uint8_t* dst) const {
    for (size_t i = 0; i < dwords_.size(); ++i) {
      const uint32_t be = __builtin_bswap32(dwords_[i]);
      std::memcpy(dst + i * 4, &be, 4);
    }
  }

 private:
  std::vector<uint32_t> dwords_;
};

}  // namespace lr::testing
