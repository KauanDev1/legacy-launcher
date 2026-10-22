// SPDX-License-Identifier: MIT
// The SPIR-V builder (runtime/gpu/shader/spirv_builder.h), checked with spirv-val (tests/support/spirv_tools.h), and
// the Vulkan shader harness (tests/support/vk_shader_harness.h) that runs builder-made modules on lavapipe.
#include <catch2/catch_test_macros.hpp>

#include <spirv/unified1/GLSL.std.450.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "runtime/gpu/shader/spirv_builder.h"
#include "support/spirv_tools.h"
#include "support/vk_shader_harness.h"

namespace sh = lr::gpu::shader;
using lr::testing::HarnessInputs;
using lr::testing::HarnessResult;
using lr::testing::ShaderHarness;
using sh::Id;
using sh::SpirvBuilder;

namespace {

using Pixel = std::array<float, 4>;

// SKIPs when SPIRV-Tools is absent; otherwise fails with spirv-val's diagnostics and the disassembly.
void require_valid(const std::vector<uint32_t>& module) {
  REQUIRE_FALSE(module.empty());
  if (!lr::testing::spirv_tools_available()) SKIP("spirv-val/spirv-dis not installed (Arch: spirv-tools)");
  if (auto err = lr::testing::spirv_val(module)) FAIL("spirv-val: " << *err << "\n" << lr::testing::spirv_dis(module));
}

// Validates when the tools are there, without skipping when they are not (for the [vulkan] tests).
void check_valid_if_possible(const std::vector<uint32_t>& module) {
  if (!lr::testing::spirv_tools_available()) return;
  if (auto err = lr::testing::spirv_val(module)) FAIL("spirv-val: " << *err << "\n" << lr::testing::spirv_dis(module));
}

ShaderHarness make_harness() {
  auto h = ShaderHarness::create();
  if (!h) {
    if (h.error().code == lr::ErrorCode::ToolMissing) SKIP("no Vulkan ICD: " << h.error().message);
    FAIL("harness: " << h.error().message);
  }
  return std::move(h.value());
}

HarnessResult draw_ok(ShaderHarness& h, const std::vector<uint32_t>& ps, const HarnessInputs& in) {
  const auto vs = lr::testing::fullscreen_triangle_vs();
  check_valid_if_possible(vs);
  check_valid_if_possible(ps);
  auto r = h.draw(vs, ps, in);
  if (!r) FAIL("draw: " << r.error().message);
  return std::move(r.value());
}

size_t count_of(const std::string& haystack, const std::string& needle) {
  size_t n = 0;
  for (size_t pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + 1)) ++n;
  return n;
}

// Common pieces of a fragment shader: float, vec4 and an output vec4 at `location`.
struct Frag {
  SpirvBuilder b{spv::ExecutionModelFragment};
  Id f32 = b.type_float(32);
  Id vec4 = b.type_vector(f32, 4);
  Id out_ptr = b.type_pointer(spv::StorageClassOutput, vec4);

  Id output(uint32_t location, std::string_view name) {
    const Id v = b.variable(spv::StorageClassOutput, out_ptr, name);
    b.decorate(v, spv::DecorationLocation, {location});
    return v;
  }
  Id vec4_const(float x, float y, float z, float w) {
    const Id c[] = {b.constant_f32(x), b.constant_f32(y), b.constant_f32(z), b.constant_f32(w)};
    return b.constant_composite(vec4, c);
  }
  std::vector<uint32_t> finish(std::initializer_list<Id> interface) {
    b.end_main();
    b.entry_point("main", std::span(interface.begin(), interface.size()));
    b.execution_mode(spv::ExecutionModeOriginUpperLeft);
    return b.finish();
  }
};

