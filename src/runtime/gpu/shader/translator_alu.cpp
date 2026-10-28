// SPDX-License-Identifier: MIT
// The ALU: the vector unit (Task 15.6) and the scalar unit (Task 15.7). Semantics per Xenia's ucode.h and SPIR-V
// translator (reference only, nothing copied):
//   - sources: temporaries (absolute or r[aL + n], clamped to 0..63) or float constants (absolute, or c[n + aL] /
//     c[n + a0] per the const_0/const_1 rule the decoder resolves; a sum outside 0..255 reads 0), then the swizzle,
//     abs, negate;
//   - the multiply of MUL, the product of MULADD (not the addend), every product of DOT4/DOT3/DOT2ADD, DST's y and the
//     scalar MULs, MUL_PREVs, MUL_PREV2s and MUL_CONST_0/1 follow Direct3D 9: a 0 factor gives +0 whatever the other one
//     is (inf and NaN included). Components where both factors are the same operand (same register, addressing,
//     modifiers and swizzle component; Xenia's GetIdenticalComponents) skip the check: x * x is only 0 when x is 0,
//     which IEEE already gives;
//   - MAX/MIN are a >= b ? a : b and a < b ? a : b (NaN picks b), the SET/CND compares are ordered except "not equal";
//   - DOT*, MAX4, PRED_SET*_PUSH and the kills replicate their scalar result;
//   - the scalar unit reads a = src3's W and b = src3's X (after the swizzle); the *_CONST ops read a from the constant
//     src3 and b from the temporary scalar_const_temp's X-selected component, both with src3's negate and abs_constants.
//     Every scalar op but RETAIN_PREV runs whatever its write mask, and its result (before saturation, as in Xenia)
//     becomes the previous scalar result ps;
//   - a kill's result is 1.0 when it kills and 0.0 otherwise (ucode.h; Xenia's SPIR-V translator returns 0.0);
//   - every float add, sub, mul, div and negate is NoContraction (no FMA fusing), so results do not depend on the
//     pipeline;
//   - both units read their sources before either writes; a0 (MAXA, then MOVA*) and p0 (the vector unit's, then the
//     scalar unit's) are stored after the register writes, so the scalar unit's value stays when both set one.
#include <spirv/unified1/GLSL.std.450.h>

#include <limits>
#include <string>

#include "runtime/gpu/shader/disasm.h"
#include "runtime/gpu/shader/translator_internal.h"

