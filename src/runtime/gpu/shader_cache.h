// SPDX-License-Identifier: MIT
// The runtime side of the translated shaders (docs/architecture.md, "Contrato dos shaders traduzidos"): the cache
// the shaders stage generates (work/shaders/shader_cache.cpp, compiled into the game) maps the XXH3-64 hash of a
// shader's microcode, as stored big-endian in its container, to the SPIR-V the patched XenosRecomp produced. The
// game uploads the same microcode with IM_LOAD / IM_LOAD_IMMEDIATE, so hashing the loaded dwords, still big-endian,
// in guest memory finds its entry. The game's main.cpp registers the generated tables; this library never names
// those symbols, so the unit tests link without a generated cache.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "common/result.h"
#include "runtime/gpu/backend.h"

// One row of the generated lr_shader_cache[] table. Global namespace and the exact layout the tool writes, so the
// extern "C" declarations in main.cpp name the generated objects.
struct lr_shader_cache_entry {
  uint64_t microcode_hash;  // XXH3-64 of the microcode as stored (big-endian)
  uint8_t stage;            // 0 = vertex, 1 = pixel
  uint32_t spirv_offset, spirv_size;  // bytes, in the decompressed lr_shader_blob
  uint32_t spec_mask;                 // bits of spec constant 0 the shader reads
};

namespace lr::gpu {

struct ShaderEntry {
  uint64_t microcode_hash;
  ShaderStage stage;
  uint32_t spirv_offset, spirv_size;  // bytes in the decompressed blob
  uint32_t spec_mask;
};

// XXH3-64 (seed 0) of the `dwords` microcode dwords at `guest_microcode`, read as the bytes they are in guest memory
// (big-endian, no swap): the hash the XenosRecomp fork emits.
uint64_t microcode_hash(const uint8_t* guest_microcode, uint32_t dwords);

class ShaderCache {
 public:
  // Takes the generated tables: `entries` (lr_shader_cache, lr_shader_cache_count), the zstd `blob` (lr_shader_blob,
  // lr_shader_blob_size) and its decompressed size (lr_shader_blob_uncompressed_size). Decompresses the blob and
  // checks every entry: stage 0 or 1, no hash twice, its SPIR-V a non-empty multiple of 4 bytes inside the blob at an
  // offset multiple of 4. Any failure is InvalidFormat and leaves the cache as it was. Replaces a previous
  // registration. Not thread-safe: register before the GPU thread starts.
  Result<void> register_entries(std::span<const lr_shader_cache_entry> entries, std::span<const uint8_t> blob,
                                size_t uncompressed_size);
  // The entry for a microcode hash, or nullptr. Thread-safe once registered.
  const ShaderEntry* find(uint64_t microcode_hash) const;
  // The SPIR-V words of an entry this cache returned.
  Result<std::vector<uint32_t>> spirv(const ShaderEntry& entry) const;
  size_t size() const { return entries_.size(); }

 private:
  std::vector<ShaderEntry> entries_;  // ascending microcode_hash
  std::vector<uint8_t> spirv_;        // the decompressed blob
};

}  // namespace lr::gpu
