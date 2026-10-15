// SPDX-License-Identifier: MIT
// Test helper: private temporary directories (mkdtemp), so test cases running in parallel under
// `ctest -j` never share a file name.
#pragma once

#include <stdlib.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace lr::testing {

// Creates <tmp>/<prefix>_XXXXXX with a unique suffix and returns it. The caller owns its removal.
inline std::filesystem::path make_temp_dir(std::string_view prefix) {
  std::string t = (std::filesystem::temp_directory_path() / (std::string(prefix) + "_XXXXXX")).string();
  REQUIRE(mkdtemp(t.data()) != nullptr);
  return t;
}

// RAII variant: the directory and everything in it is removed on destruction.
struct TempDir {
  std::filesystem::path path;
  explicit TempDir(std::string_view prefix = "lr_test") : path(make_temp_dir(prefix)) {}
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

}  // namespace lr::testing
