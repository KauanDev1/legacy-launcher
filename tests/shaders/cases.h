// SPDX-License-Identifier: MIT
// Synthetic shader containers for the XenosRecomp harness (tests/shaders). Shared by the build-time generator,
// which writes them to disk for the tool, and by the tests, which recompute their microcode hashes.
#pragma once

#include <string>
#include <vector>

#include "support/xenos_shader_builder.h"

namespace lr::testing::shaders {

namespace x = lr::testing::xenos;

struct Case {
  std::string file;  // file name under in/ (or broken-*/)
  x::ShaderDesc desc;
};

// VS: r1 = vfetch(slot 0, offset 0, stride 4, float4); oPos = r1; o[TEXCOORD0] = c0.
inline x::ShaderDesc vertex_basic() {
  x::ShaderDesc d;
  d.vertex = true;
  d.constants = {{"g_Color", x::kFloat4, 0, 1}};
  d.vertex_elements = {{1, x::kPosition, 0}};
  d.interpolators = {{x::kTexCoord, 0, 0}};
  x::emit(d.microcode, x::cf_pair(x::cf_exec(true, 1, 3, 0b000001), x::cf_nop()));
  x::emit(d.microcode, x::vfetch(1, 0, 0, 0, 4, x::kFormat32_32_32_32Float));
  x::Alu pos;
  pos.export_data = true;
  pos.vector_dest = x::kExportVsPosition;
  pos.src1 = {1};
  pos.src2 = {1};
  x::emit(d.microcode, x::alu(pos));
  x::Alu color;
  color.export_data = true;
  color.vector_dest = x::kExportVsInterpolator0;
  color.src1 = {0, false};
  color.src2 = {0, false};
  x::emit(d.microcode, x::alu(color));
  return d;
}

// PS: r1 = tfetch2D(tf0, r0.xy); oC0 = r1 * c0. Gets the alpha test (all 8 compare functions) at its end.
inline x::ShaderDesc pixel_alpha_test() {
  x::ShaderDesc d;
  d.vertex = false;
  d.constants = {{"g_Tint", x::kFloat4, 0, 1}, {"g_Texture", x::kSampler, 0, 1}};
  d.interpolators = {{x::kTexCoord, 0, 0}};
  d.ps_outputs = 1;
  x::emit(d.microcode, x::cf_pair(x::cf_exec(true, 1, 2, 0b0001), x::cf_nop()));
  x::emit(d.microcode, x::tfetch(1, 0, 0, x::kTextureDim2D));
  x::Alu out;
  out.vector_op = x::kVectorMul;
  out.export_data = true;
  out.vector_dest = x::kExportPsColor0;
  out.src1 = {1};
  out.src2 = {0, false};
  x::emit(d.microcode, x::alu(out));
  return d;
}

// VS: o[TEXCOORD0] = c1 only when bool constant 100 is true (cond_exec); oPos = c0.
inline x::ShaderDesc vertex_bool_100() {
  x::ShaderDesc d;
  d.vertex = true;
  d.constants = {{"g_Position", x::kFloat4, 0, 1}, {"g_Color", x::kFloat4, 1, 1}, {"g_UseColor", x::kBool, 100, 1}};
  d.interpolators = {{x::kTexCoord, 0, 0}};
  x::emit(d.microcode, x::cf_pair(x::cf_cond_exec(false, 1, 1, 0, 100, true), x::cf_exec(true, 2, 1, 0)));
  x::Alu color;
  color.export_data = true;
  color.vector_dest = x::kExportVsInterpolator0;
  color.src1 = {1, false};
  color.src2 = {1, false};
  x::emit(d.microcode, x::alu(color));
  x::Alu pos;
  pos.export_data = true;
  pos.vector_dest = x::kExportVsPosition;
  pos.src1 = {0, false};
  pos.src2 = {0, false};
  x::emit(d.microcode, x::alu(pos));
  return d;
}

// VS: loop over loop constant 5 accumulating r2 += c[0 + aL]; oPos = r2.
inline x::ShaderDesc vertex_loop() {
  x::ShaderDesc d;
  d.vertex = true;
  d.constants = {{"g_Weights", x::kFloat4, 0, 8}};
  // CF: 0 loop_start(i5, skip to 3) | 1 exec body | 2 loop_end(i5, back to 1) | 3 exec_end
  x::emit(d.microcode, x::cf_pair(x::cf_loop_start(5, 3), x::cf_exec(false, 2, 1, 0)));
  x::emit(d.microcode, x::cf_pair(x::cf_loop_end(5, 1), x::cf_exec(true, 3, 1, 0)));
  x::Alu acc;
  acc.vector_op = x::kVectorAdd;
  acc.vector_dest = 2;
  acc.src1 = {2};
  acc.src2 = {0, false};
  acc.const0_relative = true;
  x::emit(d.microcode, x::alu(acc));
  x::Alu pos;
  pos.export_data = true;
  pos.vector_dest = x::kExportVsPosition;
  pos.src1 = {2};
  pos.src2 = {2};
  x::emit(d.microcode, x::alu(pos));
  return d;
}

// Broken: the shader header offset points past the container.
inline x::ShaderDesc broken_bounds() {
  x::ShaderDesc d = vertex_basic();
  d.shader_offset_override = 0x00100000;
  return d;
}

// Broken: exports COLOR1 although the output mask only declares COLOR0, so DXC rejects the generated HLSL.
inline x::ShaderDesc broken_dxc() {
  x::ShaderDesc d;
  d.vertex = false;
  d.constants = {{"g_Tint", x::kFloat4, 0, 1}};
  d.ps_outputs = 1;
  x::emit(d.microcode, x::cf_pair(x::cf_exec(true, 1, 1, 0), x::cf_nop()));
  x::Alu out;
  out.export_data = true;
  out.vector_dest = x::kExportPsColor1;
  out.src1 = {0, false};
  out.src2 = {0, false};
  x::emit(d.microcode, x::alu(out));
  return d;
}

// Translated together into one cache at build time.
inline std::vector<Case> good_cases() {
  return {{"vertex_basic.bin", vertex_basic()},
          {"pixel_alpha_test.bin", pixel_alpha_test()},
          {"vertex_bool_100.bin", vertex_bool_100()},
          {"vertex_loop.bin", vertex_loop()}};
}

// Each in a directory of its own; the tool must fail on each.
inline std::vector<Case> broken_cases() {
  return {{"broken-bounds/broken.bin", broken_bounds()}, {"broken-dxc/broken.bin", broken_dxc()}};
}

}  // namespace lr::testing::shaders
