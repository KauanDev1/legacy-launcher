// SPDX-License-Identifier: MIT
#include "runtime/gpu/shader_cache.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>
#include <utility>

#include "runtime/gpu/shader/disasm.h"

namespace lr::gpu {

namespace {

shader::ShaderKey loaded_key(ShaderStage stage, uint64_t hash, uint32_t dwords) { return {stage, hash, dwords, 0}; }

const char* stage_name(ShaderStage s) { return s == ShaderStage::Vertex ? "vertex" : "pixel"; }

}  // namespace

void ShaderCache::set_disk_dir(std::optional<std::filesystem::path> dir) {
  disk_.reset();
  if (!dir) return;
  std::error_code ec;
  std::filesystem::create_directories(*dir, ec);
  if (ec || !std::filesystem::is_directory(*dir, ec)) {
    std::fprintf(stderr,
                 "[gpu] shader cache: cannot create the directory %s (%s); translated shaders stay in memory only\n",
                 dir->c_str(), ec ? ec.message().c_str() : "not a directory");
    return;
  }
  // Temporary files a crashed or killed run left behind (DiskCache::store renames them on success). One game process
  // uses the directory at a time, so none of them is in flight.
  for (std::filesystem::directory_iterator it(*dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.find(".tmp-") == std::string::npos) continue;
    std::error_code ignored;
    std::filesystem::remove(it->path(), ignored);
  }
  disk_.emplace(*dir);
}

void ShaderCache::disable_disk(const std::string& why) {
  std::fprintf(stderr, "[gpu] shader cache: %s; translated shaders stay in memory only from now on\n", why.c_str());
  disk_.reset();
}

uint64_t ShaderCache::load(ShaderStage stage, const uint8_t* guest_microcode_be, uint32_t dwords) {
  const uint64_t hash = shader::microcode_hash(guest_microcode_be, dwords);
  const shader::ShaderKey k = loaded_key(stage, hash, dwords);
  if (loaded_.contains(k)) return hash;
  Loaded l;
  l.microcode.resize(dwords);
  for (uint32_t i = 0; i < dwords; ++i) {
    uint32_t v;
    std::memcpy(&v, guest_microcode_be + 4 * size_t{i}, 4);
    l.microcode[i] = __builtin_bswap32(v);
  }
  auto program = shader::decode(stage, l.microcode);
  if (program) l.program = std::move(program.value());
  else l.decode_error = program.error().message;
  if (l.program && stage == ShaderStage::Pixel) l.inputs = shader::shader_info(*l.program, {}).interpolators_read;
  loaded_.emplace(k, std::move(l));
  return hash;
}

uint32_t ShaderCache::referenced_interpolators(ShaderStage stage, uint64_t hash, uint32_t dwords) const {
  const auto it = loaded_.find(loaded_key(stage, hash, dwords));
  return it == loaded_.end() ? 0 : it->second.inputs;
}

Result<const shader::TranslatedShader*> ShaderCache::remember(const shader::ShaderKey& key, Entry entry) {
  auto [it, inserted] = translated_.emplace(key, std::move(entry));
  (void)inserted;
  if (!it->second.shader) return Error{ErrorCode::InvalidFormat, it->second.error, ""};
  return static_cast<const shader::TranslatedShader*>(it->second.shader.get());
}

Result<const shader::TranslatedShader*> ShaderCache::translate(const shader::ShaderKey& key) {
  if (auto it = translated_.find(key); it != translated_.end()) {
    last_origin_ = Origin::Memory;
    if (!it->second.shader) return Error{ErrorCode::InvalidFormat, it->second.error, ""};
    return static_cast<const shader::TranslatedShader*>(it->second.shader.get());
  }
  last_origin_ = Origin::Translator;
  const auto lit = loaded_.find(loaded_key(key.stage, key.microcode_hash, key.dwords));
  if (lit == loaded_.end()) {
    // GpuSystem only builds keys from loaded microcode; a caller that did not is a bug, not the guest's fault.
    return Error{ErrorCode::Internal, "the microcode was never loaded", ""};
  }
  const Loaded& loaded = lit->second;
  if (!loaded.program) {
    const std::string error = "decode: " + loaded.decode_error;
    dump(key, loaded, error);
    return remember(key, Entry{nullptr, error});
  }
  const shader::TranslateOptions options = shader::key_options(key);

  if (disk_) {
    if (auto spirv = disk_->load(key, loaded.microcode)) {
      last_origin_ = Origin::Disk;
      auto t = std::make_unique<shader::TranslatedShader>();
      t->spirv = std::move(*spirv);
      t->info = shader::shader_info(*loaded.program, options);
      dump(key, loaded, {}, &t->spirv);
      return remember(key, Entry{std::move(t), {}});
    }
  }

  auto translated = shader::translate(*loaded.program, options);
  if (!translated) {
    dump(key, loaded, translated.error().message);
    return remember(key, Entry{nullptr, translated.error().message});
  }
  auto t = std::make_unique<shader::TranslatedShader>(std::move(translated.value()));
  if (disk_) {
    if (auto r = disk_->store(key, loaded.microcode, t->spirv); !r) disable_disk(r.error().message);
  }
  dump(key, loaded, {}, &t->spirv);
  return remember(key, Entry{std::move(t), {}});
}

void ShaderCache::dump(const shader::ShaderKey& key, const Loaded& loaded, const std::string& error,
                       const std::vector<uint32_t>* spirv) const {
  if (!dump_ || !disk_) return;
  std::filesystem::path file = disk_->path_of(key);
  file.replace_extension(".txt");
  std::ofstream out(file, std::ios::trunc);
  char head[160];
  std::snprintf(head, sizeof head, "; %s shader 0x%016llX, %u dwords, modifiers 0x%016llX\n", stage_name(key.stage),
                static_cast<unsigned long long>(key.microcode_hash), key.dwords,
                static_cast<unsigned long long>(key.modifiers));
  out << head;
  if (!error.empty()) out << "; cannot be translated: " << error << "\n";
  if (loaded.program) {
    out << shader::disassemble(*loaded.program);
  } else {
    for (size_t i = 0; i < loaded.microcode.size(); ++i) {
      char w[16];
      std::snprintf(w, sizeof w, "%08X%c", loaded.microcode[i], i % 3 == 2 ? '\n' : ' ');
      out << w;
    }
    out << "\n";
  }
  if (!out) std::fprintf(stderr, "[gpu] --dump-shaders: cannot write %s\n", file.c_str());
  if (!spirv) return;
  // The bare module (no LRSV header) for spirv-dis / spirv-val.
  file.replace_extension(".raw.spv");
  std::ofstream raw(file, std::ios::binary | std::ios::trunc);
  raw.write(reinterpret_cast<const char*>(spirv->data()), static_cast<std::streamsize>(spirv->size() * 4));
  if (!raw) std::fprintf(stderr, "[gpu] --dump-shaders: cannot write %s\n", file.c_str());
}

}  // namespace lr::gpu
