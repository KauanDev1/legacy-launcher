// SPDX-License-Identifier: MIT
#include "pipeline/toolchain.h"

#include <unistd.h>

#include <system_error>

#include "pipeline/process.h"

namespace lr::pipeline {
namespace fs = std::filesystem;

namespace {

// Exactly what the game build needs (clang++, cmake, ninja, SDL2 + pkg-config), i.e. what run_doctor checks.
constexpr std::string_view kInstallHint =
    "sudo pacman -S --needed clang cmake ninja sdl2 pkgconf\n"
    "sudo apt install clang cmake ninja-build libsdl2-dev pkg-config\n"
    "sudo dnf install clang cmake ninja-build SDL2-devel pkgconf";

constexpr const char* kCorruptedHint =
    "the toolchain bundled with LegacyRecomp is incomplete: the AppImage looks corrupted or truncated; download it "
    "again (or rebuild it with packaging/appimage/build.sh)";

Error bundled_missing(const fs::path& p) {
  return Error{ErrorCode::ToolMissing, "bundled toolchain file missing: " + p.string(), kCorruptedHint};
}

bool is_executable_file(const fs::path& p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec) && ::access(p.c_str(), X_OK) == 0;
}

}  // namespace

std::string_view system_toolchain_hint() { return kInstallHint; }

bool has_bundled_toolchain(const fs::path& resources) {
  std::error_code ec;
  return !resources.empty() && fs::is_directory(resources / "toolchain", ec);
}

Result<Toolchain> resolve_toolchain(const fs::path& resources) {
  Toolchain tc;
  if (has_bundled_toolchain(resources)) {
    const fs::path base = resources / "toolchain";
    tc.cxx = base / "bin" / "c++";
    tc.cmake = base / "cmake" / "bin" / "cmake";
    tc.ninja = base / "bin" / "ninja";
    tc.sdl2_prefix = base / "sdl2";
    for (const fs::path& p : {tc.cxx, tc.cmake, tc.ninja, base / "zig" / "zig"})
      if (!is_executable_file(p)) return bundled_missing(p);
    const fs::path sdl2_config = base / "sdl2" / "lib" / "cmake" / "SDL2" / "SDL2Config.cmake";
    std::error_code ec;
    if (!fs::is_regular_file(sdl2_config, ec)) return bundled_missing(sdl2_config);
    return tc;
  }

  auto lookup = [](std::string_view name, fs::path& out) -> Result<void> {
    auto p = find_program(name);
    if (!p) return Error{ErrorCode::ToolMissing, std::string(name) + " not found", std::string(kInstallHint)};
    out = *p;
    return {};
  };
  LR_TRY(lookup("clang++", tc.cxx));
  LR_TRY(lookup("cmake", tc.cmake));
  LR_TRY(lookup("ninja", tc.ninja));
  return tc;
}

}  // namespace lr::pipeline
