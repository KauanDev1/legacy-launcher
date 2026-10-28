// SPDX-License-Identifier: MIT
#include "common/sha1.h"

#include <algorithm>
#include <cstring>

namespace lr {

namespace {
constexpr uint32_t rotl(uint32_t x, unsigned n) { return (x << n) | (x >> (32 - n)); }
}  // namespace

Sha1::Sha1() : state_{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0} {}

void Sha1::process_block(const uint8_t* b) {
  uint32_t w[80];
  for (int i = 0; i < 16; ++i)
    w[i] = (uint32_t{b[4 * i]} << 24) | (uint32_t{b[4 * i + 1]} << 16) | (uint32_t{b[4 * i + 2]} << 8) |
           uint32_t{b[4 * i + 3]};
  for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  uint32_t a = state_[0], bb = state_[1], c = state_[2], d = state_[3], e = state_[4];
  for (int i = 0; i < 80; ++i) {
    uint32_t f, k;
    if (i < 20) { f = (bb & c) | (~bb & d); k = 0x5A827999; }
    else if (i < 40) { f = bb ^ c ^ d; k = 0x6ED9EBA1; }
    else if (i < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8F1BBCDC; }
    else { f = bb ^ c ^ d; k = 0xCA62C1D6; }
    const uint32_t t = rotl(a, 5) + f + e + k + w[i];
    e = d; d = c; c = rotl(bb, 30); bb = a; a = t;
  }
  state_[0] += a; state_[1] += bb; state_[2] += c; state_[3] += d; state_[4] += e;
}

void Sha1::update(std::span<const std::byte> data) {
  total_len_ += data.size();
  const auto* p = reinterpret_cast<const uint8_t*>(data.data());
  size_t n = data.size();
  if (buf_len_ > 0) {
    size_t take = std::min(n, buf_.size() - buf_len_);
    std::memcpy(buf_.data() + buf_len_, p, take);
    buf_len_ += take; p += take; n -= take;
    if (buf_len_ < buf_.size()) return;
    process_block(buf_.data());
    buf_len_ = 0;
  }
  while (n >= 64) { process_block(p); p += 64; n -= 64; }
  if (n > 0) { std::memcpy(buf_.data(), p, n); buf_len_ = n; }
}

std::array<uint8_t, 20> Sha1::finish() {
  const uint64_t bit_len = total_len_ * 8;
  buf_[buf_len_++] = 0x80;
  if (buf_len_ > 56) {
    std::fill(buf_.begin() + static_cast<std::ptrdiff_t>(buf_len_), buf_.end(), uint8_t{0});
    process_block(buf_.data());
    buf_len_ = 0;
  }
  std::fill(buf_.begin() + static_cast<std::ptrdiff_t>(buf_len_), buf_.end(), uint8_t{0});
  for (int i = 0; i < 8; ++i) buf_[56 + i] = static_cast<uint8_t>(bit_len >> (56 - 8 * i));
  process_block(buf_.data());
  buf_len_ = 0;
  std::array<uint8_t, 20> out{};
  for (int i = 0; i < 5; ++i)
    for (int j = 0; j < 4; ++j) out[4 * i + j] = static_cast<uint8_t>(state_[i] >> (24 - 8 * j));
  return out;
}

void Sha1::load(const uint32_t (&h)[5], uint64_t total_len, const uint8_t (&buf)[64]) {
  std::copy(h, h + 5, state_.begin());
  total_len_ = total_len;
  buf_len_ = static_cast<size_t>(total_len % 64);
  std::memcpy(buf_.data(), buf, buf_.size());
}

void Sha1::store(uint32_t (&h)[5], uint8_t (&buf)[64]) const {
  std::copy(state_.begin(), state_.end(), h);
  std::memcpy(buf, buf_.data(), buf_.size());
}

}  // namespace lr
