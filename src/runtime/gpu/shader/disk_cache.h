// SPDX-License-Identifier: MIT
// The on-disk cache of translated shaders (plan B, ADR 0005 M8): one file per ShaderKey in a directory the launcher
// chooses (`<root>/work/shaders/runtime/`, passed as --shader-cache-dir). The SPIR-V is game-derived: it lives only
// there and is never committed or distributed.
//
// File `cache_file_name(key)` (host byte order, little-endian on x86-64):
//   0  char[4] "LRSV"          4  u32 kTranslatorVersion   8  u32 stage (0 vertex, 1 pixel)   12 u32 dwords
//   16 u64 microcode hash      24 u64 modifiers           32 u32 SPIR-V words                36 u32 0
//   40 u64 XXH3-64 of every byte from offset 48 to the end
//   48 the microcode (dwords u32, as the caller passed it), then the SPIR-V words.
// The name already carries the version and the key; the header repeats them so a renamed or mixed-up file is caught.
// The microcode is the collision check: a file whose key matches but whose microcode differs is a different shader.
//
// load() never trusts a file: a missing file is a silent miss; a short, oversized, corrupt (checksum), mismatched
// (magic, version, key, microcode) or non-SPIR-V file is a miss with one warning on stderr, and the caller
// retranslates and store()s over it. store() writes a temporary file in the same directory and renames it over the
// final name, so a reader (or a crash) never sees a half-written file.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include "common/result.h"
#include "runtime/gpu/shader/shader_key.h"

namespace lr::gpu::shader {

inline constexpr uint32_t kDiskCacheHeaderBytes = 48;

class DiskCache {
 public:
  // `dir` must exist (ShaderCache::set_disk_dir creates it).
  explicit DiskCache(std::filesystem::path dir);

  std::optional<std::vector<uint32_t>> load(const ShaderKey& key, std::span<const uint32_t> microcode) const;
  Result<void> store(const ShaderKey& key, std::span<const uint32_t> microcode, std::span<const uint32_t> spirv);

  const std::filesystem::path& dir() const { return dir_; }
  std::filesystem::path path_of(const ShaderKey& key) const { return dir_ / cache_file_name(key); }

 private:
  std::filesystem::path dir_;
  uint64_t temp_counter_ = 0;
};

}  // namespace lr::gpu::shader