// Counts 0..3 in a loop; an if adds 10 at 2 and a switch adds 1 at 0 and 2 at 1, so the output is 13 everywhere.
std::vector<uint32_t> control_flow_ps() {
  Frag f;
  SpirvBuilder& b = f.b;
  const Id i32 = b.type_int(32, true);
  const Id bool_t = b.type_bool();
  const Id out = f.output(0, "color");
  const Id counter = b.variable(spv::StorageClassFunction, b.type_pointer(spv::StorageClassFunction, i32), "counter");
  const Id acc = b.variable(spv::StorageClassFunction, b.type_pointer(spv::StorageClassFunction, f.f32), "acc");

  b.begin_main();
  b.op_void(spv::OpStore, {counter, b.constant_i32(0)});
  b.op_void(spv::OpStore, {acc, b.constant_f32(0.0f)});
  const Id header = b.new_block(), check = b.new_block(), body = b.new_block(), then_block = b.new_block();
  const Id if_merge = b.new_block(), case0 = b.new_block(), case1 = b.new_block(), case_default = b.new_block();
  const Id switch_merge = b.new_block(), cont = b.new_block(), loop_merge = b.new_block();
  b.branch(header);

  b.set_block(header);
  b.loop_merge(loop_merge, cont);
  b.branch(check);

  b.set_block(check);
  const Id i = b.op(spv::OpLoad, i32, {counter});
  b.branch_conditional(b.op(spv::OpSLessThan, bool_t, {i, b.constant_i32(4)}), body, loop_merge);

  auto add = [&](float amount) {
    const Id v = b.op(spv::OpLoad, f.f32, {acc});
    b.op_void(spv::OpStore, {acc, b.op(spv::OpFAdd, f.f32, {v, b.constant_f32(amount)})});
  };

  b.set_block(body);
  const Id is_two = b.op(spv::OpIEqual, bool_t, {b.op(spv::OpLoad, i32, {counter}), b.constant_i32(2)});
  b.selection_merge(if_merge);
  b.branch_conditional(is_two, then_block, if_merge);

  b.set_block(then_block);
  add(10.0f);
  b.branch(if_merge);

  b.set_block(if_merge);
  const Id selector = b.op(spv::OpLoad, i32, {counter});
  b.selection_merge(switch_merge);
  const std::pair<uint32_t, Id> cases[] = {{0, case0}, {1, case1}};
  b.switch_(selector, case_default, cases);

  b.set_block(case0);
  add(1.0f);
  b.branch(switch_merge);
  b.set_block(case1);
  add(2.0f);
  b.branch(switch_merge);
  b.set_block(case_default);
  b.branch(switch_merge);

  b.set_block(switch_merge);
  b.branch(cont);

  b.set_block(cont);
  b.op_void(spv::OpStore, {counter, b.op(spv::OpIAdd, i32, {b.op(spv::OpLoad, i32, {counter}), b.constant_i32(1)})});
  b.branch(header);

  b.set_block(loop_merge);
  const Id total = b.op(spv::OpLoad, f.f32, {acc});
  b.op_void(spv::OpStore, {out, b.op(spv::OpCompositeConstruct, f.vec4, {total, total, total, total})});
  return f.finish({out});
}

// Reads a vec4 through a device address taken from the push constants as a uvec2 and bitcast to a pointer.
std::vector<uint32_t> buffer_address_ps() {
  Frag f;
  SpirvBuilder& b = f.b;
  b.capability(spv::CapabilityPhysicalStorageBufferAddresses);
  b.memory_model(spv::AddressingModelPhysicalStorageBuffer64, spv::MemoryModelGLSL450);
  const Id u32 = b.type_int(32, false);
  const Id uvec2 = b.type_vector(u32, 2);
  const Id pc_members[] = {uvec2};
  const Id pc_struct = b.type_struct(pc_members);
  b.decorate(pc_struct, spv::DecorationBlock);
  b.member_decorate(pc_struct, 0, spv::DecorationOffset, {0});
  const Id pc = b.variable(spv::StorageClassPushConstant, b.type_pointer(spv::StorageClassPushConstant, pc_struct),
                           "push");
  const Id data_members[] = {f.vec4};
  const Id data_struct = b.type_struct(data_members);
  b.decorate(data_struct, spv::DecorationBlock);
  b.member_decorate(data_struct, 0, spv::DecorationOffset, {0});
  const Id data_ptr = b.type_pointer(spv::StorageClassPhysicalStorageBuffer, data_struct);
  const Id out = f.output(0, "color");

  b.begin_main();
  const Id zero = b.constant_u32(0);
  const Id addr_ptr = b.op(spv::OpAccessChain, b.type_pointer(spv::StorageClassPushConstant, uvec2), {pc, zero});
  const Id addr = b.op(spv::OpLoad, uvec2, {addr_ptr});
  const Id data = b.op(spv::OpBitcast, data_ptr, {addr});
  const Id vec_ptr = b.op(spv::OpAccessChain, b.type_pointer(spv::StorageClassPhysicalStorageBuffer, f.vec4),
                          {data, zero});
  const Id value = b.op(spv::OpLoad, f.vec4, {vec_ptr, spv::MemoryAccessAlignedMask, 16});
  b.op_void(spv::OpStore, {out, value});
  return f.finish({pc, out});
}

