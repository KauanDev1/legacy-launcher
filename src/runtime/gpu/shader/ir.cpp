// SPDX-License-Identifier: MIT
// Xenos microcode -> Program. Encoding per Xenia ucode.h (reference only); control-flow pairing and the end of the CF
// area follow the semantics described for Xenia's shader_translator.cc (reference only). Written from our own
// understanding: every guest-controlled address, count and index is checked before use.
#include "runtime/gpu/shader/ir.h"

#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

#include "runtime/gpu/shader/disasm.h"
#include "runtime/gpu/shader/ucode.h"

namespace lr::gpu::shader {

namespace u = ucode;

bool is_exec(CfOp op) {
  switch (op) {
    case CfOp::Exec:
    case CfOp::ExecEnd:
    case CfOp::CondExec:
    case CfOp::CondExecEnd:
    case CfOp::CondPredExec:
    case CfOp::CondPredExecEnd:
    case CfOp::CondExecPredClean:
    case CfOp::CondExecPredCleanEnd:
      return true;
    default:
      return false;
  }
}

bool is_exec_end(CfOp op) {
  return op == CfOp::ExecEnd || op == CfOp::CondExecEnd || op == CfOp::CondPredExecEnd ||
         op == CfOp::CondExecPredCleanEnd;
}

uint32_t vector_op_source_count(VectorOp op) {
  switch (op) {
    case VectorOp::Frac:
    case VectorOp::Trunc:
    case VectorOp::Floor:
    case VectorOp::Max4:
      return 1;
    case VectorOp::MulAdd:
    case VectorOp::CndEq:
    case VectorOp::CndGe:
    case VectorOp::CndGt:
    case VectorOp::Dot2Add:
      return 3;
    default:
      return 2;
  }
}

bool is_kill(VectorOp op) { return op >= VectorOp::KillEq && op <= VectorOp::KillNe; }
bool is_kill(ScalarOp op) { return op >= ScalarOp::KillEq && op <= ScalarOp::KillOne; }

bool vector_op_has_side_effect(VectorOp op) {
  return (op >= VectorOp::PredSetEqPush && op <= VectorOp::PredSetGePush) || is_kill(op) || op == VectorOp::MaxA;
}

bool scalar_op_is_const(ScalarOp op) { return op >= ScalarOp::MulConst0 && op <= ScalarOp::SubConst1; }

uint32_t scalar_op_operand_count(ScalarOp op) {
  switch (op) {
    case ScalarOp::RetainPrev:
    case ScalarOp::PredSetClr:
      return 0;
    case ScalarOp::Add:
    case ScalarOp::Mul:
    case ScalarOp::Max:
    case ScalarOp::Min:
    case ScalarOp::Sub:
    case ScalarOp::MovA:
    case ScalarOp::MovAFloor:
      return 2;
    default:
      return scalar_op_is_const(op) ? 2 : 1;
  }
}

bool scalar_op_has_side_effect(ScalarOp op) {
  return (op >= ScalarOp::PredSetEq && op <= ScalarOp::PredSetRestore) || is_kill(op) || op == ScalarOp::MovA ||
         op == ScalarOp::MovAFloor;
}

bool fetch_writes_dest(const FetchInstr& f) {
  if (f.op == FetchOp::SetTextureLod || f.op == FetchOp::SetTextureGradientsHorz ||
      f.op == FetchOp::SetTextureGradientsVert)
    return false;
  for (uint32_t i = 0; i < 4; ++i)
    if (((f.dst_swizzle >> (3 * i)) & 7) != kFetchSwizzleKeep) return true;
  return false;
}

namespace {

Error fail(uint32_t index, std::string_view kind, std::string_view what) {
  return Error{ErrorCode::InvalidFormat,
               "instruction " + std::to_string(index) + " (" + std::string(kind) + "): " + std::string(what), ""};
}

std::string cf_kind(CfOp op) { return "cf " + std::string(cf_op_name(op)); }

CfInstr decode_cf(uint64_t w) {
  CfInstr c;
  c.op = static_cast<CfOp>(u::get(w, u::cf::kOpcode));  // 4 bits: every value is an op
  const auto bit = [&](u::Field f) { return u::get(w, f) != 0; };
  const auto field = [&](u::Field f) { return static_cast<uint32_t>(u::get(w, f)); };
  switch (c.op) {
    case CfOp::Exec:
    case CfOp::ExecEnd:
      c.address = field(u::cf::kExecAddress);
      c.count = field(u::cf::kExecCount);
      c.sequence = field(u::cf::kExecSequence);
      break;
    case CfOp::CondExec:
    case CfOp::CondExecEnd:
    case CfOp::CondExecPredClean:
    case CfOp::CondExecPredCleanEnd:
      c.address = field(u::cf::kExecAddress);
      c.count = field(u::cf::kExecCount);
      c.sequence = field(u::cf::kExecSequence);
      c.bool_index = field(u::cf::kBoolAddress);
      c.condition = bit(u::cf::kCondition);
      break;
    case CfOp::CondPredExec:
    case CfOp::CondPredExecEnd:
      c.address = field(u::cf::kExecAddress);
      c.count = field(u::cf::kExecCount);
      c.sequence = field(u::cf::kExecSequence);
      c.condition = bit(u::cf::kCondition);
      c.use_predicate = true;
      break;
    case CfOp::LoopStart:
      c.address = c.target = field(u::cf::kTargetAddress);
      c.loop_id = field(u::cf::kLoopId);
      c.is_repeat = bit(u::cf::kLoopRepeat);
      break;
    case CfOp::LoopEnd:
      c.address = c.target = field(u::cf::kTargetAddress);
      c.loop_id = field(u::cf::kLoopId);
      c.is_predicated_break = bit(u::cf::kLoopPredBreak);
      c.pred_break_condition = bit(u::cf::kCondition);
      break;
    case CfOp::CondCall:
    case CfOp::CondJmp:
      c.address = c.target = field(u::cf::kTargetAddress);
      c.is_unconditional = bit(u::cf::kBranchUnconditional);
      if (!c.is_unconditional) {
        c.condition = bit(u::cf::kCondition);
        c.use_predicate = bit(u::cf::kBranchPredicated);
        if (!c.use_predicate) c.bool_index = field(u::cf::kBoolAddress);
      }
      break;
    case CfOp::Alloc:
      c.count = field(u::cf::kAllocSize);
      c.alloc_type = field(u::cf::kAllocType);
      break;
    case CfOp::Nop:
    case CfOp::Return:
    case CfOp::MarkVsFetchDone:
      break;
  }
  return c;
}

bool uses_bool(const CfInstr& c) {
  switch (c.op) {
    case CfOp::CondExec:
    case CfOp::CondExecEnd:
    case CfOp::CondExecPredClean:
    case CfOp::CondExecPredCleanEnd:
      return true;
    case CfOp::CondCall:
    case CfOp::CondJmp:
      return !c.is_unconditional && !c.use_predicate;
    default:
      return false;
  }
}

// Builds the Program summary while slots are decoded.
class Decoder {
 public:
  Decoder(ShaderStage stage, std::span<const uint32_t> code) : code_(code) { p_.stage = stage; }

