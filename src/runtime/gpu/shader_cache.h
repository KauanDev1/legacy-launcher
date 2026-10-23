// SPDX-License-Identifier: MIT
// The runtime side of the plan B translator (ADR 0005 M8): every microcode the game loads (IM_LOAD /
// IM_LOAD_IMMEDIATE) is copied and decoded at once (an IM_LOAD_IMMEDIATE pointer is only valid during the packet),
// and each ShaderKey a draw uses is translated once: from memory, else from the disk cache (shader/disk_cache.h),
// else by the translator, whose result goes to memory and to disk. Failures are remembered too (one translation per
// key, never one per draw). GPU-thread only; no Vulkan types.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/result.h"
#include "runtime/gpu/backend.h"
#include "runtime/gpu/shader/disk_cache.h"
#include "runtime/gpu/shader/ir.h"
#include "runtime/gpu/shader/shader_key.h"
#include "runtime/gpu/shader/translator.h"

namespace lr::gpu {

class ShaderCache {
 public:
  // Where the last translate() found its result.
  enum class Origin { Memory, Disk, Translator };

  // nullopt: memory only. The directory is created; if that fails, or a later store fails, a warning goes to stderr
  // and the cache stays in memory only (the run goes on). Before the first translate().
  void set_disk_dir(std::optional<std::filesystem::path> dir);
  // --dump-shaders: next to each cache file, `<name>.txt` with the key, the microcode disassembly and, for a
  // failure, the reason. Needs a disk directory (nothing is written without one).
  void set_dump(bool dump) { dump_ = dump; }
  bool disk_enabled() const { return disk_.has_value(); }

  // IM_LOAD: copy + decode; returns the hash. Decode errors are kept and surface at translate().
  uint64_t load(ShaderStage stage, const uint8_t* guest_microcode_be, uint32_t dwords);
  Result<const shader::TranslatedShader*> translate(const shader::ShaderKey& key);  // memory -> disk -> translator
  Origin last_origin() const { return last_origin_; }

 private:
  struct Loaded {
    std::vector<uint32_t> microcode;  // host order
    std::optional<shader::Program> program;
    std::string decode_error;
  };
  struct Entry {
    std::unique_ptr<shader::TranslatedShader> shader;  // null: failed
    std::string error;
  };

  Result<const shader::TranslatedShader*> remember(const shader::ShaderKey& key, Entry entry);
  void dump(const shader::ShaderKey& key, const Loaded& loaded, const std::string& error) const;
  void disable_disk(const std::string& why);

  std::map<shader::ShaderKey, Loaded> loaded_;  // by (stage, hash, dwords), modifiers 0
  std::map<shader::ShaderKey, Entry> translated_;
  std::optional<shader::DiskCache> disk_;
  bool dump_ = false;
  Origin last_origin_ = Origin::Memory;
};

}  // namespace lr::gpu
