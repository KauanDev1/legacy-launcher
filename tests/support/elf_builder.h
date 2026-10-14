// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace lr::test {

// Builds a minimal big-endian ELF32 (one PT_LOAD, one executable section) that XenonUtils'
// Image::ParseImage accepts. Used as a loadable synthetic "guest image" in tests.
struct ElfSection {
  std::string name;
  uint32_t addr;                // absolute guest address
  std::vector<uint8_t> bytes;
  bool exec = true;
};

inline std::vector<uint8_t> build_elf(uint32_t entry, const std::vector<ElfSection>& secs) {
  std::vector<uint8_t> f(52 + 32, 0);
  auto put32 = [&](size_t off, uint32_t v) {
    f[off] = uint8_t(v >> 24); f[off + 1] = uint8_t(v >> 16); f[off + 2] = uint8_t(v >> 8); f[off + 3] = uint8_t(v);
  };
  auto put16 = [&](size_t off, uint16_t v) { f[off] = uint8_t(v >> 8); f[off + 1] = uint8_t(v); };
  auto push32 = [&](std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x));
  };

  // Section payloads, then the string table.
  std::vector<uint32_t> offsets;
  for (const auto& s : secs) {
    offsets.push_back(uint32_t(f.size()));
    f.insert(f.end(), s.bytes.begin(), s.bytes.end());
  }
  std::string strtab(1, '\0');
  std::vector<uint32_t> name_off;
  for (const auto& s : secs) { name_off.push_back(uint32_t(strtab.size())); strtab += s.name + '\0'; }
  const uint32_t shstr_name = uint32_t(strtab.size());
  strtab += ".shstrtab";
  strtab += '\0';
  const uint32_t strtab_off = uint32_t(f.size());
  f.insert(f.end(), strtab.begin(), strtab.end());
  while (f.size() % 4) f.push_back(0);

  const uint32_t shoff = uint32_t(f.size());
  const uint32_t nsh = uint32_t(secs.size()) + 2;
  std::vector<uint8_t> sh(40, 0);  // null section
  uint32_t base = secs.empty() ? 0 : secs[0].addr;
  uint32_t end = base;
  for (size_t i = 0; i < secs.size(); ++i) {
    push32(sh, name_off[i]); push32(sh, 1); push32(sh, secs[i].exec ? 6u : 2u); push32(sh, secs[i].addr);
    push32(sh, offsets[i]); push32(sh, uint32_t(secs[i].bytes.size())); push32(sh, 0); push32(sh, 0);
    push32(sh, 4); push32(sh, 0);
    end = std::max(end, secs[i].addr + uint32_t(secs[i].bytes.size()));
  }
  // .shstrtab is mapped by XenonUtils too, so give it an address inside the image.
  push32(sh, shstr_name); push32(sh, 3); push32(sh, 0); push32(sh, end);
  push32(sh, strtab_off); push32(sh, uint32_t(strtab.size())); push32(sh, 0); push32(sh, 0); push32(sh, 1); push32(sh, 0);
  f.insert(f.end(), sh.begin(), sh.end());

  const uint8_t ident[16] = {0x7F, 'E', 'L', 'F', 1, 2, 1, 0};
  std::memcpy(f.data(), ident, 16);
  put16(16, 2); put16(18, 20); put32(20, 1); put32(24, entry); put32(28, 52); put32(32, shoff); put32(36, 0);
  put16(40, 52); put16(42, 32); put16(44, 1); put16(46, 40); put16(48, uint16_t(nsh)); put16(50, uint16_t(nsh - 1));
  put32(52, 1);  // PT_LOAD
  put32(52 + 4, 0); put32(52 + 8, base); put32(52 + 12, base);
  put32(52 + 16, end - base); put32(52 + 20, end - base); put32(52 + 24, 5); put32(52 + 28, 4);
  return f;
}

}  // namespace lr::test
