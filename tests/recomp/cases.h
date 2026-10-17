// SPDX-License-Identifier: MIT
// Synthetic PowerPC functions recompiled by XenonRecomp at build time (see CMakeLists.txt) and executed by
// test_recomp_instructions.cpp. Shared between the fixture generator and the tests.
#pragma once

#include <cstdint>
#include <vector>

namespace lr::test {

inline constexpr uint32_t kBlr = 0x4E800020;

struct RecompCase {
  const char* name;
  uint32_t address;             // guest address of the function (one 0x100 slot per case from 0x82000000)
  std::vector<uint32_t> words;  // big-endian instruction words; every case ends with blr
};

inline const std::vector<RecompCase>& recomp_cases() {
  static const std::vector<RecompCase> cases = {
      {"addi baseline", 0x82000000, {0x38630005 /* addi r3,r3,5 */, kBlr}},
  };
  return cases;
}

}  // namespace lr::test
