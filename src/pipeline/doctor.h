// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/result.h"

namespace lr::pipeline {

struct DoctorProbe {
  virtual ~DoctorProbe() = default;
  virtual std::optional<std::filesystem::path> find(std::string_view program) = 0;
  virtual Result<std::string> version_output(const std::filesystem::path& program) = 0;
  virtual Result<uint64_t> free_bytes(const std::filesystem::path& path) = 0;
  virtual uint64_t total_ram() = 0;
  virtual bool pkg_config_exists(std::string_view package) = 0;
  // The game is compiled with -march=sandybridge (src/runtime/template/CMakeLists.txt): the host CPU needs AVX.
  virtual bool cpu_supports_avx() = 0;
};

// Probe backed by the real system (PATH lookup, statvfs, sysconf, pkg-config).
struct SystemDoctorProbe : DoctorProbe {
  std::optional<std::filesystem::path> find(std::string_view program) override;
  Result<std::string> version_output(const std::filesystem::path& program) override;
  Result<uint64_t> free_bytes(const std::filesystem::path& path) override;
  uint64_t total_ram() override;
  bool pkg_config_exists(std::string_view package) override;
  bool cpu_supports_avx() override;  // CPUID/XGETBV (AVX usable) on x86-64; true elsewhere
};

struct DoctorReport {
  std::vector<std::string> warnings;
};

// Requires a CPU with AVX (InsufficientResources), a toolchain and 8 GiB free at root (or its nearest existing
// parent). Less than 8 GiB of RAM only produces a warning. Toolchain: when <resources>/toolchain exists (the
// AppImage, see resolve_toolchain) every bundled file must be present, else ToolMissing with a "corrupted AppImage"
// hint; otherwise clang++ >= 18, cmake >= 3.25, ninja and pkg-config sdl2 from the system (probe).
Result<DoctorReport> run_doctor(DoctorProbe& probe, const std::filesystem::path& root,
                                const std::filesystem::path& resources = {});

}  // namespace lr::pipeline
