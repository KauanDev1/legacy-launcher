// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include "common/result.h"

namespace lr::pipeline {

// C++ compiler, CMake and Ninja the build stage uses to compile the game (the template is CXX-only: no C compiler).
struct Toolchain {
  std::filesystem::path cxx, cmake, ninja;
  std::optional<std::filesystem::path> sdl2_prefix;  // CMAKE_PREFIX_PATH for find_package(SDL2)
};

// Bundled mode (the AppImage): <resources>/toolchain/ exists and holds
//   bin/c++                wrapper running `zig c++ -target x86_64-linux-gnu.2.31` (build.sh also ships bin/cc,
//                          which the game build does not use and is not checked)
//   bin/ninja              Ninja
//   cmake/bin/cmake        CMake (with its cmake/share/cmake-X.Y modules)
//   zig/zig                the Zig compiler (zig/lib holds clang's headers, libc++ and the glibc stubs)
//   sdl2/                  static SDL2: include/, lib/libSDL2.a, lib/cmake/SDL2/SDL2Config.cmake
// Every one of these must be present, else ToolMissing with a "corrupted AppImage" hint.
// Otherwise (system mode) clang++, cmake and ninja are looked up on PATH (the tools run_doctor checks); ToolMissing
// with the package manager hint when one is missing.
Result<Toolchain> resolve_toolchain(const std::filesystem::path& resources);

bool has_bundled_toolchain(const std::filesystem::path& resources);

// DXC (HLSL -> SPIR-V, used by XenosRecomp at install time). Bundled mode: <resources>/toolchain/dxc/bin/dxc and
// <resources>/toolchain/dxc/lib/libdxcompiler.so must exist (the AppImage does not ship them yet, ADR 0005); a
// missing file is ToolMissing with a hint that says so. System mode uses `dxc` on PATH (see run_doctor).
Result<void> check_bundled_dxc(const std::filesystem::path& resources);

// Install hint for the system DXC (the directx-shader-compiler package).
std::string_view system_dxc_hint();

// Package manager commands that install the system toolchain (pacman, apt, dnf).
std::string_view system_toolchain_hint();

}  // namespace lr::pipeline
