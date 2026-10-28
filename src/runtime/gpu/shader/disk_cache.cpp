// SPDX-License-Identifier: MIT
#include "runtime/gpu/shader/disk_cache.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#include "runtime/gpu/shader/translator.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace lr::gpu::shader {

namespace {

constexpr char kMagic[4] = {'L', 'R', 'S', 'V'};
constexpr uint32_t kSpirvMagic = 0x07230203;
// No translated shader comes near this; a larger file is not ours (and is never read whole).
constexpr uint64_t kMaxFileBytes = 64ull << 20;

void put32(std::vector<uint8_t>& out, size_t at, uint32_t v) { std::memcpy(out.data() + at, &v, 4); }
void put64(std::vector<uint8_t>& out, size_t at, uint64_t v) { std::memcpy(out.data() + at, &v, 8); }
uint32_t get32(const std::vector<uint8_t>& in, size_t at) {
  uint32_t v;
  std::memcpy(&v, in.data() + at, 4);
  return v;
}
uint64_t get64(const std::vector<uint8_t>& in, size_t at) {
  uint64_t v;
  std::memcpy(&v, in.data() + at, 8);
  return v;
}

void warn(const std::filesystem::path& file, const std::string& why) {
  std::fprintf(stderr, "[gpu] shader cache: ignoring %s (%s); translating again\n", file.c_str(), why.c_str());
}

}  // namespace

DiskCache::DiskCache(std::filesystem::path dir) : dir_(std::move(dir)) {}

std::optional<std::vector<uint32_t>> DiskCache::load(const ShaderKey& key, std::span<const uint32_t> microcode) const {
  const std::filesystem::path file = path_of(key);
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) return std::nullopt;
  const uint64_t size = std::filesystem::file_size(file, ec);
  if (ec) {
    warn(file, "cannot stat: " + ec.message());
    return std::nullopt;
  }
  const uint64_t expect_min = kDiskCacheHeaderBytes + uint64_t{microcode.size()} * 4;
  if (size < expect_min + 20 || size > kMaxFileBytes || size % 4 != 0) {  // 20: a SPIR-V header
    warn(file, "bad size " + std::to_string(size));
    return std::nullopt;
  }
  std::vector<uint8_t> bytes(size);
  {
    std::ifstream in(file, std::ios::binary);
    if (!in || !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
      warn(file, "cannot read");
      return std::nullopt;
    }
  }
  if (std::memcmp(bytes.data(), kMagic, 4) != 0) {
    warn(file, "bad magic");
    return std::nullopt;
  }
  if (get32(bytes, 4) != kTranslatorVersion) {
    warn(file, "translator version " + std::to_string(get32(bytes, 4)));
    return std::nullopt;
  }
  if (get32(bytes, 8) != (key.stage == ShaderStage::Vertex ? 0u : 1u) || get32(bytes, 12) != key.dwords ||
      get64(bytes, 16) != key.microcode_hash || get64(bytes, 24) != key.modifiers ||
      key.dwords != microcode.size()) {
    warn(file, "the header names another key");
    return std::nullopt;
  }
  const uint64_t spirv_words = get32(bytes, 32);
  if (expect_min + spirv_words * 4 != size) {
    warn(file, "truncated or oversized");
    return std::nullopt;
  }
  if (get64(bytes, 40) != XXH3_64bits(bytes.data() + kDiskCacheHeaderBytes, size - kDiskCacheHeaderBytes)) {
    warn(file, "checksum mismatch");
    return std::nullopt;
  }
  if (!microcode.empty() &&
      std::memcmp(bytes.data() + kDiskCacheHeaderBytes, microcode.data(), microcode.size() * 4) != 0) {
    warn(file, "same hash, different microcode");
    return std::nullopt;
  }
  std::vector<uint32_t> spirv(spirv_words);
  std::memcpy(spirv.data(), bytes.data() + expect_min, spirv_words * 4);
  if (spirv.empty() || spirv[0] != kSpirvMagic) {
    warn(file, "not a SPIR-V module");
    return std::nullopt;
  }
  return spirv;
}

Result<void> DiskCache::store(const ShaderKey& key, std::span<const uint32_t> microcode,
                              std::span<const uint32_t> spirv) {
  const size_t body = (microcode.size() + spirv.size()) * 4;
  std::vector<uint8_t> bytes(kDiskCacheHeaderBytes + body, 0);
  std::memcpy(bytes.data(), kMagic, 4);
  put32(bytes, 4, kTranslatorVersion);
  put32(bytes, 8, key.stage == ShaderStage::Vertex ? 0u : 1u);
  put32(bytes, 12, key.dwords);
  put64(bytes, 16, key.microcode_hash);
  put64(bytes, 24, key.modifiers);
  put32(bytes, 32, static_cast<uint32_t>(spirv.size()));
  if (!microcode.empty()) std::memcpy(bytes.data() + kDiskCacheHeaderBytes, microcode.data(), microcode.size() * 4);
  if (!spirv.empty())
    std::memcpy(bytes.data() + kDiskCacheHeaderBytes + microcode.size() * 4, spirv.data(), spirv.size() * 4);
  put64(bytes, 40, XXH3_64bits(bytes.data() + kDiskCacheHeaderBytes, body));

  const std::filesystem::path file = path_of(key);
  std::filesystem::path temp = file;
  temp += ".tmp-" + std::to_string(::getpid()) + "-" + std::to_string(temp_counter_++);
  {
    // POSIX calls, so the reported errno is the failing call's own (an ofstream does not promise to keep it).
    const auto fail = [&](const char* what, int err) -> Error {
      std::error_code ignored;
      std::filesystem::remove(temp, ignored);
      return Error{ErrorCode::IoError, std::string("cannot ") + what + " " + temp.string() + ": " + std::strerror(err),
                   ""};
    };
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return fail("create", errno);
    size_t done = 0;
    while (done < bytes.size()) {
      const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) {
        const int err = n < 0 ? errno : EIO;
        ::close(fd);
        return fail("write", err);
      }
      done += static_cast<size_t>(n);
    }
    if (::close(fd) != 0) return fail("close", errno);
  }
  std::error_code ec;
  std::filesystem::rename(temp, file, ec);
  if (ec) {
    std::error_code ignored;
    std::filesystem::remove(temp, ignored);
    return Error{ErrorCode::IoError, "cannot rename " + temp.string() + " to " + file.string() + ": " + ec.message(),
                 ""};
  }
  return {};
}

}  // namespace lr::gpu::shader
