// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

#include "common/result.h"

namespace lr {

class Sha256 {
 public:
  Sha256();
  void update(std::span<const std::byte> data);
  std::array<uint8_t, 32> finish();

 private:
  void process_block(const uint8_t* block);

  std::array<uint32_t, 8> state_;
  std::array<uint8_t, 64> buf_{};
  size_t buf_len_ = 0;
  uint64_t total_len_ = 0;
};

std::string to_hex(std::span<const uint8_t> bytes);

// Lowercase hex SHA-256 of a file, streamed in 1 MiB chunks.
Result<std::string> sha256_file(const std::filesystem::path& path);

}  // namespace lr
