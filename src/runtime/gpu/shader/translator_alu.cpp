// SPDX-License-Identifier: MIT
// The ALU (Task 15.6: the vector unit; the scalar unit is Task 15.7, and until then only the scalar PRED_SET*s of
// translator_cf.cpp are accepted). Semantics per Xenia's ucode.h and SPIR-V translator (reference only, nothing
// copied):
//   - sources: temporaries (absolute or r[aL + n], clamped to 0..63) or float constants (absolute, or c[n + aL] /
//     c[n + a0] per the const_0/const_1 rule the decoder resolves; a sum outside 0..255 reads 0), then the swizzle,
//     abs, negate;
//   - the multiply of MUL, the product of MULADD (not the addend), every product of DOT4/DOT3/DOT2ADD and DST's y follow
//     Direct3D 9: a 0 factor gives +0 whatever the other one is (inf and NaN included). Components where both factors
//     are the same operand (same register, addressing, modifiers and swizzle component; Xenia's
//     GetIdenticalComponents) skip the check: x * x is only 0 when x is 0, which IEEE already gives;
//   - MAX/MIN are a >= b ? a : b and a < b ? a : b (NaN picks b), the SET/CND compares are ordered except "not equal";
//   - DOT*, MAX4, PRED_SET*_PUSH and the kills replicate their scalar result;
//   - both units read their sources before either writes; a0 (MAXA) and p0 are stored after the register writes,
//     p0 last (the scalar unit's after the vector unit's).
#include <spirv/unified1/GLSL.std.450.h>

#include <string>

#include "runtime/gpu/shader/disasm.h"
#include "runtime/gpu/shader/translator_internal.h"

