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

// vpkd3d128 vD,vB,type,pack,shift = VX128_4(6, 1552): 0x18000610 | VD128 | VB128 | type<<18 | pack<<16 | shift<<6
// (fields VD3D0/VD3D1/VD3D2 of the disassembler). Only v0..v31 here, so VD128 = vd<<21 and VB128 = vb<<11.
constexpr uint32_t vpkd3d128(uint32_t vd, uint32_t vb, uint32_t type, uint32_t pack, uint32_t shift) {
  return 0x18000610u | (vd << 21) | (vb << 11) | (type << 18) | (pack << 16) | (shift << 6);
}
inline constexpr uint32_t kFloat16x4 = 5;  // VPACK_FLOAT16_4

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
      // vpkd3d128 FLOAT16_4 (type 5) packs v4's four floats into two words, XY = x<<16|y and ZW = z<<16|w, and
      // merges them into v3 according to (pack, shift); see the test file for the expected layout.
      {"f16x4 p2 s0", 0x82001600, {vpkd3d128(3, 4, kFloat16x4, 2, 0), kBlr}},
      {"f16x4 p2 s1", 0x82001700, {vpkd3d128(3, 4, kFloat16x4, 2, 1), kBlr}},
      {"f16x4 p2 s2", 0x82001800, {vpkd3d128(3, 4, kFloat16x4, 2, 2), kBlr}},
      {"f16x4 p2 s3", 0x82001900, {vpkd3d128(3, 4, kFloat16x4, 2, 3), kBlr}},
      {"f16x4 p3 s0", 0x82001A00, {vpkd3d128(3, 4, kFloat16x4, 3, 0), kBlr}},
      {"f16x4 p3 s3", 0x82001B00, {vpkd3d128(3, 4, kFloat16x4, 3, 3), kBlr}},
      {"f16x4 p1 s0", 0x82001C00, {vpkd3d128(3, 4, kFloat16x4, 1, 0), kBlr}},
      {"f16x4 p1 s1", 0x82001D00, {vpkd3d128(3, 4, kFloat16x4, 1, 1), kBlr}},
      {"f16x4 p1 s2", 0x82001E00, {vpkd3d128(3, 4, kFloat16x4, 1, 2), kBlr}},
      {"f16x4 p1 s3", 0x82001F00, {vpkd3d128(3, 4, kFloat16x4, 1, 3), kBlr}},
      {"f16x4 p2 s2 same reg", 0x82002000, {vpkd3d128(3, 3, kFloat16x4, 2, 2), kBlr}},
      // Integer carry/multiply (XO-form: 0x7C000000 | RT<<21 | RA<<16 | RB<<11 | XO<<1 | Rc), RT=3, RA=4, RB=5.
      // addc XO=10, addme XO=234 (RB=0), mulhd XO=73.
      {"addc", 0x82002100, {0x7C642814, kBlr}},    // addc r3,r4,r5
      {"addc.", 0x82002200, {0x7C642815, kBlr}},   // addc. r3,r4,r5
      {"addme", 0x82002300, {0x7C6401D4, kBlr}},   // addme r3,r4
      {"addme.", 0x82002400, {0x7C6401D5, kBlr}},  // addme. r3,r4
      {"mulhd", 0x82002500, {0x7C642892, kBlr}},   // mulhd r3,r4,r5
      // Indexed loads/stores with update (X-form, reg=4, rA=3, rB=5: 0x7C832800 | XO<<1). XO: lhzux 311, lbzux 119,
      // lwzux 55, ldux 53, lfdux 631, sthux 439, stbux 247, stdux 181; lhbrx 790 and lvehx 39 have no update.
      {"lhzux", 0x82002600, {0x7C832A6E, kBlr}},   // lhzux r4,r3,r5
      {"lbzux", 0x82002700, {0x7C8328EE, kBlr}},   // lbzux r4,r3,r5
      {"lwzux", 0x82002800, {0x7C83286E, kBlr}},   // lwzux r4,r3,r5
      {"ldux", 0x82002900, {0x7C83286A, kBlr}},    // ldux r4,r3,r5
      {"lfdux", 0x82002A00, {0x7C832CEE, kBlr}},   // lfdux f4,r3,r5
      {"sthux", 0x82002B00, {0x7C832B6E, kBlr}},   // sthux r4,r3,r5
      {"stbux", 0x82002C00, {0x7C8329EE, kBlr}},   // stbux r4,r3,r5
      {"stdux", 0x82002D00, {0x7C83296A, kBlr}},   // stdux r4,r3,r5
      {"lhbrx", 0x82002E00, {0x7C832E2C, kBlr}},   // lhbrx r4,r3,r5
      {"lvehx", 0x82002F00, {0x7C83284E, kBlr}},   // lvehx v4,r3,r5
      // D-form with update: lfdu 51 (0xCC000000), stfdu 55 (0xDC000000), lhau 43 (0xAC000000).
      {"lfdu", 0x82003000, {0xCC230008, kBlr}},    // lfdu f1,8(r3)
      {"stfdu", 0x82003100, {0xDC21FFF8, kBlr}},   // stfdu f1,-8(r1)
      {"lhau", 0x82003200, {0xAC850002, kBlr}},    // lhau r4,2(r5)
      // Branches (layout as above): bns = bc 4,BI (BO=0b00100), BI=4*crN+3; bdnzt BO=0b01000.
      {"bns cr0", 0x82003300, {0x4083000C, 0x38600001, kBlr, 0x38600002, kBlr}},    // bns cr0,+12
      {"bns cr1", 0x82003400, {0x4087000C, 0x38600001, kBlr, 0x38600002, kBlr}},    // bns cr1,+12
      {"bdnzt cr0.gt", 0x82003500, {0x4101000C, 0x38600001, kBlr, 0x38600002, kBlr}},  // bdnzt 1,+12
      // VMX (VX-form, opcode 4): 0x10000000 | vD<<21 | vA<<16 | vB<<11 | XO with vD=3, vA=4, vB=5 (0x10642800).
      {"vmaxuh", 0x82003600, {0x10642842, kBlr}},     // XO 66
      {"vminuh", 0x82003700, {0x10642A42, kBlr}},     // XO 578
      {"vadduhs", 0x82003800, {0x10642A40, kBlr}},    // XO 576
      {"vsubuws", 0x82003900, {0x10642E80, kBlr}},    // XO 1664
      {"vavguw", 0x82003A00, {0x10642C82, kBlr}},     // XO 1154
      {"vslh", 0x82003B00, {0x10642944, kBlr}},       // XO 324
      {"vsl", 0x82003C00, {0x106429C4, kBlr}},        // XO 452
      {"vcmpequh", 0x82003D00, {0x10642846, kBlr}},   // VXR 70, Rc=0
      {"vcmpequh.", 0x82003E00, {0x10642C46, kBlr}},  // VXR 70, Rc=1 (0x400)
      {"vcmpgtuw", 0x82003F00, {0x10642A86, kBlr}},   // VXR 646
      {"vcmpgtuw.", 0x82004000, {0x10642E86, kBlr}},
      {"vctuxs 0", 0x82004100, {0x10602B8A, kBlr}},   // vctuxs v3,v5,0 (XO 906, UIMM in the vA field)
      {"vctuxs 2", 0x82004200, {0x10622B8A, kBlr}},   // vctuxs v3,v5,2
      // VMX128 (opcode 5): VX128 = 0x14000000 | (xop & 0x3D0), v3,v4,v5 = 0x14642800.
      {"vpkswss128", 0x82004300, {0x14642A80, kBlr}},  // xop 640
      {"vpkuwum128", 0x82004400, {0x14642B80, kBlr}},  // xop 896
      {"vpkuwus128", 0x82004500, {0x14642BC0, kBlr}},  // xop 960
      {"vpkuhum128", 0x82004600, {0x14642B00, kBlr}},  // xop 768
      {"vpkuhus128", 0x82004700, {0x14642B40, kBlr}},  // xop 832
      {"vnor128", 0x82004800, {0x14642A90, kBlr}},     // xop 656
      {"vslo128", 0x82004900, {0x14642B90, kBlr}},     // xop 912
      // Opcode 6: vrlw128 = VX128(6, 80) = 0x18000050; vcfpuxws128 v3,v5,UIMM = VX128_3(6, 624) = 0x18000270.
      {"vrlw128", 0x82004A00, {0x18642850, kBlr}},
      {"vcfpuxws128 2", 0x82004B00, {0x18622A70, kBlr}},
  };
  return cases;
}

}  // namespace lr::test
