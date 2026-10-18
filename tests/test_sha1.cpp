// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <string>

#include "common/sha1.h"

static std::string sha1_of(const std::string& s, size_t chunk) {
  lr::Sha1 h;
  const auto* p = reinterpret_cast<const std::byte*>(s.data());
  for (size_t i = 0; i < s.size(); i += chunk) h.update({p + i, std::min(chunk, s.size() - i)});
  auto d = h.finish();
  return lr::to_hex(d);
}

TEST_CASE("sha1 FIPS vectors", "[unit][sha1]") {
  CHECK(sha1_of("", 1) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  CHECK(sha1_of("abc", 1) == "a9993e364706816aba3e25717850c26c9cd0d89d");
  CHECK(sha1_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 5) ==
        "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
  CHECK(sha1_of(std::string(1000000, 'a'), 7) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}
