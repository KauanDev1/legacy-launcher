// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/result.h"

namespace lr::pipeline {

// Compiler, CMake and Ninja the build stage uses to compile the game.
struct Toolchain {
  std::filesystem::path cc, cxx, cmake, ninja;
  std::vector<std::string> extra_cxx_flags;          // appended to CMAKE_CXX_FLAGS of the game template
  std::optional<std::filesystem::path> sdl2_prefix;  // CMAKE_PREFIX_PATH for find_package(SDL2)
  bool bundled = false;
};

// Bundled mode (the AppImage): <resources>/toolchain/ exists and holds
//   bin/cc, bin/c++        wrappers running `zig cc|c++ -target x86_64-linux-gnu.2.31`
//   bin/ninja              Ninja
//   cmake/bin/cmake        CMake (with its cmake/share/cmake-X.Y modules)
//   zig/zig                the Zig compiler (zig/lib holds clang's headers, libc++ and the glibc stubs)
//   sdl2/                  static SDL2: include/, lib/libSDL2.a, lib/cmake/SDL2/SDL2Config.cmake
// Every one of these must be present, else ToolMissing with a "corrupted AppImage" hint.
// Otherwise (system mode) clang, clang++, cmake and ninja are looked up on PATH; ToolMissing with the package
// manager hint when one is missing.
Result<Toolchain> resolve_toolchain(const std::filesystem::path& resources);

bool has_bundled_toolchain(const std::filesystem::path& resources);

// Package manager commands that install the system toolchain (pacman, apt, dnf).
std::string_view system_toolchain_hint();

}  // namespace lr::pipeline