  Result<Program> run();

 private:
  Result<void> decode_cf_area();
  Result<void> check_cf(uint32_t index);
  Result<void> decode_exec(uint32_t index);
  Result<AluInstr> decode_alu(uint32_t slot, const uint32_t* w);
  Result<FetchInstr> decode_fetch(uint32_t slot, const uint32_t* w);
  Result<void> note_export(uint32_t slot, uint32_t reg);
  void note_temp(uint32_t reg, bool relative) {
    if (relative)
      p_.uses_relative_temps = true;
    else
      max_temp_ = std::max(max_temp_, reg + 1);
  }

  std::span<const uint32_t> code_;
  Program p_;
  uint32_t total_slots_ = 0;
  uint32_t cf_slots_ = 0;              // slots the CF area occupies
  std::vector<uint8_t> slot_kind_;     // 0 not decoded, 1 ALU, 2 fetch
  uint32_t max_temp_ = 0;
  bool have_full_vfetch_ = false;
  FetchInstr last_full_vfetch_;
};

Result<void> Decoder::decode_cf_area() {
  // The CF area runs from slot 0 to the lowest slot an exec clause runs (the clauses follow the CF); it is read pair
  // by pair, and every exec seen lowers the bound. Empty clauses (count 0, e.g. a final exec_end) do not count: their
  // address is never used.
  uint32_t bound = total_slots_;
  for (uint32_t i = 0; i < bound; ++i) {
    const uint32_t* w = code_.data() + size_t{i} * u::kSlotDwords;
    for (uint64_t raw : {u::cf_lo(w), u::cf_hi(w)}) {
      CfInstr c = decode_cf(raw);
      if (is_exec(c.op) && c.count > 0) bound = std::min(bound, c.address);
      p_.cf.push_back(c);
    }
    cf_slots_ = i + 1;
  }
  return {};
}

Result<void> Decoder::check_cf(uint32_t index) {
  const CfInstr& c = p_.cf[index];
  const auto cf_count = static_cast<uint32_t>(p_.cf.size());
  const auto outside = [&](uint32_t target) {
    return fail(index, cf_kind(c.op),
                "target " + std::to_string(target) + " is outside the program (" + std::to_string(cf_count) +
                    " control-flow instructions)");
  };
  switch (c.op) {
    case CfOp::LoopStart:
      // The skip target is the instruction after the matching loop_end: it may be one past the last one.
      if (c.target > cf_count) return outside(c.target);
      break;
    case CfOp::LoopEnd:
    case CfOp::CondCall:
    case CfOp::CondJmp:
      if (c.target >= cf_count) return outside(c.target);
      break;
    default:
      break;
  }
  if (c.op == CfOp::LoopStart || c.op == CfOp::LoopEnd) p_.loops_used.set(c.loop_id);
  if (uses_bool(c)) p_.bools_used.set(c.bool_index);
  if (c.op == CfOp::LoopStart || c.op == CfOp::LoopEnd || c.op == CfOp::CondCall || c.op == CfOp::CondJmp ||
      c.op == CfOp::Return)
    p_.has_jumps_calls_or_loops = true;
  return {};
}

Result<void> Decoder::decode_exec(uint32_t index) {
  const CfInstr& c = p_.cf[index];
  if (!is_exec(c.op) || c.count == 0) return {};
  const uint64_t end = uint64_t{c.address} + c.count;
  if (end > total_slots_)
    return fail(index, cf_kind(c.op),
                "runs slots " + std::to_string(c.address) + ".." + std::to_string(end - 1) +
                    ", past the end of the microcode (" + std::to_string(total_slots_) + " slots)");
  if (c.address < cf_slots_)
    return fail(index, cf_kind(c.op),
                "slot address " + std::to_string(c.address) + " overlaps the control flow (slots 0.." +
                    std::to_string(cf_slots_ - 1) + ")");
  if (p_.slots.size() < end) {
    p_.slots.resize(end);
    slot_kind_.resize(end, 0);
  }
  for (uint32_t k = 0; k < c.count; ++k) {
    const uint32_t slot = c.address + k;
    // 12 sequence bits cover 6 instructions; a 7th reads as ALU (the bits past the field are 0).
    const bool is_fetch = k < 6 && ((c.sequence >> (2 * k)) & 1) != 0;
    const uint8_t kind = is_fetch ? 2 : 1;
    if (slot_kind_[slot] == kind) continue;  // already decoded by an earlier clause
    if (slot_kind_[slot] != 0)
      return fail(slot, is_fetch ? "fetch" : "alu",
                  "cf " + std::to_string(index) + " runs the slot as " + (is_fetch ? "fetch" : "alu") +
                      ", an earlier clause as " + (is_fetch ? "alu" : "fetch"));
    const uint32_t* w = code_.data() + size_t{slot} * u::kSlotDwords;
    if (is_fetch) {
      LR_TRY_ASSIGN(f, decode_fetch(slot, w));
      p_.slots[slot] = f;
    } else {
      LR_TRY_ASSIGN(a, decode_alu(slot, w));
      p_.slots[slot] = a;
    }
    slot_kind_[slot] = kind;
  }
  return {};
}

Result<void> Decoder::note_export(uint32_t slot, uint32_t reg) {
  if (reg >= kExportAddress && reg < kExportData0 + kExportDataCount) {
    p_.uses_memexport = true;
    p_.memexport_written |= 1u << (reg - kExportAddress);
    return {};
  }
  if (p_.stage == ShaderStage::Vertex) {
    if (reg < kExportInterpolatorCount) {
      p_.interpolators_written |= 1u << reg;
      return {};
    }
    if (reg == kExportVsPosition) {
      p_.writes_position = true;
      return {};
    }
    if (reg == kExportVsPointSize) {
      p_.writes_point_size = true;
      return {};
    }
    return fail(slot, "alu", "export register " + std::to_string(reg) + " is not a vertex shader export");
  }
  if (reg < kExportColorCount) {
    p_.colors_written |= 1u << reg;
    return {};
  }
  if (reg == kExportPsDepth) {
    p_.writes_depth = true;
    return {};
  }
  return fail(slot, "alu", "export register " + std::to_string(reg) + " is not a pixel shader export");
}

Result<AluInstr> Decoder::decode_alu(uint32_t slot, const uint32_t* w) {
  namespace f = u::alu;
  const uint32_t vop = u::get32(w[2], f::kVectorOpcode);
  if (vop > static_cast<uint32_t>(VectorOp::MaxA))
    return fail(slot, "alu", "unknown vector opcode " + std::to_string(vop));
  const uint32_t sop = u::get32(w[0], f::kScalarOpcode);
  if (sop > static_cast<uint32_t>(ScalarOp::RetainPrev) || sop == static_cast<uint32_t>(ScalarOp::Invalid41))
    return fail(slot, "alu", "unknown scalar opcode " + std::to_string(sop));

  AluInstr a;
  a.vector_op = static_cast<VectorOp>(vop);
  a.scalar_op = static_cast<ScalarOp>(sop);
  a.vector_dest = u::get32(w[0], f::kVectorDest);
  a.vector_dest_rel = u::get32(w[0], f::kVectorDestRel) != 0;
  a.abs_constants = u::get32(w[0], f::kAbsConstants) != 0;
  a.scalar_dest = u::get32(w[0], f::kScalarDest);
  a.scalar_dest_rel = u::get32(w[0], f::kScalarDestRel) != 0;
  a.export_data = u::get32(w[0], f::kExportData) != 0;
  a.vector_write_mask = u::get32(w[0], f::kVectorWriteMask);
  a.scalar_write_mask = u::get32(w[0], f::kScalarWriteMask);
  a.vector_saturate = u::get32(w[0], f::kVectorSaturate) != 0;
  a.scalar_saturate = u::get32(w[0], f::kScalarSaturate) != 0;
  a.pred_condition = u::get32(w[1], f::kPredCondition) != 0;
  a.predicated = u::get32(w[1], f::kPredicated) != 0;
  a.a0_rel = u::get32(w[1], f::kA0Relative) != 0;
  a.const1_rel = u::get32(w[1], f::kConst1Rel) != 0;
  a.const0_rel = u::get32(w[1], f::kConst0Rel) != 0;

  const bool scalar_const = scalar_op_is_const(a.scalar_op);
  uint32_t constants_seen = 0;
  for (uint32_t i = 0; i < 3; ++i) {
    AluSource& s = a.src[i];
    const uint32_t raw = u::get32(w[2], f::kSrcReg[i]);
    s.swizzle = u::get32(w[1], f::kSwizzle[i]);
    s.negate = u::get32(w[1], f::kNegate[i]) != 0;
    // The *_CONST scalar ops reuse src3's select bit for their temporary's index: src3 is always the constant.
    s.is_temp = (i == 2 && scalar_const) ? false : u::get32(w[2], f::kSrcIsTemp[i]) != 0;
    if (s.is_temp) {
      s.reg = u::get32(raw, f::kTempIndex);
      a.is_src_temp_relative[i] = u::get32(raw, f::kTempRelative) != 0;
      s.abs = u::get32(raw, f::kTempAbs) != 0;
    } else {
      s.reg = raw;
      s.abs = a.abs_constants;
      // The first constant operand of the instruction uses const_0_rel, any later one const_1_rel (ADR 0005 M5).
      s.const_relative = constants_seen == 0 ? a.const0_rel : a.const1_rel;
      ++constants_seen;
    }
  }
  if (scalar_const) {
    const uint32_t sel3 = u::get32(w[2], f::kSrcIsTemp[2]);
    a.scalar_const_temp = (sop & 1) | (sel3 << 1) | (a.src[2].swizzle & 0x3C);
  }

  // Destinations.
  if (a.export_data) {
    if (a.vector_dest_rel) return fail(slot, "alu", "export to a relative destination");
    if ((a.vector_write_mask | a.scalar_write_mask) != 0) LR_TRY(note_export(slot, a.vector_dest));
  } else {
    if (a.vector_write_mask != 0) note_temp(a.vector_dest, a.vector_dest_rel);
    if (a.scalar_write_mask != 0) note_temp(a.scalar_dest, a.scalar_dest_rel);
  }
  // Sources the issued ops read.
  if (a.vector_write_mask != 0 || vector_op_has_side_effect(a.vector_op)) {
    for (uint32_t i = 0; i < vector_op_source_count(a.vector_op); ++i)
      if (a.src[i].is_temp) note_temp(a.src[i].reg, a.is_src_temp_relative[i]);
  }
  if (a.scalar_write_mask != 0 || scalar_op_has_side_effect(a.scalar_op)) {
    if (scalar_const)
      note_temp(a.scalar_const_temp, false);
    else if (scalar_op_operand_count(a.scalar_op) > 0 && a.src[2].is_temp)
      note_temp(a.src[2].reg, a.is_src_temp_relative[2]);
  }
  if (is_kill(a.vector_op) || is_kill(a.scalar_op)) p_.uses_kill = true;
  return a;
}

Result<FetchInstr> Decoder::decode_fetch(uint32_t slot, const uint32_t* w) {
  namespace f = u::fetch;
  FetchInstr x;
  const uint32_t op = u::get32(w[0], f::kOpcode);
  switch (op) {
    case 0: x.op = FetchOp::VertexFetch; break;
    case 1: x.op = FetchOp::TextureFetch; break;
    case 16: x.op = FetchOp::GetTextureBorderColorFrac; break;
    case 17: x.op = FetchOp::GetTextureComputedLod; break;
    case 18: x.op = FetchOp::GetTextureGradients; break;
    case 19: x.op = FetchOp::GetTextureWeights; break;
    case 24: x.op = FetchOp::SetTextureLod; break;
    case 25: x.op = FetchOp::SetTextureGradientsHorz; break;
    case 26: x.op = FetchOp::SetTextureGradientsVert; break;
    default: return fail(slot, "fetch", "unknown fetch opcode " + std::to_string(op));
  }
  x.src_reg = u::get32(w[0], f::kSrcReg);
  x.src_rel = u::get32(w[0], f::kSrcRel) != 0;
  x.dst_reg = u::get32(w[0], f::kDstReg);
  x.dst_rel = u::get32(w[0], f::kDstRel) != 0;
  x.dst_swizzle = u::get32(w[1], f::kDstSwizzle);
  x.predicated = u::get32(w[1], f::kPredicated) != 0;
  x.pred_condition = u::get32(w[2], f::kPredCondition) != 0;

  if (x.op == FetchOp::VertexFetch) {
    x.mini = u::get32(w[1], f::kVfMini) != 0;
    x.is_signed = u::get32(w[1], f::kVfSigned) != 0;
    x.normalized = u::get32(w[1], f::kVfNumFormat) == 0;
    x.signed_rf_mode = u::get32(w[1], f::kVfSignedRfMode) != 0;
    x.format = u::get32(w[1], f::kVfFormat);
    x.exp_adjust = u::sign_extend(u::get32(w[1], f::kVfExpAdjust), f::kVfExpAdjust.width);
    x.offset = u::sign_extend(u::get32(w[2], f::kVfOffset), f::kVfOffset.width);
    x.prefetch_count = u::get32(w[0], f::kVfPrefetchCount);
    if (x.mini) {
      // A mini fetch reads the same vertex as the last full fetch: source, constant and stride come from it.
      if (!have_full_vfetch_) return fail(slot, "fetch", "mini vfetch without a full vfetch before it");
      x.src_reg = last_full_vfetch_.src_reg;
      x.src_rel = last_full_vfetch_.src_rel;
      x.src_swizzle = last_full_vfetch_.src_swizzle;
      x.index_component = last_full_vfetch_.index_component;
      x.slot = last_full_vfetch_.slot;
      x.stride = last_full_vfetch_.stride;
      x.rounded = last_full_vfetch_.rounded;
    } else {
      x.slot = u::get32(w[0], f::kConstIndex) * 3 + u::get32(w[0], f::kVfConstSelect);
      if (x.slot >= 96)
        return fail(slot, "fetch", "vertex fetch constant " + std::to_string(x.slot) + " is out of range (0..95)");
      x.src_swizzle = x.index_component = u::get32(w[0], f::kVfSrcSwizzle);
      x.stride = u::get32(w[2], f::kVfStride);
      x.rounded = u::get32(w[1], f::kVfRounded) != 0;
      last_full_vfetch_ = x;
      have_full_vfetch_ = true;
    }
    p_.vfetch_slots.set(x.slot);
  } else {
    x.const_index = u::get32(w[0], f::kConstIndex);
    x.fetch_valid_only = u::get32(w[0], f::kTfFetchValidOnly) != 0;
    x.unnormalized = u::get32(w[0], f::kTfUnnormalized) != 0;
    x.src_swizzle = u::get32(w[0], f::kTfSrcSwizzle);
    x.mag_filter = u::get32(w[1], f::kTfMagFilter);
    x.min_filter = u::get32(w[1], f::kTfMinFilter);
    x.mip_filter = u::get32(w[1], f::kTfMipFilter);
    x.aniso_filter = u::get32(w[1], f::kTfAnisoFilter);
    x.vol_mag_filter = u::get32(w[1], f::kTfVolMagFilter);
    x.vol_min_filter = u::get32(w[1], f::kTfVolMinFilter);
    x.use_computed_lod = u::get32(w[1], f::kTfUseComputedLod) != 0;
    x.use_register_lod = u::get32(w[1], f::kTfUseRegisterLod) != 0;
    x.use_register_gradients = u::get32(w[2], f::kTfUseRegisterGradients) != 0;
    x.sample_location_center = u::get32(w[2], f::kTfSampleLocation) != 0;
    x.lod_bias = static_cast<float>(u::sign_extend(u::get32(w[2], f::kTfLodBias), f::kTfLodBias.width)) / 16.0f;
    x.dimension = u::get32(w[2], f::kTfDimension);
    x.offset_x = static_cast<float>(u::sign_extend(u::get32(w[2], f::kTfOffsetX), f::kTfOffsetX.width)) / 2.0f;
    x.offset_y = static_cast<float>(u::sign_extend(u::get32(w[2], f::kTfOffsetY), f::kTfOffsetY.width)) / 2.0f;
    x.offset_z = static_cast<float>(u::sign_extend(u::get32(w[2], f::kTfOffsetZ), f::kTfOffsetZ.width)) / 2.0f;
    if (x.op == FetchOp::TextureFetch || x.op == FetchOp::GetTextureBorderColorFrac ||
        x.op == FetchOp::GetTextureComputedLod || x.op == FetchOp::GetTextureWeights)
      p_.tfetch_slots.set(x.const_index);
  }
  note_temp(x.src_reg, x.src_rel);
  if (fetch_writes_dest(x)) note_temp(x.dst_reg, x.dst_rel);
  return x;
}

Result<Program> Decoder::run() {
  if (code_.empty()) return fail(0, "cf", "empty microcode");
  if (code_.size() < u::kSlotDwords)
    return fail(0, "cf",
                "microcode of " + std::to_string(code_.size()) + " dwords is shorter than one control-flow pair");
  // A trailing partial slot (fewer than 3 dwords) is never read.
  total_slots_ = static_cast<uint32_t>(std::min<size_t>(code_.size() / u::kSlotDwords, UINT32_MAX));
  LR_TRY(decode_cf_area());
  for (uint32_t i = 0; i < p_.cf.size(); ++i) LR_TRY(check_cf(i));
  for (uint32_t i = 0; i < p_.cf.size(); ++i) LR_TRY(decode_exec(i));
  p_.temp_count = p_.uses_relative_temps ? kMaxTemps : max_temp_;
  return std::move(p_);
}

}  // namespace

Result<Program> decode(ShaderStage stage, std::span<const uint32_t> microcode) {
  return Decoder(stage, microcode).run();
}

}  // namespace lr::gpu::shader
