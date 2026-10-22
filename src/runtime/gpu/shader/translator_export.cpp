// SPDX-License-Identifier: MIT
// The stage interface of a translated shader and its exports (contract.h): VS r0.x and the 16 interpolator outputs,
// the position transform (VTE flags, then ndc scale/offset) and the point size; PS registers from the
// interpolators, param_gen, colors, depth and the alpha test. Xenos semantics per Xenia's SPIR-V translator
// (reference only, nothing copied).
#include <spirv/unified1/GLSL.std.450.h>

#include <string>

#include "runtime/gpu/shader/translator_internal.h"

namespace lr::gpu::shader::detail {

namespace {
constexpr uint32_t kSignBit = 0x80000000u;
}

void Translator::declare_stage_interface() {
  const Id func_vec4 = b_.type_pointer(spv::StorageClassFunction, t_.vec4);
  const auto export_var = [&](uint32_t reg) {
    export_vars_[reg] = b_.variable(spv::StorageClassFunction, func_vec4, "export" + std::to_string(reg));
  };

  if (is_vs()) {
    vertex_index_in_ = global_var(spv::StorageClassInput, t_.i32, "vertex_index");
    b_.decorate(vertex_index_in_, spv::DecorationBuiltIn, {spv::BuiltInVertexIndex});
    position_out_ = global_var(spv::StorageClassOutput, t_.vec4, "position");
    b_.decorate(position_out_, spv::DecorationBuiltIn, {spv::BuiltInPosition});
    point_size_out_ = global_var(spv::StorageClassOutput, t_.f32, "point_size");
    b_.decorate(point_size_out_, spv::DecorationBuiltIn, {spv::BuiltInPointSize});
    // Always all 16 (64 components, the Vulkan minimum), so any PS links by register index; no interpolation
    // decorations on the VS side (the PS decides).
    for (uint32_t i = 0; i < kInterpolatorLocations; ++i) {
      interpolators_[i] = global_var(spv::StorageClassOutput, t_.vec4, "out_interp" + std::to_string(i));
      b_.decorate(interpolators_[i], spv::DecorationLocation, {i});
      if ((program_.interpolators_written >> i) & 1) export_var(i);
    }
    if (program_.writes_position) export_var(kExportVsPosition);
    return;
  }

  // Pixel shader inputs: every register r < 16 the program references, except the param_gen register.
  for (uint32_t r = 0; r < kInterpolatorLocations && r < reg_count_; ++r) {
    if (!temps_.relative && !temps_.absolute.test(r)) continue;
    if (r == options_.ps_param_gen) continue;
    interpolators_[r] = global_var(spv::StorageClassInput, t_.vec4, "in_r" + std::to_string(r));
    b_.decorate(interpolators_[r], spv::DecorationLocation, {r});
    if ((options_.flat_mask >> r) & 1) b_.decorate(interpolators_[r], spv::DecorationFlat);
    if ((options_.centroid_mask >> r) & 1) b_.decorate(interpolators_[r], spv::DecorationCentroid);
    info_.interpolators_read |= 1u << r;
  }
  if (options_.ps_param_gen < reg_count_) {
    frag_coord_in_ = global_var(spv::StorageClassInput, t_.vec4, "frag_coord");
    b_.decorate(frag_coord_in_, spv::DecorationBuiltIn, {spv::BuiltInFragCoord});
    front_facing_in_ = global_var(spv::StorageClassInput, t_.bool_, "front_facing");
    b_.decorate(front_facing_in_, spv::DecorationBuiltIn, {spv::BuiltInFrontFacing});
    if (options_.ps_point) {
      point_coord_in_ = global_var(spv::StorageClassInput, t_.vec2, "point_coord");
      b_.decorate(point_coord_in_, spv::DecorationBuiltIn, {spv::BuiltInPointCoord});
    }
  }
  for (uint32_t i = 0; i < kExportColorCount; ++i) {
    if (!((program_.colors_written >> i) & 1)) continue;
    color_outputs_[i] = global_var(spv::StorageClassOutput, t_.vec4, "oC" + std::to_string(i));
    b_.decorate(color_outputs_[i], spv::DecorationLocation, {i});
    export_var(i);
  }
  if (program_.writes_depth) {
    depth_out_ = global_var(spv::StorageClassOutput, t_.f32, "depth");
    b_.decorate(depth_out_, spv::DecorationBuiltIn, {spv::BuiltInFragDepth});
    export_var(kExportPsDepth);
  }
  if (program_.colors_written & 1) spec_alpha_ = b_.spec_constant_u32(kSpecAlphaTest, 0);
}

void Translator::emit_stage_prologue() {
  const Id zero4 = const_vec4(0, 0, 0, 0);
  for (Id v : export_vars_)
    if (v) b_.op_void(spv::OpStore, {v, zero4});

  if (is_vs()) {
    // r0.x = float(clamp((VertexIndex + index_offset) & 0xFFFFFF, min_index, max_index)): the hardware index is 24
    // bits, so the sum is masked before the unsigned clamp.
    const Id vertex = b_.op(spv::OpLoad, t_.i32, {vertex_index_in_});
    const Id sum = b_.op(spv::OpIAdd, t_.i32, {vertex, load_shared(kSharedIndexOffset)});
    Id index = b_.op(spv::OpBitwiseAnd, t_.u32, {b_.op(spv::OpBitcast, t_.u32, {sum}), const_u(kVertexIndexMask)});
    index = glsl(t_.u32, GLSLstd450UMax, {index, load_shared(kSharedMinIndex)});
    index = glsl(t_.u32, GLSLstd450UMin, {index, load_shared(kSharedMaxIndex)});
    const Id x = b_.op(spv::OpConvertUToF, t_.f32, {index});
    const Id zero = const_f(0.0f);
    store_reg_masked(0, b_.op(spv::OpCompositeConstruct, t_.vec4, {x, zero, zero, zero}), 0xF);
    return;
  }

  if ((program_.colors_written & 1) || program_.uses_kill) {
    killed_ = b_.variable(spv::StorageClassFunction, b_.type_pointer(spv::StorageClassFunction, t_.bool_), "killed");
    b_.op_void(spv::OpStore, {killed_, b_.constant_bool(false)});
  }
  for (uint32_t r = 0; r < kInterpolatorLocations; ++r)
    if (interpolators_[r]) store_reg_masked(r, b_.op(spv::OpLoad, t_.vec4, {interpolators_[r]}), 0xF);

  if (frag_coord_in_) {
    // param_gen: x = |floor(FragCoord.x)|, sign bit = back-facing; y = |floor(FragCoord.y)|, sign bit = point;
    // z, w = the point coordinate (clamped to 0..1) for points, else 0.
    const Id coord = b_.op(spv::OpLoad, t_.vec4, {frag_coord_in_});
    const auto magnitude = [&](uint32_t c) {
      const Id v = glsl(t_.f32, GLSLstd450FAbs, {glsl(t_.f32, GLSLstd450Floor, {extract(coord, c)})});
      return b_.op(spv::OpBitcast, t_.u32, {v});
    };
    Id x = magnitude(0);
    const Id back = b_.op(spv::OpLogicalNot, t_.bool_, {b_.op(spv::OpLoad, t_.bool_, {front_facing_in_})});
    x = b_.op(spv::OpSelect, t_.u32, {back, b_.op(spv::OpBitwiseOr, t_.u32, {x, const_u(kSignBit)}), x});
    Id y = magnitude(1);
    if (options_.ps_point) y = b_.op(spv::OpBitwiseOr, t_.u32, {y, const_u(kSignBit)});
    Id z = const_f(0.0f), w = const_f(0.0f);
    if (point_coord_in_) {
      const Id pc = b_.op(spv::OpLoad, t_.vec2, {point_coord_in_});
      const Id zeros[] = {const_f(0.0f), const_f(0.0f)};
      const Id ones[] = {const_f(1.0f), const_f(1.0f)};
      const Id clamped = glsl(t_.vec2, GLSLstd450NClamp,
                              {pc, b_.constant_composite(t_.vec2, zeros), b_.constant_composite(t_.vec2, ones)});
      z = extract(clamped, 0);
      w = extract(clamped, 1);
    }
    const Id value = b_.op(spv::OpCompositeConstruct, t_.vec4,
                           {b_.op(spv::OpBitcast, t_.f32, {x}), b_.op(spv::OpBitcast, t_.f32, {y}), z, w});
    store_reg_masked(options_.ps_param_gen, value, 0xF);
  }
}

Result<void> Translator::emit_export(uint32_t slot, const AluInstr& a, Id vector_result, Id scalar_result) {
  const uint32_t reg = a.vector_dest;
  // An export with no component to write does nothing, whatever its register (the decoder does not count it).
  if ((a.export_vector_mask | a.export_scalar_mask | a.export_const0_mask | a.export_const1_mask) == 0) return {};
  if (reg >= kExportAddress && reg < kExportData0 + kExportDataCount)
    return unsupported(slot, "alu", "memexport (eA/eM)");
  if (is_vs() && reg == kExportVsPointSize) return {};  // dropped in M3 (ShaderInfo::drops_point_size)
  const Id var = reg < export_vars_.size() ? export_vars_[reg] : 0;
  if (!var) return internal("export register " + std::to_string(reg) + " has no variable");

  const Id old_value = b_.op(spv::OpLoad, t_.vec4, {var});
  Id c[4];
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t bit = 1u << i;
    if (a.export_const1_mask & bit)
      c[i] = const_f(1.0f);
    else if (a.export_const0_mask & bit)
      c[i] = const_f(0.0f);
    else if ((a.export_vector_mask & bit) && vector_result)
      c[i] = extract(vector_result, i);
    else if ((a.export_scalar_mask & bit) && scalar_result)
      c[i] = scalar_result;
    else if ((a.export_vector_mask | a.export_scalar_mask) & bit)
      return internal("export component without a result");
    else
      c[i] = extract(old_value, i);
  }
  b_.op_void(spv::OpStore, {var, b_.op(spv::OpCompositeConstruct, t_.vec4, {c[0], c[1], c[2], c[3]})});
  return {};
}

