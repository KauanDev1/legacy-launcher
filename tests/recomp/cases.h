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
      // Branch cases share one layout: word 0 is the branch (target +12), then `li r3,1; blr` is the fall-through
      // and `li r3,2; blr` the taken path, so r3 tells which way it went. li r3,1 = 0x38600001, li r3,2 = 0x38600002.
      // B-form: opcode 16 (0x40000000) | BO<<21 | BI<<16 | BD (byte offset 12 = 0xC), AA=LK=0.
      // bdzf  BO=0b00010: 0x40400000 | BI<<16 | 0xC.  BI = 4*crN + {0 lt, 1 gt, 2 eq, 3 so}.
      {"bdzf cr0.lt", 0x82000100, {0x4040000C, 0x38600001, kBlr, 0x38600002, kBlr}},  // bdzf 0,+12
      {"bdzf cr0.gt", 0x82000200, {0x4041000C, 0x38600001, kBlr, 0x38600002, kBlr}},  // bdzf 1,+12
      {"bdzf cr0.eq", 0x82000300, {0x4042000C, 0x38600001, kBlr, 0x38600002, kBlr}},  // bdzf 2,+12
      {"bdzf cr0.so", 0x82000400, {0x4043000C, 0x38600001, kBlr, 0x38600002, kBlr}},  // bdzf 3,+12
      {"bdzf cr2.gt", 0x82000500, {0x4049000C, 0x38600001, kBlr, 0x38600002, kBlr}},  // bdzf 9,+12
      // bdnzf BO=0b00000: 0x40000000 | BI<<16 | 0xC.
      {"bdnzf cr0.gt", 0x82000600, {0x4001000C, 0x38600001, kBlr, 0x38600002, kBlr}},  // bdnzf 1,+12
      {"bdnzf cr0.eq", 0x82000700, {0x4002000C, 0x38600001, kBlr, 0x38600002, kBlr}},  // bdnzf 2,+12
      // bso = bc 12 (BO=0b01100 -> 0x01800000), BI=4*crN+3.
      {"bso cr0", 0x82000800, {0x4183000C, 0x38600001, kBlr, 0x38600002, kBlr}},  // bso cr0,+12 (bc 12,3)
      {"bso cr1", 0x82000900, {0x4187000C, 0x38600001, kBlr, 0x38600002, kBlr}},  // bso cr1,+12 (bc 12,7)
      // fcmpu cr0,f1,f2 = 0xFC011000, then bso cr0,+12 (word 1; target is word 4, the `li r3,2`).
      {"fcmpu then bso", 0x82000A00, {0xFC011000, 0x4183000C, 0x38600001, kBlr, 0x38600002, kBlr}},
      // Load/store with update. D-form: opcode<<26 | reg<<21 | rA<<16 | d; X-form: 0x7C000000 | reg<<21 | rA<<16 |
      // rB<<11 | XO<<1. Opcodes: lfsu 49 (0xC4000000), stfsu 53 (0xD4000000), lhzu 41 (0xA4000000), sthu 45
      // (0xB4000000). XO: lfsux 567 (0x46E), stfsux 695 (0x56E).
      {"lfsu", 0x82000B00, {0xC4230008, kBlr}},    // lfsu f1,8(r3)
      {"lfsux", 0x82000C00, {0x7C23246E, kBlr}},   // lfsux f1,r3,r4
      {"stfsu", 0x82000D00, {0xD421FFF8, kBlr}},   // stfsu f1,-8(r1)
      {"stfsux", 0x82000E00, {0x7C23256E, kBlr}},  // stfsux f1,r3,r4
      {"lhzu", 0x82000F00, {0xA4850002, kBlr}},    // lhzu r4,2(r5)
      {"sthu", 0x82001000, {0xB485FFFE, kBlr}},    // sthu r4,-2(r5)
      // X-form ALU: 0x7C000000 | S<<21 | A<<16 | B<<11 | XO<<1 | Rc. eqv XO=284 (0x238); subfze XO=200 (0x190, OE=0),
      // for which rB=0 and the destination is RT<<21.
      {"eqv", 0x82001100, {0x7C832A38, kBlr}},    // eqv r3,r4,r5   (S=4, A=3, B=5)
      {"eqv.", 0x82001200, {0x7C832A39, kBlr}},   // eqv. r3,r4,r5
      {"subfze", 0x82001300, {0x7C640190, kBlr}},   // subfze r3,r4   (RT=3, RA=4)
      {"subfze.", 0x82001400, {0x7C640191, kBlr}},  // subfze. r3,r4
      // vsel128 = VX128(5, 848): opcode 5 (0x14000000) | 0x350. VMX128 splits 7-bit register numbers across the
      // word (see insert_*128 in the disassembler): VD128 = ((v&0x60)>>3) | ((v&0x1f)<<21), VA128 = ((v&0x40)<<4) |
      // (v&0x20) | ((v&0x1f)<<16), VB128 = ((v&0x60)>>5) | ((v&0x1f)<<11). VS128 is VD128, so the mask is vD.
      // vD=v64 (0x8), vA=v1 (1<<16), vB=v2 (2<<11): 0x14000350|0x8|0x10000|0x1000 = 0x14011358.
      {"vsel128", 0x82001500, {0x14011358, kBlr}},  // vsel128 v64,v1,v2,v64
  };
  return cases;
}

}  // namespace lr::test
