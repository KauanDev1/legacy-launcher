// SPDX-License-Identifier: MIT
// Build-time tool: writes a synthetic big-endian ELF with one function per case plus the recomp.toml that
// drives XenonRecomp over it. Usage: lr_gen_recomp_fixture <output dir>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cases.h"
#include "support/elf_builder.h"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: lr_gen_recomp_fixture <output dir>\n");
    return 2;
  }
  const auto& cases = lr::test::recomp_cases();
  // XenonRecomp writes 256 functions per ppc_recomp.N.cpp; CMakeLists.txt only lists ppc_recomp.0.cpp.
  if (cases.empty() || cases.size() > 256) {
    std::fprintf(stderr, "lr_gen_recomp_fixture: need 1..256 cases, got %zu\n", cases.size());
    return 1;
  }

  std::vector<lr::test::ElfSection> sections;
  std::string functions;
  for (const auto& c : cases) {
    lr::test::ElfSection s;
    s.name = ".text";
    s.addr = c.address;
    for (uint32_t w : c.words)
      for (int shift = 24; shift >= 0; shift -= 8) s.bytes.push_back(uint8_t(w >> shift));
    functions += "  { address = " + std::to_string(c.address) + ", size = " + std::to_string(s.bytes.size()) + " },\n";
    sections.push_back(std::move(s));
  }
  // XenonRecomp dereferences Image::Find(".pdata") unconditionally; an empty, non-executable one is enough.
  sections.push_back({".pdata", 0x82F00000, {}, false});
  const auto elf = lr::test::build_elf(cases.front().address, sections);

  const fs::path dir = argv[1];
  std::error_code ec;
  fs::create_directories(dir / "out", ec);
  std::ofstream(dir / "image.elf", std::ios::binary)
      .write(reinterpret_cast<const char*>(elf.data()), std::streamsize(elf.size()));
  std::ofstream(dir / "switch.toml").flush();  // empty: no switch tables
  std::ofstream toml(dir / "recomp.toml");
  toml << "[main]\nfile_path = \"image.elf\"\nout_directory_path = \"out\"\nswitch_table_file_path = \"switch.toml\"\n"
       << "functions = [\n"
       << functions << "]\n";
  toml.flush();
  if (!toml.good()) {
    std::fprintf(stderr, "lr_gen_recomp_fixture: cannot write into %s\n", dir.string().c_str());
    return 1;
  }
  return 0;
}