namespace lr::gpu::shader::detail {

namespace {

constexpr float kFltMax = std::numeric_limits<float>::max();
constexpr float kInf = std::numeric_limits<float>::infinity();

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
  if (uses_a0_) {
    a0_ = b_.variable(spv::StorageClassFunction, b_.type_pointer(spv::StorageClassFunction, t_.i32), "a0");
    b_.op_void(spv::OpStore, {a0_, b_.constant_i32(0)});
  }
  if (uses_ps_) {
    ps_ = b_.variable(spv::StorageClassFunction, b_.type_pointer(spv::StorageClassFunction, t_.f32), "ps");
    b_.op_void(spv::OpStore, {ps_, const_f(0.0f)});
  }
}

Id Translator::arith(spv::Op opcode, Id type, std::initializer_list<Id> operands) {
  const Id id = b_.op(opcode, type, operands);
  b_.decorate(id, spv::DecorationNoContraction);
  return id;
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
  if (s.negate) v = arith(spv::OpFNegate, t_.vec4, {v});
  return v;
}

// --- the Xenos multiply -------------------------------------------------------------------------------------------

Id Translator::xenos_mul(Id a, Id b, uint32_t identical) {
  const Id product = arith(spv::OpFMul, t_.vec4, {a, b});
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

Id Translator::xenos_mul_f32(Id a, Id b, bool identical) {
  const Id product = arith(spv::OpFMul, t_.f32, {a, b});
  if (identical) return product;
  const Id zero = const_f(0.0f);
  const Id is_zero = b_.op(spv::OpLogicalOr, t_.bool_,
                           {b_.op(spv::OpFOrdEqual, t_.bool_, {a, zero}), b_.op(spv::OpFOrdEqual, t_.bool_, {b, zero})});
  return b_.op(spv::OpSelect, t_.f32, {is_zero, zero, product});
}

// --- the vector unit ----------------------------------------------------------------------------------------------

Result<Translator::UnitResult> Translator::emit_vector_op(uint32_t slot, const AluInstr& a) {
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
    for (uint32_t k = 1; k < n; ++k) sum = arith(spv::OpFAdd, t_.f32, {sum, comp(p, k)});
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

  UnitResult out;
  Id r = 0;
  switch (a.vector_op) {
    case VectorOp::Add:
      r = arith(spv::OpFAdd, t_.vec4, {src[0], src[1]});
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
        const Id rounded = glsl(t_.f32, GLSLstd450Floor, {arith(spv::OpFAdd, t_.f32, {comp(src[0], 3), const_f(0.5f)})});
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
      r = arith(spv::OpFAdd, t_.vec4, {xenos_mul(src[0], src[1], identical_components(a, 0, 1)), src[2]});
      break;
    case VectorOp::CndEq: r = cnd(spv::OpFOrdEqual); break;
    case VectorOp::CndGe: r = cnd(spv::OpFOrdGreaterThanEqual); break;
    case VectorOp::CndGt: r = cnd(spv::OpFOrdGreaterThan); break;
    case VectorOp::Dot4: r = splat(dot(4)); break;
    case VectorOp::Dot3: r = splat(dot(3)); break;
    case VectorOp::Dot2Add: r = splat(arith(spv::OpFAdd, t_.f32, {dot(2), comp(src[2], 0)})); break;
    case VectorOp::Cube: {
      // The game passes src1.zzxy (and src2.yxzz, unused): x = src1[2], y = src1[3], z = src1[0]. The result is
      // (tc, sc, 2 * major axis, face id); ties pick Z, then Y.
      const Id x = comp(src[0], 2), y = comp(src[0], 3), z = comp(src[0], 0);
      const auto absf = [&](Id v) { return glsl(t_.f32, GLSLstd450FAbs, {v}); };
      const auto neg = [&](Id v) { return arith(spv::OpFNegate, t_.f32, {v}); };
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
      r = b_.op(spv::OpCompositeConstruct, t_.vec4, {tc, sc, arith(spv::OpFMul, t_.f32, {const_f(2.0f), ma}), id});
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
      r = splat(b_.op(spv::OpSelect, t_.f32, {holds(0), zero, arith(spv::OpFAdd, t_.f32, {comp(src[0], 0), one})}));
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

// --- the scalar unit ----------------------------------------------------------------------------------------------

Result<Translator::UnitResult> Translator::emit_scalar_op(uint32_t slot, const AluInstr& a) {
  const ScalarOp op = a.scalar_op;
  // The operands: a = src3's W, b = src3's X; for the CONST ops a = the constant's W, b = the temporary's X.
  Id sa = 0, sb = 0;
  bool same_operand = false;  // a and b are bitwise the same value (the multiply rule's exception)
  if (scalar_op_is_const(op)) {
    LR_TRY_ASSIGN(constant, load_source(slot, a, 2));
    sa = extract(constant, 3);
    if (a.scalar_const_temp >= reg_count_) return internal("scalar constant-op temporary past the register array");
    sb = extract(load_reg(a.scalar_const_temp), swizzle_component(a.src[2], 0));
    if (a.src[2].abs) sb = glsl(t_.f32, GLSLstd450FAbs, {sb});
    if (a.src[2].negate) sb = arith(spv::OpFNegate, t_.f32, {sb});
  } else if (scalar_op_operand_count(op) > 0) {
    LR_TRY_ASSIGN(v, load_source(slot, a, 2));
    sa = extract(v, 3);
    sb = extract(v, 0);
    same_operand = swizzle_component(a.src[2], 3) == swizzle_component(a.src[2], 0);
  }
  Id ps = 0;
  if (scalar_op_reads_ps(op)) {
    if (!ps_) return internal("ps read without the ps state");
    ps = b_.op(spv::OpLoad, t_.f32, {ps_});
  }

  const Id zero = const_f(0.0f);
  const Id one = const_f(1.0f);
  const Id flt_max = const_f(kFltMax);
  const Id neg_flt_max = const_f(-kFltMax);
  const Id inf = const_f(kInf);
  const Id neg_inf = const_f(-kInf);
  const auto sel = [&](Id c, Id p, Id q) { return b_.op(spv::OpSelect, t_.f32, {c, p, q}); };
  const auto cmp = [&](spv::Op compare, Id x, Id y) { return b_.op(compare, t_.bool_, {x, y}); };
  const auto both = [&](Id p, Id q) { return b_.op(spv::OpLogicalAnd, t_.bool_, {p, q}); };
  const auto bool_to_float = [&](Id c) { return sel(c, one, zero); };
  // ucode.h: the transcendentals of 1.0 give their exact value (log 0, 1/x and 1/sqrt(x) 1).
  const auto at_one = [&](Id exact, Id value) { return sel(cmp(spv::OpFOrdEqual, sa, one), exact, value); };
  // The edge cases of the transcendentals are decided from the input with selects, never by testing a driver's result
  // (GLSL.std.450 leaves log2(0), inversesqrt(0) and friends undefined, and SignedZeroInfNanPreserve is not enabled).
  // The sign comes from the sign bit, so -0 counts as negative. Denormals count as a signed zero (the Xenos ALU
  // flushes them; it also keeps 1/x finite for every other finite input).
  const auto constant_by_sign = [&](float v) {
    const Id sign_bit = b_.op(spv::OpBitwiseAnd, t_.u32, {b_.op(spv::OpBitcast, t_.u32, {sa}), const_u(0x80000000u)});
    return sel(b_.op(spv::OpINotEqual, t_.bool_, {sign_bit, const_u(0)}), const_f(-v), const_f(v));
  };
  const auto abs_a = [&] { return glsl(t_.f32, GLSLstd450FAbs, {sa}); };
  const auto is_tiny = [&] { return cmp(spv::OpFOrdLessThan, abs_a(), const_f(std::numeric_limits<float>::min())); };
  const auto is_inf = [&] { return cmp(spv::OpFOrdEqual, abs_a(), inf); };
  const auto is_pos_inf = [&] { return cmp(spv::OpFOrdEqual, sa, inf); };
  // Negative and not a (flushed) zero: log, sqrt and 1/sqrt give NaN.
  const auto is_negative = [&] {
    return both(cmp(spv::OpFOrdLessThan, sa, zero), b_.op(spv::OpLogicalNot, t_.bool_, {is_tiny()}));
  };
  const Id nan = const_f(std::numeric_limits<float>::quiet_NaN());
  // RECIP_*: +-0 gives +-zero_result (inf, FLT_MAX or 0 by variant), +-inf gives +-0.
  const auto recip = [&](float zero_result) {
    const Id general = at_one(one, arith(spv::OpFDiv, t_.f32, {one, sa}));
    return sel(is_tiny(), constant_by_sign(zero_result), sel(is_inf(), constant_by_sign(0.0f), general));
  };
  // RECIPSQ_*: +-0 gives +-zero_result, +inf gives +0, a negative input NaN.
  const auto recip_sqrt = [&](float zero_result) {
    const Id general = at_one(one, glsl(t_.f32, GLSLstd450InverseSqrt, {sa}));
    return sel(is_tiny(), constant_by_sign(zero_result),
               sel(is_pos_inf(), zero, sel(is_negative(), nan, general)));
  };
  // LOG_*: +-0 gives zero_result (-inf, or -FLT_MAX clamped), +inf gives +inf, a negative input NaN.
  const auto log_of = [&](float zero_result) {
    const Id general = at_one(zero, glsl(t_.f32, GLSLstd450Log2, {sa}));
    return sel(is_tiny(), const_f(zero_result), sel(is_pos_inf(), inf, sel(is_negative(), nan, general)));
  };
  // p0 = a <op> 0 for PRED_SET{E,NE,GT,GTE}s and the kills: "not equal" is unordered (NaN holds), the others ordered.
  const auto compare_zero = [&](ScalarOp o) {
    switch (o) {
      case ScalarOp::PredSetEq:
      case ScalarOp::KillEq: return cmp(spv::OpFOrdEqual, sa, zero);
      case ScalarOp::PredSetGt:
      case ScalarOp::KillGt: return cmp(spv::OpFOrdGreaterThan, sa, zero);
      case ScalarOp::PredSetGe:
      case ScalarOp::KillGe: return cmp(spv::OpFOrdGreaterThanEqual, sa, zero);
      case ScalarOp::KillOne: return cmp(spv::OpFOrdEqual, sa, one);
      default: return cmp(spv::OpFUnordNotEqual, sa, zero);
    }
  };

  UnitResult out;
  Id r = 0;
  switch (op) {
    case ScalarOp::Add:
    case ScalarOp::AddConst0:
    case ScalarOp::AddConst1: r = arith(spv::OpFAdd, t_.f32, {sa, sb}); break;
    case ScalarOp::AddPrev: r = arith(spv::OpFAdd, t_.f32, {sa, ps}); break;
    case ScalarOp::Sub:
    case ScalarOp::SubConst0:
    case ScalarOp::SubConst1: r = arith(spv::OpFSub, t_.f32, {sa, sb}); break;  // the constant is the minuend
    case ScalarOp::SubPrev: r = arith(spv::OpFSub, t_.f32, {sa, ps}); break;
    case ScalarOp::Mul: r = xenos_mul_f32(sa, sb, same_operand); break;
    case ScalarOp::MulConst0:
    case ScalarOp::MulConst1: r = xenos_mul_f32(sa, sb, false); break;  // a constant and a temporary never match
    case ScalarOp::MulPrev: r = xenos_mul_f32(sa, ps, false); break;
    case ScalarOp::MulPrev2: {
      // a * ps, unless ps is -FLT_MAX or not finite, or b is not finite or not > 0: -FLT_MAX then.
      const auto finite = [&](Id v) {
        return cmp(spv::OpFOrdLessThanEqual, glsl(t_.f32, GLSLstd450FAbs, {v}), flt_max);  // false for inf and NaN
      };
      const Id ok = both(both(cmp(spv::OpFUnordNotEqual, ps, neg_flt_max), finite(ps)),
                         both(finite(sb), cmp(spv::OpFOrdGreaterThan, sb, zero)));
      r = sel(ok, xenos_mul_f32(sa, ps, false), neg_flt_max);
      break;
    }
    case ScalarOp::Max:  // a >= b ? a : b (NaN picks b)
    case ScalarOp::MovA:
    case ScalarOp::MovAFloor:
      r = sel(cmp(spv::OpFOrdGreaterThanEqual, sa, sb), sa, sb);
      if (op != ScalarOp::Max) {
        // a0 = int(clamp(floor(a + 0.5), -256, 255)) (MOVA) or int(clamp(floor(a), -256, 255)) (MOVA_FLOOR); NClamp
        // keeps a NaN in range.
        const Id x = op == ScalarOp::MovA ? arith(spv::OpFAdd, t_.f32, {sa, const_f(0.5f)}) : sa;
        const Id clamped =
            glsl(t_.f32, GLSLstd450NClamp, {glsl(t_.f32, GLSLstd450Floor, {x}), const_f(-256.0f), const_f(255.0f)});
        out.a0 = b_.op(spv::OpConvertFToS, t_.i32, {clamped});
      }
      break;
    case ScalarOp::Min: r = sel(cmp(spv::OpFOrdLessThan, sa, sb), sa, sb); break;
    case ScalarOp::SetEq: r = bool_to_float(cmp(spv::OpFOrdEqual, sa, zero)); break;
    case ScalarOp::SetGt: r = bool_to_float(cmp(spv::OpFOrdGreaterThan, sa, zero)); break;
    case ScalarOp::SetGe: r = bool_to_float(cmp(spv::OpFOrdGreaterThanEqual, sa, zero)); break;
    case ScalarOp::SetNe: r = bool_to_float(cmp(spv::OpFUnordNotEqual, sa, zero)); break;
    case ScalarOp::Frac: r = glsl(t_.f32, GLSLstd450Fract, {sa}); break;
    case ScalarOp::Trunc: r = glsl(t_.f32, GLSLstd450Trunc, {sa}); break;
    case ScalarOp::Floor: r = glsl(t_.f32, GLSLstd450Floor, {sa}); break;
    case ScalarOp::Exp:
      // ucode.h: exactly 1 at +-0; -inf gives +0, +inf gives +inf.
      r = sel(is_tiny(), one,
              sel(cmp(spv::OpFOrdEqual, sa, neg_inf), zero, sel(is_pos_inf(), inf, glsl(t_.f32, GLSLstd450Exp2, {sa}))));
      break;
    case ScalarOp::Log: r = log_of(-kInf); break;
    case ScalarOp::LogClamp: r = log_of(-kFltMax); break;
    case ScalarOp::Recip: r = recip(kInf); break;
    case ScalarOp::RecipClamp: r = recip(kFltMax); break;
    case ScalarOp::RecipFixedFunc: r = recip(0.0f); break;
    case ScalarOp::RecipSqrt: r = recip_sqrt(kInf); break;
    case ScalarOp::RecipSqrtClamp: r = recip_sqrt(kFltMax); break;
    case ScalarOp::RecipSqrtFixedFunc: r = recip_sqrt(0.0f); break;
    case ScalarOp::Sqrt:
      // +-0 passes through (with its sign), +inf gives +inf, a negative input NaN.
      r = sel(is_tiny(), constant_by_sign(0.0f),
              sel(is_pos_inf(), inf, sel(is_negative(), nan, glsl(t_.f32, GLSLstd450Sqrt, {sa}))));
      break;
    case ScalarOp::Sin: r = glsl(t_.f32, GLSLstd450Sin, {sa}); break;  // radians, unscaled
    case ScalarOp::Cos: r = glsl(t_.f32, GLSLstd450Cos, {sa}); break;
    case ScalarOp::PredSetEq:
    case ScalarOp::PredSetNe:
    case ScalarOp::PredSetGt:
    case ScalarOp::PredSetGe:
      // p0 = a <op> 0; the result is 0 when it holds, else 1.
      out.p0 = compare_zero(op);
      r = sel(out.p0, zero, one);
      break;
    case ScalarOp::PredSetInv:
      // p0 = a == 1; the result is 0 then, else (a == 0 ? 1 : a).
      out.p0 = cmp(spv::OpFOrdEqual, sa, one);
      r = sel(out.p0, zero, sel(cmp(spv::OpFOrdEqual, sa, zero), one, sa));
      break;
    case ScalarOp::PredSetPop: {
      // p0 = a - 1 <= 0; the result is 0 then, else a - 1.
      const Id t = arith(spv::OpFSub, t_.f32, {sa, one});
      out.p0 = cmp(spv::OpFOrdLessThanEqual, t, zero);
      r = sel(out.p0, zero, t);
      break;
    }
    case ScalarOp::PredSetClr:
      out.p0 = b_.constant_bool(false);
      r = flt_max;
      break;
    case ScalarOp::PredSetRestore:
      // p0 = a == 0; the result is 0 then, else a.
      out.p0 = cmp(spv::OpFOrdEqual, sa, zero);
      r = sel(out.p0, zero, sa);
      break;
    case ScalarOp::KillEq:
    case ScalarOp::KillGt:
    case ScalarOp::KillGe:
    case ScalarOp::KillNe:
    case ScalarOp::KillOne: {
      // As the vector kills: the result is 1 when it kills, else 0; only a pixel shader discards (at the end).
      const Id kills = compare_zero(op);
      if (!is_vs()) {
        if (!killed_) return internal("kill without the killed flag");
        kill_if(kills);
      }
      r = bool_to_float(kills);
      break;
    }
    case ScalarOp::RetainPrev: r = ps; break;
    case ScalarOp::Invalid41: break;
  }
  if (!r) return unsupported(slot, "alu", "scalar op " + std::string(scalar_op_name(op)));
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
  // Both units read their sources before either writes.
  UnitResult vector, scalar;
  if (vector_unit_runs(a)) {
    LR_TRY_ASSIGN(v, emit_vector_op(slot, a));
    vector = v;
  }
  if (scalar_unit_runs(a)) {
    LR_TRY_ASSIGN(s, emit_scalar_op(slot, a));
    scalar = s;
  }
  if ((vector.p0 || scalar.p0) && !p0_) return internal("predicate set before the control-flow state exists");
  // The written scalar value is saturated; ps keeps the unsaturated one (as in Xenia).
  Id scalar_written = scalar.value;
  if (scalar_written && a.scalar_saturate)
    scalar_written = glsl(t_.f32, GLSLstd450NClamp, {scalar_written, const_f(0.0f), const_f(1.0f)});
  if (a.export_data) {
    LR_TRY(emit_export(slot, a, vector.value, scalar_written));
  } else {
    const auto store = [&](uint32_t reg, bool relative, Id value, uint32_t mask) -> Result<void> {
      if (relative ? reg_count_ != kMaxTemps : reg >= reg_count_)
        return internal("destination register past the register array");
      store_reg_masked_at(temp_index(reg, relative), value, mask);
      return {};
    };
    // Vector first, then scalar: each store keeps the components outside its mask, so results that target the same
    // register merge by write mask.
    if (a.vector_write_mask != 0) LR_TRY(store(a.vector_dest, a.vector_dest_rel, vector.value, a.vector_write_mask));
    if (a.scalar_write_mask != 0) {
      const Id s = scalar_written;
      const Id splat = b_.op(spv::OpCompositeConstruct, t_.vec4, {s, s, s, s});
      LR_TRY(store(a.scalar_dest, a.scalar_dest_rel, splat, a.scalar_write_mask));
    }
  }
  if (scalar.value && ps_ && a.scalar_op != ScalarOp::RetainPrev) b_.op_void(spv::OpStore, {ps_, scalar.value});
  // a0, then p0; when both units set one, the scalar unit's value stays.
  for (Id value : {vector.a0, scalar.a0}) {
    if (!value) continue;
    if (!a0_) return internal("a0 written without the a0 state");
    b_.op_void(spv::OpStore, {a0_, value});
  }
  if (vector.p0) b_.op_void(spv::OpStore, {p0_, vector.p0});
  if (scalar.p0) b_.op_void(spv::OpStore, {p0_, scalar.p0});
  return {};
}

}  // namespace lr::gpu::shader::detail