// Samples the first 2D texture of the set 0 heap with the first sampler of the set 3 heap, at u = x / width.
std::vector<uint32_t> texture_ps(float width) {
  Frag f;
  SpirvBuilder& b = f.b;
  b.capability(spv::CapabilityRuntimeDescriptorArray);
  const Id vec2 = b.type_vector(f.f32, 2);
  const Id image = b.type_image(f.f32, spv::Dim2D, false, false, false, 1);
  const Id sampler = b.type_sampler();
  const Id textures = b.variable(spv::StorageClassUniformConstant,
                                 b.type_pointer(spv::StorageClassUniformConstant, b.type_runtime_array(image)),
                                 "textures_2d");
  b.decorate(textures, spv::DecorationDescriptorSet, {0});
  b.decorate(textures, spv::DecorationBinding, {0});
  const Id samplers = b.variable(spv::StorageClassUniformConstant,
                                 b.type_pointer(spv::StorageClassUniformConstant, b.type_runtime_array(sampler)),
                                 "samplers");
  b.decorate(samplers, spv::DecorationDescriptorSet, {3});
  b.decorate(samplers, spv::DecorationBinding, {0});
  const Id frag_coord = b.variable(spv::StorageClassInput, b.type_pointer(spv::StorageClassInput, f.vec4),
                                   "frag_coord");
  b.decorate(frag_coord, spv::DecorationBuiltIn, {spv::BuiltInFragCoord});
  const Id out = f.output(0, "color");

  b.begin_main();
  const Id zero = b.constant_u32(0);
  const Id img = b.op(spv::OpLoad, image,
                      {b.op(spv::OpAccessChain, b.type_pointer(spv::StorageClassUniformConstant, image), {textures, zero})});
  const Id smp = b.op(spv::OpLoad, sampler,
                      {b.op(spv::OpAccessChain, b.type_pointer(spv::StorageClassUniformConstant, sampler),
                            {samplers, zero})});
  const Id combined = b.op(spv::OpSampledImage, b.type_sampled_image(image), {img, smp});
  const Id x = b.op(spv::OpCompositeExtract, f.f32, {b.op(spv::OpLoad, f.vec4, {frag_coord}), 0});
  const Id u = b.op(spv::OpFDiv, f.f32, {x, b.constant_f32(width)});
  const Id coord = b.op(spv::OpCompositeConstruct, vec2, {u, b.constant_f32(0.5f)});
  b.op_void(spv::OpStore, {out, b.op(spv::OpImageSampleImplicitLod, f.vec4, {combined, coord})});
  return f.finish({textures, samplers, frag_coord, out});
}

}  // namespace

TEST_CASE("empty fragment shader validates", "[unit]") {
  SpirvBuilder b(spv::ExecutionModelFragment);
  b.begin_main();
  b.end_main();
  b.entry_point("main", {});
  b.execution_mode(spv::ExecutionModeOriginUpperLeft);
  const auto m = b.finish();
  REQUIRE(m.size() > 5);
  CHECK(m[0] == 0x07230203u);
  CHECK(m[1] == 0x00010500u);  // SPIR-V 1.5
  CHECK(m[4] == 0u);
  require_valid(m);
}

