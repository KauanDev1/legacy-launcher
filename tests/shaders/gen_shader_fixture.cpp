// SPDX-License-Identifier: MIT
// Build-time tool: writes the synthetic shader containers of cases.h for the XenosRecomp harness.
// Usage: lr_gen_shader_fixture <output dir>   (good cases in <dir>/in, broken ones in <dir>/broken-*)
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "cases.h"

namespace fs = std::filesystem;

namespace {

bool write(const fs::path& path, const std::vector<uint8_t>& bytes) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(f);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: lr_gen_shader_fixture <output dir>\n");
    return 2;
  }
  const fs::path dir = argv[1];
  std::error_code ec;
  fs::remove_all(dir / "in", ec);
  for (const auto& c : lr::testing::shaders::good_cases())
    if (!write(dir / "in" / c.file, lr::testing::xenos::build_container(c.desc))) {
      std::fprintf(stderr, "lr_gen_shader_fixture: cannot write %s\n", c.file.c_str());
      return 1;
    }
  for (const auto& c : lr::testing::shaders::broken_cases())
    if (!write(dir / c.file, lr::testing::xenos::build_container(c.desc))) {
      std::fprintf(stderr, "lr_gen_shader_fixture: cannot write %s\n", c.file.c_str());
      return 1;
    }
  return 0;
}
