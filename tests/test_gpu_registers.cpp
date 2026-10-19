// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>

#include "runtime/gpu/registers.h"

using namespace lr::gpu;

TEST_CASE("named registers have the documented indices", "[unit][gpu]") {
  CHECK(CP_RB_BASE == 0x01C0);
  CHECK(CP_RB_WPTR == 0x01C5);
  CHECK(SCRATCH_REG0 == 0x0578);
  CHECK(SCRATCH_REG7 == 0x057F);
  CHECK(RB_SURFACE_INFO == 0x2000);
  CHECK(PA_SC_WINDOW_OFFSET == 0x2080);
  CHECK(PA_CL_VPORT_XSCALE == 0x210F);
  CHECK(SQ_PROGRAM_CNTL == 0x2180);
  CHECK(VGT_DRAW_INITIATOR == 0x21FC);
  CHECK(RB_COPY_CONTROL == 0x2318);
  CHECK(SHADER_CONSTANT_000_X == 0x4000);
  CHECK(SHADER_CONSTANT_FETCH_00_0 == 0x4800);
  CHECK(SHADER_CONSTANT_BOOL_000_031 == 0x4900);
  CHECK(SHADER_CONSTANT_LOOP_00 == 0x4908);
  // Range bounds agree with the count constants.
  CHECK(kShaderConstantFloatBase + kShaderConstantFloatCount - 1 == SHADER_CONSTANT_511_W);
  CHECK(kShaderConstantFetchBase + kShaderConstantFetchCount * kShaderConstantFetchRegsPerConstant - 1 ==
        SHADER_CONSTANT_FETCH_31_5);
  CHECK(kShaderConstantBoolBase + kShaderConstantBoolRegCount - 1 == SHADER_CONSTANT_BOOL_224_255);
  CHECK(kShaderConstantLoopBase + kShaderConstantLoopCount - 1 == SHADER_CONSTANT_LOOP_31);
}

TEST_CASE("register names resolve and unnamed indices give nullptr", "[unit][gpu]") {
  CHECK(std::strcmp(RegisterFile::name(RB_COLOR_INFO), "RB_COLOR_INFO") == 0);
  CHECK(std::strcmp(RegisterFile::name(SHADER_CONSTANT_LOOP_31), "SHADER_CONSTANT_LOOP_31") == 0);
  CHECK(RegisterFile::name(0x0000) == nullptr);
  CHECK(RegisterFile::name(0x4001) == nullptr);
  CHECK(RegisterFile::name(kRegisterCount) == nullptr);
  CHECK(RegisterFile::name(0xFFFFFFFFu) == nullptr);
}

TEST_CASE("float constants round-trip", "[unit][gpu]") {
  RegisterFile rf;
  const uint32_t reg = kShaderConstantFloatBase + 4 * 7 + 2;
  uint32_t bits;
  const float v = -1.5f;
  std::memcpy(&bits, &v, sizeof bits);
  rf.write(reg, bits);
  CHECK(rf.read(reg) == bits);
  CHECK(rf.read_float(reg) == v);
  CHECK(rf.read_float(reg + 1) == 0.0f);
}

TEST_CASE("out-of-range index is ignored on write and reads 0", "[unit][gpu]") {
  RegisterFile rf;
  rf.write(kRegisterCount - 1, 5);
  rf.write(kRegisterCount, 0xDEADBEEF);
  rf.write(0xFFFFFFFFu, 1);
  CHECK(rf.read(kRegisterCount - 1) == 5);
  CHECK(rf.read(kRegisterCount) == 0);
  CHECK(rf.read(0xFFFFFFFFu) == 0);
  CHECK(rf.read_float(kRegisterCount) == 0.0f);
}