TEST_CASE("types and constants are deduplicated", "[unit]") {
  SpirvBuilder b(spv::ExecutionModelFragment);
  const Id f32 = b.type_float(32);
  CHECK(b.type_float(32) == f32);
  CHECK(b.type_void() == b.type_void());
  CHECK(b.type_bool() == b.type_bool());
  CHECK(b.type_int(32, true) == b.type_int(32, true));
  CHECK(b.type_int(32, true) != b.type_int(32, false));
  const Id vec4 = b.type_vector(f32, 4);
  CHECK(b.type_vector(f32, 4) == vec4);
  CHECK(b.type_vector(f32, 3) != vec4);
  CHECK(b.type_pointer(spv::StorageClassOutput, vec4) == b.type_pointer(spv::StorageClassOutput, vec4));
  CHECK(b.type_pointer(spv::StorageClassOutput, vec4) != b.type_pointer(spv::StorageClassInput, vec4));
  CHECK(b.type_function(b.type_void()) == b.type_function(b.type_void()));
  const Id image = b.type_image(f32, spv::Dim2D, false, false, false, 1);
  CHECK(b.type_image(f32, spv::Dim2D, false, false, false, 1) == image);
  CHECK(b.type_image(f32, spv::Dim3D, false, false, false, 1) != image);
  CHECK(b.type_sampler() == b.type_sampler());
  CHECK(b.type_sampled_image(image) == b.type_sampled_image(image));

  CHECK(b.constant_f32(1.0f) == b.constant_f32(1.0f));
  CHECK(b.constant_f32(0.0f) != b.constant_f32(-0.0f));  // keyed by bit pattern
  CHECK(b.constant_u32(1) == b.constant_u32(1));
  CHECK(b.constant_u32(1) != b.constant_i32(1));
  CHECK(b.constant_i32(-1) == b.constant_i32(-1));
  CHECK(b.constant_bool(true) == b.constant_bool(true));
  CHECK(b.constant_bool(true) != b.constant_bool(false));
  const Id parts[] = {b.constant_f32(1.0f), b.constant_f32(2.0f), b.constant_f32(3.0f), b.constant_f32(4.0f)};
  CHECK(b.constant_composite(vec4, parts) == b.constant_composite(vec4, parts));

  // Not merged: structs and arrays carry per-type layout decorations, spec constants their SpecId.
  const Id members[] = {f32};
  CHECK(b.type_struct(members) != b.type_struct(members));
  CHECK(b.type_runtime_array(f32) != b.type_runtime_array(f32));
  CHECK(b.type_array(f32, b.constant_u32(4)) != b.type_array(f32, b.constant_u32(4)));
  CHECK(b.spec_constant_u32(0, 7) != b.spec_constant_u32(1, 7));

  b.begin_main();
  b.end_main();
  b.entry_point("main", {});
  b.execution_mode(spv::ExecutionModeOriginUpperLeft);
  const auto m = b.finish();
  require_valid(m);
  const std::string dis = lr::testing::spirv_dis(m);
  INFO(dis);
  CHECK(count_of(dis, "OpTypeFloat 32") == 1);
  CHECK(count_of(dis, "OpTypeVector") == 2);
  CHECK(count_of(dis, "OpTypeSampler") == 1);
  CHECK(count_of(dis, "OpConstantComposite") == 1);
  CHECK(count_of(dis, "OpSpecConstant ") == 2);
  CHECK(count_of(dis, "SpecId") == 2);  // one decoration each
}

TEST_CASE("structured if, loop and switch validate", "[unit]") {
  require_valid(control_flow_ps());
}

TEST_CASE("physical storage buffer load through a bitcast uvec2 validates", "[unit]") {
  const auto m = buffer_address_ps();
  require_valid(m);
  const std::string dis = lr::testing::spirv_dis(m);
  INFO(dis);
  CHECK(dis.find("OpMemoryModel PhysicalStorageBuffer64 GLSL450") != std::string::npos);
  CHECK(dis.find("OpBitcast") != std::string::npos);
  CHECK(dis.find("Aligned 16") != std::string::npos);  // loads through a physical pointer carry their alignment
  CHECK(dis.find("OpCapability Int64") == std::string::npos);  // no shaderInt64
}

TEST_CASE("entry point without a used global is rejected", "[unit]") {
  Frag f;
  const Id out = f.output(0, "color");
  f.b.begin_main();
  f.b.op_void(spv::OpStore, {out, f.vec4_const(1, 0, 0, 1)});
  f.b.end_main();
  f.b.execution_mode(spv::ExecutionModeOriginUpperLeft);

  f.b.entry_point("main", {});
  const auto missing = f.b.check_interface();
  REQUIRE_FALSE(missing.ok());
  CHECK(missing.error().code == lr::ErrorCode::InvalidFormat);
  CHECK(missing.error().message.find("'color'") != std::string::npos);

  const Id iface[] = {out};
  f.b.entry_point("main", iface);
  CHECK(f.b.check_interface().ok());
  require_valid(f.b.finish());
}

