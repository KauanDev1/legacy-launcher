// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "common/sha256.h"  // to_hex

namespace lr {

// SHA-1 (FIPS 180-4), same shape as Sha256. Used by the XeCrypt kernel imports, never for security decisions.
// load/store expose the running state so a guest-memory state block (XECRYPT_SHA_STATE) can be resumed.
class Sha1 {
 public:
  Sha1();
  void update(std::span<const std::byte> data);
  std::array<uint8_t, 20> finish();

  // Resumes from `h` (chaining words), `total_len` bytes hashed so far and `buf` (its first total_len % 64 bytes
  // are the pending partial block).
  void load(const uint32_t (&h)[5], uint64_t total_len, const uint8_t (&buf)[64]);
  // Writes the chaining words and the pending partial block (the total length is total_len()).
  void store(uint32_t (&h)[5], uint8_t (&buf)[64]) const;
  uint64_t total_len() const { return total_len_; }

 private:
  void process_block(const uint8_t* block);

  std::array<uint32_t, 5> state_;
  std::array<uint8_t, 64> buf_{};
  size_t buf_len_ = 0;
  uint64_t total_len_ = 0;
};

}  // namespace lr