namespace lr::gpu::shader::detail {

namespace {

// The component source i of `s` reads (0..3).
uint32_t swizzle_component(const AluSource& s, uint32_t i) { return ((s.swizzle >> (2 * i)) + i) & 3; }

// The components in which sources i and j of `a` are bitwise the same value.
uint32_t identical_components(const AluInstr& a, uint32_t i, uint32_t j) {
  const AluSource& x = a.src[i];
  const AluSource& y = a.src[j];
  if (x.is_temp != y.is_temp || x.reg != y.reg || x.negate != y.negate || x.abs != y.abs) return 0;
  if (x.is_temp ? a.is_src_temp_relative[i] != a.is_src_temp_relative[j] : x.const_relative != y.const_relative)
    return 0;
  uint32_t mask = 0;
  for (uint32_t k = 0; k < 4; ++k)
    if (swizzle_component(x, k) == swizzle_component(y, k)) mask |= 1u << k;
  return mask;
}

}  // namespace

void Translator::declare_alu_state() {
  if (!uses_a0_) return;
  a0_ = b_.variable(spv::StorageClassFunction, b_.type_pointer(spv::StorageClassFunction, t_.i32), "a0");
  b_.op_void(spv::OpStore, {a0_, b_.constant_i32(0)});
}

// --- sources ------------------------------------------------------------------------------------------------------

Id Translator::load_relative_constant(const AluInstr& a, uint32_t reg) {
  Id base = 0;
  if (a.a0_rel) {
    if (!a0_) {
      record_internal("a0 read without the a0 state");
      base = b_.constant_i32(0);
    } else {
      base = b_.op(spv::OpLoad, t_.i32, {a0_});
    }
  } else {
    base = current_al();
  }
  const Id index =
      b_.op(spv::OpBitcast, t_.u32, {b_.op(spv::OpIAdd, t_.i32, {base, b_.constant_i32(static_cast<int32_t>(reg))})});
  // A negative sum is a large unsigned one: one unsigned compare covers both ends, and the clamped index keeps the
  // load inside the constant buffer either way.
  const Id in_range = b_.op(spv::OpULessThan, t_.bool_, {index, const_u(kFloatConstantCount)});
  const Id value = load_float_constant(glsl(t_.u32, GLSLstd450UMin, {index, const_u(kFloatConstantCount - 1)}));
  return b_.op(spv::OpSelect, t_.vec4, {in_range, value, const_vec4(0, 0, 0, 0)});
}

Result<Id> Translator::load_source(uint32_t, const AluInstr& a, uint32_t i) {
  const AluSource& s = a.src[i];
  Id v = 0;
  if (s.is_temp) {
    const bool relative = a.is_src_temp_relative[i];
    if (relative ? reg_count_ != kMaxTemps : s.reg >= reg_count_)
      return internal("source register past the register array");
    v = b_.op(spv::OpLoad, t_.vec4, {reg_ptr(temp_index(s.reg, relative))});
  } else if (s.const_relative) {
    v = load_relative_constant(a, s.reg);
  } else {
    v = load_float_constant(const_u(s.reg));
  }
  uint32_t c[4];
  bool identity = true;
  for (uint32_t k = 0; k < 4; ++k) {
    c[k] = swizzle_component(s, k);
    identity = identity && c[k] == k;
  }
  if (!identity) v = b_.op(spv::OpVectorShuffle, t_.vec4, {v, v, c[0], c[1], c[2], c[3]});
  if (s.abs) v = glsl(t_.vec4, GLSLstd450FAbs, {v});
  if (s.negate) v = b_.op(spv::OpFNegate, t_.vec4, {v});
  return v;
}

// --- the vector unit ----------------------------------------------------------------------------------------------

Id Translator::xenos_mul(Id a, Id b, uint32_t identical) {
  const Id product = b_.op(spv::OpFMul, t_.vec4, {a, b});
  if ((identical & 0xF) == 0xF) return product;
  const Id zero4 = const_vec4(0, 0, 0, 0);
  Id zero = b_.op(spv::OpLogicalOr, t_.bvec4,
                  {b_.op(spv::OpFOrdEqual, t_.bvec4, {a, zero4}), b_.op(spv::OpFOrdEqual, t_.bvec4, {b, zero4})});
  if (identical != 0) {
    Id keep[4];
    for (uint32_t k = 0; k < 4; ++k) keep[k] = b_.constant_bool(((identical >> k) & 1) == 0);
    zero = b_.op(spv::OpLogicalAnd, t_.bvec4, {zero, b_.constant_composite(t_.bvec4, keep)});
  }
  return b_.op(spv::OpSelect, t_.vec4, {zero, zero4, product});
}

Result<Translator::VectorResult> Translator::emit_vector_op(uint32_t slot, const AluInstr& a) {
  Id src[3] = {};
  for (uint32_t i = 0; i < vector_op_source_count(a.vector_op); ++i) {
    LR_TRY_ASSIGN(v, load_source(slot, a, i));
    src[i] = v;
  }
  const Id zero4 = const_vec4(0, 0, 0, 0);
  const Id one4 = const_vec4(1, 1, 1, 1);
  const Id zero = const_f(0.0f);
  const Id one = const_f(1.0f);
  const auto splat = [&](Id s) { return b_.op(spv::OpCompositeConstruct, t_.vec4, {s, s, s, s}); };
  const auto comp = [&](Id v, uint32_t k) { return extract(v, k); };
  const auto set = [&](spv::Op compare, Id x, Id y) {
    return b_.op(spv::OpSelect, t_.vec4, {b_.op(compare, t_.bvec4, {x, y}), one4, zero4});
  };
  const auto cnd = [&](spv::Op compare) {
    return b_.op(spv::OpSelect, t_.vec4, {b_.op(compare, t_.bvec4, {src[0], zero4}), src[1], src[2]});
  };
  // The sum of the first n Xenos products of src0 and src1, in component order.
  const auto dot = [&](uint32_t n) {
    const Id p = xenos_mul(src[0], src[1], identical_components(a, 0, 1));
    Id sum = comp(p, 0);
    for (uint32_t k = 1; k < n; ++k) sum = b_.op(spv::OpFAdd, t_.f32, {sum, comp(p, k)});
    return sum;
  };
  // The compare of the predicate pushes and the kills: "not equal" is unordered (NaN holds), the others ordered.
  const auto compare_op = [](VectorOp op) {
    switch (op) {
      case VectorOp::PredSetEqPush:
      case VectorOp::KillEq: return spv::OpFOrdEqual;
      case VectorOp::PredSetGtPush:
      case VectorOp::KillGt: return spv::OpFOrdGreaterThan;
      case VectorOp::PredSetGePush:
      case VectorOp::KillGe: return spv::OpFOrdGreaterThanEqual;
      default: return spv::OpFUnordNotEqual;
    }
  };

  VectorResult out;
  Id r = 0;
  switch (a.vector_op) {
    case VectorOp::Add:
      r = b_.op(spv::OpFAdd, t_.vec4, {src[0], src[1]});
      break;
    case VectorOp::Mul:
      r = xenos_mul(src[0], src[1], identical_components(a, 0, 1));
      break;
    case VectorOp::Max:  // a >= b ? a : b; with src1 == src2 it is the mov idiom
    case VectorOp::MaxA:
      r = b_.op(spv::OpSelect, t_.vec4,
                {b_.op(spv::OpFOrdGreaterThanEqual, t_.bvec4, {src[0], src[1]}), src[0], src[1]});
      if (a.vector_op == VectorOp::MaxA) {
        // a0 = int(clamp(floor(src0.w + 0.5), -256, 255)). NClamp keeps a NaN in range (the conversion of a NaN
        // would be undefined).
        const Id rounded = glsl(t_.f32, GLSLstd450Floor, {b_.op(spv::OpFAdd, t_.f32, {comp(src[0], 3), const_f(0.5f)})});
        const Id clamped = glsl(t_.f32, GLSLstd450NClamp, {rounded, const_f(-256.0f), const_f(255.0f)});
        out.a0 = b_.op(spv::OpConvertFToS, t_.i32, {clamped});
      }
      break;
    case VectorOp::Min:  // a < b ? a : b
      r = b_.op(spv::OpSelect, t_.vec4, {b_.op(spv::OpFOrdLessThan, t_.bvec4, {src[0], src[1]}), src[0], src[1]});
      break;
    case VectorOp::SetEq: r = set(spv::OpFOrdEqual, src[0], src[1]); break;
    case VectorOp::SetGt: r = set(spv::OpFOrdGreaterThan, src[0], src[1]); break;
    case VectorOp::SetGe: r = set(spv::OpFOrdGreaterThanEqual, src[0], src[1]); break;
    case VectorOp::SetNe: r = set(spv::OpFUnordNotEqual, src[0], src[1]); break;
    case VectorOp::Frac: r = glsl(t_.vec4, GLSLstd450Fract, {src[0]}); break;
    case VectorOp::Trunc: r = glsl(t_.vec4, GLSLstd450Trunc, {src[0]}); break;
    case VectorOp::Floor: r = glsl(t_.vec4, GLSLstd450Floor, {src[0]}); break;
    case VectorOp::MulAdd:
      r = b_.op(spv::OpFAdd, t_.vec4, {xenos_mul(src[0], src[1], identical_components(a, 0, 1)), src[2]});
      break;
    case VectorOp::CndEq: r = cnd(spv::OpFOrdEqual); break;
    case VectorOp::CndGe: r = cnd(spv::OpFOrdGreaterThanEqual); break;
    case VectorOp::CndGt: r = cnd(spv::OpFOrdGreaterThan); break;
    case VectorOp::Dot4: r = splat(dot(4)); break;
    case VectorOp::Dot3: r = splat(dot(3)); break;
    case VectorOp::Dot2Add: r = splat(b_.op(spv::OpFAdd, t_.f32, {dot(2), comp(src[2], 0)})); break;
    case VectorOp::Cube: {
      // The game passes src1.zzxy (and src2.yxzz, unused): x = src1[2], y = src1[3], z = src1[0]. The result is
      // (tc, sc, 2 * major axis, face id); ties pick Z, then Y.
      const Id x = comp(src[0], 2), y = comp(src[0], 3), z = comp(src[0], 0);
      const auto absf = [&](Id v) { return glsl(t_.f32, GLSLstd450FAbs, {v}); };
      const auto neg = [&](Id v) { return b_.op(spv::OpFNegate, t_.f32, {v}); };
      const auto ge = [&](Id p, Id q) { return b_.op(spv::OpFOrdGreaterThanEqual, t_.bool_, {p, q}); };
      const auto less0 = [&](Id v) { return b_.op(spv::OpFOrdLessThan, t_.bool_, {v, zero}); };
      const auto sel = [&](Id c, Id p, Id q) { return b_.op(spv::OpSelect, t_.f32, {c, p, q}); };
      const Id ax = absf(x), ay = absf(y), az = absf(z);
      const Id is_z = b_.op(spv::OpLogicalAnd, t_.bool_, {ge(az, ax), ge(az, ay)});
      const Id is_y = b_.op(spv::OpLogicalAnd, t_.bool_, {b_.op(spv::OpLogicalNot, t_.bool_, {is_z}), ge(ay, ax)});
      // X major: tc = -y, sc = x < 0 ? z : -z, ma = x, id = x < 0 ? 1 : 0.
      Id tc = neg(y);
      Id sc = sel(less0(x), z, neg(z));
      Id ma = x;
      Id id = sel(less0(x), const_f(1.0f), zero);
      // Y major: tc = y < 0 ? -z : z, sc = x, ma = y, id = y < 0 ? 3 : 2.
      tc = sel(is_y, sel(less0(y), neg(z), z), tc);
      sc = sel(is_y, x, sc);
      ma = sel(is_y, y, ma);
      id = sel(is_y, sel(less0(y), const_f(3.0f), const_f(2.0f)), id);
      // Z major: tc = -y, sc = z < 0 ? -x : x, ma = z, id = z < 0 ? 5 : 4.
      tc = sel(is_z, neg(y), tc);
      sc = sel(is_z, sel(less0(z), neg(x), x), sc);
      ma = sel(is_z, z, ma);
      id = sel(is_z, sel(less0(z), const_f(5.0f), const_f(4.0f)), id);
      r = b_.op(spv::OpCompositeConstruct, t_.vec4, {tc, sc, b_.op(spv::OpFMul, t_.f32, {const_f(2.0f), ma}), id});
      break;
    }
    case VectorOp::Max4: {
      // NMax, as Xenia: a NaN component is ignored.
      Id m = comp(src[0], 0);
      for (uint32_t k = 1; k < 4; ++k) m = glsl(t_.f32, GLSLstd450NMax, {m, comp(src[0], k)});
      r = splat(m);
      break;
    }
    case VectorOp::PredSetEqPush:
    case VectorOp::PredSetNePush:
    case VectorOp::PredSetGtPush:
    case VectorOp::PredSetGePush: {
      // p0 = src0.w == 0 && src1.w <op> 0; result = (src0.x == 0 && src1.x <op> 0) ? 0 : src0.x + 1.
      const spv::Op op = compare_op(a.vector_op);
      const auto holds = [&](uint32_t k) {
        return b_.op(spv::OpLogicalAnd, t_.bool_,
                     {b_.op(spv::OpFOrdEqual, t_.bool_, {comp(src[0], k), zero}),
                      b_.op(op, t_.bool_, {comp(src[1], k), zero})});
      };
      out.p0 = holds(3);
      r = splat(b_.op(spv::OpSelect, t_.f32, {holds(0), zero, b_.op(spv::OpFAdd, t_.f32, {comp(src[0], 0), one})}));
      break;
    }
    case VectorOp::KillEq:
    case VectorOp::KillGt:
    case VectorOp::KillGe:
    case VectorOp::KillNe: {
      // Any component of src0 <op> src1 kills; the result is 1 then, else 0. A pixel shader only sets the killed
      // flag (main ends with the one OpKill); a vertex shader has nothing to discard and only gets the result.
      const Id kills = b_.op(spv::OpAny, t_.bool_, {b_.op(compare_op(a.vector_op), t_.bvec4, {src[0], src[1]})});
      if (!is_vs()) {
        if (!killed_) return internal("kill without the killed flag");
        kill_if(kills);
      }
      r = splat(b_.op(spv::OpSelect, t_.f32, {kills, one, zero}));
      break;
    }
    case VectorOp::Dst: {
      // (1, src0.y * src1.y, src0.z, src1.w), the product with the Xenos rule.
      const Id p = xenos_mul(src[0], src[1], identical_components(a, 0, 1));
      r = b_.op(spv::OpCompositeConstruct, t_.vec4, {one, comp(p, 1), comp(src[0], 2), comp(src[1], 3)});
      break;
    }
  }
  if (!r) return unsupported(slot, "alu", "vector op " + std::string(vector_op_name(a.vector_op)));
  if (a.vector_saturate) r = glsl(t_.vec4, GLSLstd450NClamp, {r, zero4, one4});
  out.value = r;
  return out;
}

// --- the instruction ----------------------------------------------------------------------------------------------

Result<void> Translator::emit_alu(uint32_t slot, const AluInstr& a) {
  // A predicated instruction runs when p0 == pred_condition: a branch around it (only fetches must avoid the branch,
  // for their derivatives; Task 15.8 evaluates them unconditionally and merges with OpSelect).
  if (!a.predicated) return emit_alu_unpredicated(slot, a);
  return emit_if(pred_matches(a.pred_condition), [&] { return emit_alu_unpredicated(slot, a); });
}

Result<void> Translator::emit_alu_unpredicated(uint32_t slot, const AluInstr& a) {
  const uint32_t vector_mask = a.export_data ? a.export_vector_mask : a.vector_write_mask;
  const uint32_t scalar_mask = a.export_data ? a.export_scalar_mask : a.scalar_write_mask;
  const bool scalar_issued = scalar_mask != 0 || scalar_op_has_side_effect(a.scalar_op);
  if (scalar_issued && !(a.scalar_op >= ScalarOp::PredSetEq && a.scalar_op <= ScalarOp::PredSetGe))
    return unsupported(slot, "alu", "scalar op " + std::string(scalar_op_name(a.scalar_op)));
  // Both units read their sources before either writes.
  VectorResult vector;
  if (vector_mask != 0 || vector_op_has_side_effect(a.vector_op)) {
    LR_TRY_ASSIGN(v, emit_vector_op(slot, a));
    vector = v;
  }
  Id scalar_result = 0;
  Id scalar_p0 = 0;
  if (scalar_issued) {
    LR_TRY_ASSIGN(s, emit_scalar_pred_set(slot, a));
    scalar_result = s.first;
    scalar_p0 = s.second;
  }
  if (a.export_data) {
    LR_TRY(emit_export(slot, a, vector.value, scalar_result));
  } else {
    const auto store = [&](uint32_t reg, bool relative, Id value, uint32_t mask) -> Result<void> {
      if (relative ? reg_count_ != kMaxTemps : reg >= reg_count_)
        return internal("destination register past the register array");
      store_reg_masked_at(temp_index(reg, relative), value, mask);
      return {};
    };
    if (a.vector_write_mask != 0) LR_TRY(store(a.vector_dest, a.vector_dest_rel, vector.value, a.vector_write_mask));
    if (a.scalar_write_mask != 0) {
      const Id s = scalar_result;
      const Id splat = b_.op(spv::OpCompositeConstruct, t_.vec4, {s, s, s, s});
      LR_TRY(store(a.scalar_dest, a.scalar_dest_rel, splat, a.scalar_write_mask));
    }
  }
  if (vector.a0) {
    if (!a0_) return internal("a0 written without the a0 state");
    b_.op_void(spv::OpStore, {a0_, vector.a0});
  }
  // p0 last; when both units set it, the scalar unit's value stays.
  if (vector.p0) b_.op_void(spv::OpStore, {p0_, vector.p0});
  if (scalar_p0) b_.op_void(spv::OpStore, {p0_, scalar_p0});
  return {};
}

}  // namespace lr::gpu::shader::detail