TEST_CASE("sampled image types and a sample instruction validate", "[unit]") {
  Frag f;
  SpirvBuilder& b = f.b;
  b.capability(spv::CapabilityRuntimeDescriptorArray);
  b.capability(spv::CapabilityImageQuery);
  const Id i32 = b.type_int(32, true);
  const Id vec2 = b.type_vector(f.f32, 2);
  const Id ivec2 = b.type_vector(i32, 2);
  const Id image = b.type_image(f.f32, spv::Dim2D, false, false, false, 1);
  const Id cube = b.type_image(f.f32, spv::DimCube, false, false, false, 1);
  const Id sampler = b.type_sampler();
  auto heap = [&](Id element, uint32_t set, std::string_view name) {
    const Id v = b.variable(spv::StorageClassUniformConstant,
                            b.type_pointer(spv::StorageClassUniformConstant, b.type_runtime_array(element)), name);
    b.decorate(v, spv::DecorationDescriptorSet, {set});
    b.decorate(v, spv::DecorationBinding, {0});
    return v;
  };
  const Id tex2d = heap(image, 0, "textures_2d");
  const Id texcube = heap(cube, 2, "textures_cube");
  const Id samplers = heap(sampler, 3, "samplers");
  const Id out = f.output(0, "color");

  b.begin_main();
  const Id idx = b.constant_u32(0);
  auto load = [&](Id type, Id heap_var) {
    return b.op(spv::OpLoad, type,
                {b.op(spv::OpAccessChain, b.type_pointer(spv::StorageClassUniformConstant, type), {heap_var, idx})});
  };
  const Id img = load(image, tex2d);
  const Id cube_img = load(cube, texcube);
  const Id smp = load(sampler, samplers);
  const Id combined = b.op(spv::OpSampledImage, b.type_sampled_image(image), {img, smp});
  const Id cube_combined = b.op(spv::OpSampledImage, b.type_sampled_image(cube), {cube_img, smp});
  const Id uv_parts[] = {b.constant_f32(0.5f), b.constant_f32(0.5f)};
  const Id uv = b.constant_composite(vec2, uv_parts);
  const Id dir_parts[] = {b.constant_f32(1.0f), b.constant_f32(0.0f), b.constant_f32(0.0f)};
  const Id dir = b.constant_composite(b.type_vector(f.f32, 3), dir_parts);
  const Id implicit = b.op(spv::OpImageSampleImplicitLod, f.vec4, {combined, uv});
  const Id explicit_lod =
      b.op(spv::OpImageSampleExplicitLod, f.vec4, {combined, uv, spv::ImageOperandsLodMask, b.constant_f32(1.0f)});
  const Id cube_sample = b.op(spv::OpImageSampleExplicitLod, f.vec4,
                              {cube_combined, dir, spv::ImageOperandsLodMask, b.constant_f32(0.0f)});
  const Id size = b.op(spv::OpImageQuerySizeLod, ivec2, {img, b.constant_i32(0)});
  const Id fsize = b.op(spv::OpConvertSToF, vec2, {size});
  const Id size4 = b.op(spv::OpCompositeConstruct, f.vec4, {fsize, b.constant_f32(0.0f), b.constant_f32(0.0f)});
  Id sum = b.op(spv::OpFAdd, f.vec4, {implicit, explicit_lod});
  sum = b.op(spv::OpFAdd, f.vec4, {sum, cube_sample});
  sum = b.op(spv::OpFAdd, f.vec4, {sum, size4});
  b.op_void(spv::OpStore, {out, b.ext_inst(f.vec4, GLSLstd450FClamp, {sum, f.vec4_const(0, 0, 0, 0),
                                                                       f.vec4_const(1, 1, 1, 1)})});
  const auto m = f.finish({tex2d, texcube, samplers, out});
  require_valid(m);
  const std::string dis = lr::testing::spirv_dis(m);
  INFO(dis);
  CHECK(dis.find("OpTypeSampledImage") != std::string::npos);
  CHECK(dis.find("OpImageSampleImplicitLod") != std::string::npos);
  CHECK(dis.find("OpExtInstImport \"GLSL.std.450\"") != std::string::npos);
}

