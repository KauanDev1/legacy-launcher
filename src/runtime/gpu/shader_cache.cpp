// SPDX-License-Identifier: MIT
#include "runtime/gpu/shader_cache.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

// Header-only xxHash (XenonRecomp's bundled copy, the one the XenosRecomp fork links): nothing extra to link.
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <zstd.h>

namespace lr::gpu {

namespace {

std::string hex64(uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof b, "0x%016llX", static_cast<unsigned long long>(v));
  return b;
}

Error bad(std::string what) {
  return Error{ErrorCode::InvalidFormat, "shader cache: " + std::move(what),
               "the generated shader cache is corrupt: re-run `legacyrecomp install --force shaders`"};
}

}  // namespace

uint64_t microcode_hash(const uint8_t* guest_microcode, uint32_t dwords) {
  return XXH3_64bits(guest_microcode, size_t{dwords} * 4);
}

Result<void> ShaderCache::register_entries(std::span<const lr_shader_cache_entry> entries,
                                           std::span<const uint8_t> blob, size_t uncompressed_size) {
  std::vector<uint8_t> spirv(uncompressed_size);
  if (uncompressed_size != 0 || !blob.empty()) {
    const size_t n = ZSTD_decompress(spirv.data(), spirv.size(), blob.data(), blob.size());
    if (ZSTD_isError(n)) return bad(std::string("cannot decompress the SPIR-V blob: ") + ZSTD_getErrorName(n));
    if (n != uncompressed_size)
      return bad("the SPIR-V blob decompresses to " + std::to_string(n) + " bytes, not " +
                 std::to_string(uncompressed_size));
  }
  std::vector<ShaderEntry> out;
  out.reserve(entries.size());
  for (const lr_shader_cache_entry& e : entries) {
    if (e.stage > 1) return bad("entry " + hex64(e.microcode_hash) + " has stage " + std::to_string(e.stage));
    if (e.spirv_size == 0 || e.spirv_size % 4 != 0 || e.spirv_offset % 4 != 0 ||
        uint64_t{e.spirv_offset} + e.spirv_size > uncompressed_size)
      return bad("entry " + hex64(e.microcode_hash) + ": SPIR-V at " + std::to_string(e.spirv_offset) + " (" +
                 std::to_string(e.spirv_size) + " bytes) is not a 4-byte aligned module inside the " +
                 std::to_string(uncompressed_size) + "-byte blob");
    out.push_back({e.microcode_hash, e.stage == 0 ? ShaderStage::Vertex : ShaderStage::Pixel, e.spirv_offset,
                   e.spirv_size, e.spec_mask});
  }
  std::sort(out.begin(), out.end(),
            [](const ShaderEntry& a, const ShaderEntry& b) { return a.microcode_hash < b.microcode_hash; });
  for (size_t i = 1; i < out.size(); ++i)
    if (out[i].microcode_hash == out[i - 1].microcode_hash)
      return bad("microcode hash " + hex64(out[i].microcode_hash) + " appears twice");
  entries_ = std::move(out);
  spirv_ = std::move(spirv);
  return {};
}

const ShaderEntry* ShaderCache::find(uint64_t hash) const {
  auto it = std::lower_bound(entries_.begin(), entries_.end(), hash,
                             [](const ShaderEntry& e, uint64_t h) { return e.microcode_hash < h; });
  return it != entries_.end() && it->microcode_hash == hash ? &*it : nullptr;
}

Result<std::vector<uint32_t>> ShaderCache::spirv(const ShaderEntry& entry) const {
  if (uint64_t{entry.spirv_offset} + entry.spirv_size > spirv_.size() || entry.spirv_size % 4 != 0)
    return Error{ErrorCode::OutOfRange, "shader cache: entry " + hex64(entry.microcode_hash) + " lies outside the blob",
                 ""};
  std::vector<uint32_t> words(entry.spirv_size / 4);
  if (words.empty()) return words;
  std::memcpy(words.data(), spirv_.data() + entry.spirv_offset, entry.spirv_size);
  return words;
}

}  // namespace lr::gpu