void Translator::emit_stage_epilogue() {
  const Id zero4 = const_vec4(0, 0, 0, 0);
  if (is_vs()) {
    for (uint32_t i = 0; i < kInterpolatorLocations; ++i) {
      const Id v = export_vars_[i] ? b_.op(spv::OpLoad, t_.vec4, {export_vars_[i]}) : zero4;
      b_.op_void(spv::OpStore, {interpolators_[i], v});
    }
    b_.op_void(spv::OpStore, {point_size_out_, const_f(1.0f)});

    Id position = zero4;
    if (const Id var = export_vars_[kExportVsPosition]) {
      // VTE flags first (undo the divide by W of xy and z; W given as 1/W unless vtx_w0_fmt), then
      // xyz = xyz * ndc_scale + ndc_offset * w.
      const Id p = b_.op(spv::OpLoad, t_.vec4, {var});
      const Id vte = load_shared(kSharedVteFlags);
      const auto flag = [&](uint32_t bit) {
        return b_.op(spv::OpINotEqual, t_.bool_, {b_.op(spv::OpBitwiseAnd, t_.u32, {vte, const_u(bit)}), const_u(0)});
      };
      const Id one = const_f(1.0f);
      Id w = extract(p, 3);
      w = b_.op(spv::OpSelect, t_.f32,
                {flag(kVteW0NotReciprocal), w, b_.op(spv::OpFDiv, t_.f32, {one, w})});
      const Id xy_scale = b_.op(spv::OpSelect, t_.f32, {flag(kVteXyDividedByW), w, one});
      const Id z_scale = b_.op(spv::OpSelect, t_.f32, {flag(kVteZDividedByW), w, one});
      const Id x = b_.op(spv::OpFMul, t_.f32, {extract(p, 0), xy_scale});
      const Id y = b_.op(spv::OpFMul, t_.f32, {extract(p, 1), xy_scale});
      const Id z = b_.op(spv::OpFMul, t_.f32, {extract(p, 2), z_scale});
      const Id unscaled = b_.op(spv::OpCompositeConstruct, t_.vec4, {x, y, z, w});
      const Id scaled = b_.op(spv::OpFMul, t_.vec4, {unscaled, load_shared(kSharedNdcScale)});
      const Id offset = b_.op(spv::OpVectorTimesScalar, t_.vec4, {load_shared(kSharedNdcOffset), w});
      position = b_.op(spv::OpCompositeInsert, t_.vec4, {w, b_.op(spv::OpFAdd, t_.vec4, {scaled, offset}), 3});
    }
    b_.op_void(spv::OpStore, {position_out_, position});
    return;
  }

  for (uint32_t i = 0; i < kExportColorCount; ++i)
    if (color_outputs_[i]) b_.op_void(spv::OpStore, {color_outputs_[i], b_.op(spv::OpLoad, t_.vec4, {export_vars_[i]})});
  if (depth_out_)
    b_.op_void(spv::OpStore, {depth_out_, extract(b_.op(spv::OpLoad, t_.vec4, {export_vars_[kExportPsDepth]}), 0)});

  if (spec_alpha_) {
    // Alpha test on oC0.w: spec constant 0 bit 1 enables it, bits 2..4 are the Xenos compare function, a bit mask
    // of less (1), equal (2) and greater (4). NaN compares unordered: only "not equal" and "always" pass.
    const Id enabled = b_.op(spv::OpINotEqual, t_.bool_,
                             {b_.op(spv::OpBitwiseAnd, t_.u32, {spec_alpha_, const_u(kAlphaTestEnable)}), const_u(0)});
    const Id func = b_.op(spv::OpBitwiseAnd, t_.u32,
                          {b_.op(spv::OpShiftRightLogical, t_.u32, {spec_alpha_, const_u(kAlphaTestFuncShift)}),
                           const_u(kAlphaTestFuncMask)});
    const auto func_has = [&](uint32_t bit) {
      return b_.op(spv::OpINotEqual, t_.bool_, {b_.op(spv::OpBitwiseAnd, t_.u32, {func, const_u(bit)}), const_u(0)});
    };
    const Id alpha = extract(b_.op(spv::OpLoad, t_.vec4, {export_vars_[0]}), 3);
    const Id ref = load_shared(kSharedAlphaRef);
    const auto both = [&](Id p, Id q) { return b_.op(spv::OpLogicalAnd, t_.bool_, {p, q}); };
    const auto either = [&](Id p, Id q) { return b_.op(spv::OpLogicalOr, t_.bool_, {p, q}); };
    Id pass = both(func_has(1), b_.op(spv::OpFOrdLessThan, t_.bool_, {alpha, ref}));
    pass = either(pass, both(func_has(2), b_.op(spv::OpFOrdEqual, t_.bool_, {alpha, ref})));
    pass = either(pass, both(func_has(4), b_.op(spv::OpFOrdGreaterThan, t_.bool_, {alpha, ref})));
    const Id unordered = either(b_.op(spv::OpIsNan, t_.bool_, {alpha}), b_.op(spv::OpIsNan, t_.bool_, {ref}));
    pass = either(pass, both(unordered, both(func_has(1), func_has(4))));
    kill_if(both(enabled, b_.op(spv::OpLogicalNot, t_.bool_, {pass})));
  }

  if (killed_) {
    const Id kill_block = b_.new_block();
    const Id merge = b_.new_block();
    const Id killed = b_.op(spv::OpLoad, t_.bool_, {killed_});
    b_.selection_merge(merge);
    b_.branch_conditional(killed, kill_block, merge);
    b_.set_block(kill_block);
    b_.op_void(spv::OpKill, {});
    b_.set_block(merge);
  }
}

}  // namespace lr::gpu::shader::detail