TEST_CASE("harness renders a builder-made constant colour", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  Frag f;
  const Id out = f.output(0, "color");
  f.b.begin_main();
  f.b.op_void(spv::OpStore, {out, f.vec4_const(0.25f, 0.5f, 0.75f, 1.0f)});
  const auto ps = f.finish({out});

  HarnessInputs in;
  in.width = 4;
  const HarnessResult r = draw_ok(h, ps, in);
  REQUIRE(r.color[0].size() == 4);
  for (const Pixel& p : r.color[0]) CHECK(p == Pixel{0.25f, 0.5f, 0.75f, 1.0f});
  CHECK(r.color[1].empty());
  CHECK(r.depth.empty());
}

TEST_CASE("harness samples a builder-made texture", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  HarnessInputs in;
  in.width = 2;
  lr::testing::HarnessTexture t;
  t.w = 2;
  t.h = 1;
  t.texels = {Pixel{1.0f, 0.0f, 0.0f, 1.0f}, Pixel{0.0f, 1.0f, 0.5f, 0.25f}};
  in.textures.push_back(t);
  in.samplers.push_back({});  // nearest, clamp to edge
  const HarnessResult r = draw_ok(h, texture_ps(2.0f), in);
  REQUIRE(r.color[0].size() == 2);
  CHECK(r.color[0][0] == Pixel{1.0f, 0.0f, 0.0f, 1.0f});
  CHECK(r.color[0][1] == Pixel{0.0f, 1.0f, 0.5f, 0.25f});
}

TEST_CASE("harness reads a buffer through its device address", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  HarnessInputs in;
  in.width = 3;
  const float data[] = {1.5f, -2.0f, 3.25f, 4.0f};
  in.buffers.push_back({lr::testing::harness_bytes(std::span<const float>(data)), 0});
  const HarnessResult r = draw_ok(h, buffer_address_ps(), in);
  REQUIRE(r.color[0].size() == 3);
  for (const Pixel& p : r.color[0]) CHECK(p == Pixel{1.5f, -2.0f, 3.25f, 4.0f});
}

TEST_CASE("harness runs builder-made structured control flow", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  const HarnessResult r = draw_ok(h, control_flow_ps(), HarnessInputs{});
  REQUIRE(r.color[0].size() == 4);
  for (const Pixel& p : r.color[0]) CHECK(p == Pixel{13.0f, 13.0f, 13.0f, 13.0f});
}

TEST_CASE("harness reads back every colour target and depth", "[unit][vulkan]") {
  ShaderHarness h = make_harness();
  Frag f;
  std::vector<Id> outs;
  for (uint32_t t = 0; t < 4; ++t) outs.push_back(f.output(t, "color" + std::to_string(t)));
  const Id depth = f.b.variable(spv::StorageClassOutput, f.b.type_pointer(spv::StorageClassOutput, f.f32), "depth");
  f.b.decorate(depth, spv::DecorationBuiltIn, {spv::BuiltInFragDepth});
  f.b.execution_mode(spv::ExecutionModeDepthReplacing);
  f.b.begin_main();
  for (uint32_t t = 0; t < 4; ++t) {
    const float v = static_cast<float>(t + 1);
    f.b.op_void(spv::OpStore, {outs[t], f.vec4_const(v, v * 10, v * 100, -v)});
  }
  f.b.op_void(spv::OpStore, {depth, f.b.constant_f32(0.375f)});
  const auto ps = f.finish({outs[0], outs[1], outs[2], outs[3], depth});

  HarnessInputs in;
  in.width = 2;
  in.color_targets = 4;
  in.depth_target = true;
  const HarnessResult r = draw_ok(h, ps, in);
  for (uint32_t t = 0; t < 4; ++t) {
    const float v = static_cast<float>(t + 1);
    REQUIRE(r.color[t].size() == 2);
    for (const Pixel& p : r.color[t]) CHECK(p == Pixel{v, v * 10, v * 100, -v});
  }
  REQUIRE(r.depth.size() == 2);
  for (float d : r.depth) CHECK(d == 0.375f);
}
