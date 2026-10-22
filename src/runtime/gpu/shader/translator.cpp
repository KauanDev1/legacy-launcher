// SPDX-License-Identifier: MIT
// The translator's driver: module setup, the runtime contract (contract.h), the register array, and for now the
// straight-line control flow and the first vector ops (Task 15.4). Xenos semantics per Xenia's shader translators
// (reference only, nothing copied).
#include "runtime/gpu/shader/translator.h"

#include <spirv/unified1/GLSL.std.450.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "runtime/gpu/shader/disasm.h"
#include "runtime/gpu/shader/translator_internal.h"

namespace lr::gpu::shader {
namespace detail {

namespace {

// Calls fn(slot, instruction) for every slot some exec clause runs (a slot several clauses run comes once per clause).
template <class Fn>
void for_each_exec_slot(const Program& p, Fn&& fn) {
  for (const CfInstr& c : p.cf) {
    if (!is_exec(c.op)) continue;
    for (uint32_t k = 0; k < c.count; ++k) {
      const uint32_t slot = c.address + k;
      if (slot < p.slots.size()) fn(slot, p.slots[slot]);
    }
  }
}

// The slot of the first ALU export to eA/eM, or nothing.
std::optional<uint32_t> first_memexport(const Program& p) {
  std::optional<uint32_t> found;
  for_each_exec_slot(p, [&](uint32_t slot, const std::variant<AluInstr, FetchInstr>& s) {
    const auto* a = std::get_if<AluInstr>(&s);
    if (!a || !a->export_data) return;
    if (a->vector_dest < kExportAddress || a->vector_dest >= kExportData0 + kExportDataCount) return;
    if ((a->vector_write_mask | a->scalar_write_mask) == 0 && !a->scalar_dest_rel) return;
    if (!found || slot < *found) found = slot;
  });
  return found;
}

}  // namespace

TempUse referenced_temps(const Program& p) {
  TempUse use;
  const auto note = [&](uint32_t reg, bool relative) {
    if (relative)
      use.relative = true;
    else if (reg < kMaxTemps)
      use.absolute.set(reg);
  };
  for_each_exec_slot(p, [&](uint32_t, const std::variant<AluInstr, FetchInstr>& s) {
    if (const auto* a = std::get_if<AluInstr>(&s)) {
      if (!a->export_data) {
        if (a->vector_write_mask != 0) note(a->vector_dest, a->vector_dest_rel);
        if (a->scalar_write_mask != 0) note(a->scalar_dest, a->scalar_dest_rel);
      }
      if (a->vector_write_mask != 0 || vector_op_has_side_effect(a->vector_op))
        for (uint32_t i = 0; i < vector_op_source_count(a->vector_op); ++i)
          if (a->src[i].is_temp) note(a->src[i].reg, a->is_src_temp_relative[i]);
      if (a->scalar_write_mask != 0 || scalar_op_has_side_effect(a->scalar_op)) {
        if (scalar_op_is_const(a->scalar_op))
          note(a->scalar_const_temp, false);
        else if (scalar_op_operand_count(a->scalar_op) > 0 && a->src[2].is_temp)
          note(a->src[2].reg, a->is_src_temp_relative[2]);
      }
    } else {
      const auto& f = std::get<FetchInstr>(s);
      note(f.src_reg, f.src_rel);
      if (fetch_writes_dest(f)) note(f.dst_reg, f.dst_rel);
    }
  });
  return use;
}

Translator::Translator(const Program& program, const TranslateOptions& options)
    : program_(program),
      options_(options),
      b_(program.stage == ShaderStage::Vertex ? spv::ExecutionModelVertex : spv::ExecutionModelFragment) {}

Error Translator::unsupported(uint32_t index, std::string_view kind, std::string_view what) {
  return Error{ErrorCode::InvalidFormat,
               "instruction " + std::to_string(index) + " (" + std::string(kind) + "): " + std::string(what) +
                   " is not supported",
               "the shader translator cannot handle this microcode yet"};
}

Error Translator::internal(std::string_view what) {
  return Error{ErrorCode::Internal, "shader translator: " + std::string(what), ""};
}

// --- types, constants, globals ------------------------------------------------------------------------------------

Id Translator::const_vec4(float x, float y, float z, float w) {
  const Id c[] = {const_f(x), const_f(y), const_f(z), const_f(w)};
  return b_.constant_composite(t_.vec4, c);
}

Id Translator::global_var(spv::StorageClass storage, Id type, std::string_view name) {
  const Id v = b_.variable(storage, b_.type_pointer(storage, type), name);
  interface_.push_back(v);
  return v;
}

Id Translator::extract(Id composite, uint32_t component, Id type) {
  return b_.op(spv::OpCompositeExtract, type ? type : t_.f32, {composite, component});
}

// --- the contract -------------------------------------------------------------------------------------------------

void Translator::declare_contract() {
  b_.capability(spv::CapabilityPhysicalStorageBufferAddresses);
  b_.memory_model(spv::AddressingModelPhysicalStorageBuffer64, spv::MemoryModelGLSL450);

  // Push constants: four uvec2 device addresses.
  const Id pc_members[] = {t_.uvec2, t_.uvec2, t_.uvec2, t_.uvec2};
  const Id pc_struct = b_.type_struct(pc_members);
  b_.decorate(pc_struct, spv::DecorationBlock);
  const uint32_t pc_offsets[] = {kPushVsConstants, kPushPsConstants, kPushSharedBlock, kPushVertexFetchTable};
  for (uint32_t m = 0; m < 4; ++m) b_.member_decorate(pc_struct, m, spv::DecorationOffset, {pc_offsets[m]});
  push_block_ = global_var(spv::StorageClassPushConstant, pc_struct, "push");

  // Float constants: { vec4 c[256]; }.
  const Id c_array = b_.type_array(t_.vec4, const_u(kFloatConstantCount));
  b_.decorate(c_array, spv::DecorationArrayStride, {16});
  const Id c_members[] = {c_array};
  const Id c_struct = b_.type_struct(c_members);
  b_.decorate(c_struct, spv::DecorationBlock);
  b_.member_decorate(c_struct, 0, spv::DecorationOffset, {0});
  consts_ptr_type_ = b_.type_pointer(spv::StorageClassPhysicalStorageBuffer, c_struct);

  // The shared block, member by member in offset order (the reserved tail is not declared).
  const Id u32_32 = b_.type_array(t_.u32, const_u(32));
  b_.decorate(u32_32, spv::DecorationArrayStride, {4});
  const Id u32_8 = b_.type_array(t_.u32, const_u(8));
  b_.decorate(u32_8, spv::DecorationArrayStride, {4});
  shared_members_ = {
      {kSharedTexture2DIndex, u32_32}, {kSharedTexture3DIndex, u32_32}, {kSharedTextureCubeIndex, u32_32},
      {kSharedSamplerIndex, u32_32},   {kSharedBools, u32_8},           {kSharedLoops, u32_32},
      {kSharedNdcScale, t_.vec4},      {kSharedNdcOffset, t_.vec4},     {kSharedAlphaRef, t_.f32},
      {kSharedVteFlags, t_.u32},       {kSharedIndexOffset, t_.i32},    {kSharedMinIndex, t_.u32},
      {kSharedMaxIndex, t_.u32},
  };
  std::vector<Id> s_members;
  for (const auto& [offset, type] : shared_members_) s_members.push_back(type);
  const Id s_struct = b_.type_struct(s_members);
  b_.decorate(s_struct, spv::DecorationBlock);
  for (uint32_t m = 0; m < shared_members_.size(); ++m)
    b_.member_decorate(s_struct, m, spv::DecorationOffset, {shared_members_[m].first});
  shared_ptr_type_ = b_.type_pointer(spv::StorageClassPhysicalStorageBuffer, s_struct);
}

void Translator::load_contract_pointers() {
  const Id pc_ptr = b_.type_pointer(spv::StorageClassPushConstant, t_.uvec2);
  const auto address = [&](uint32_t member) {
    return b_.op(spv::OpLoad, t_.uvec2, {b_.op(spv::OpAccessChain, pc_ptr, {push_block_, const_u(member)})});
  };
  consts_ = b_.op(spv::OpBitcast, consts_ptr_type_, {address(is_vs() ? 0 : 1)});
  shared_ = b_.op(spv::OpBitcast, shared_ptr_type_, {address(2)});
}

Id Translator::load_float_constant(Id index) {
  const Id ptr = b_.op(spv::OpAccessChain, b_.type_pointer(spv::StorageClassPhysicalStorageBuffer, t_.vec4),
                       {consts_, const_u(0), index});
  return b_.op(spv::OpLoad, t_.vec4, {ptr, spv::MemoryAccessAlignedMask, 16});
}

Id Translator::load_shared(uint32_t contract_offset) {
  for (uint32_t m = 0; m < shared_members_.size(); ++m) {
    if (shared_members_[m].first != contract_offset) continue;
    const Id type = shared_members_[m].second;
    const Id ptr =
        b_.op(spv::OpAccessChain, b_.type_pointer(spv::StorageClassPhysicalStorageBuffer, type), {shared_, const_u(m)});
    return b_.op(spv::OpLoad, type, {ptr, spv::MemoryAccessAlignedMask, type == t_.vec4 ? 16u : 4u});
  }
  return 0;  // not a member offset: a translator bug, caught by the caller's use (never reached by design)
}

// --- registers ----------------------------------------------------------------------------------------------------

Id Translator::reg_ptr(Id index) {
  return b_.op(spv::OpAccessChain, b_.type_pointer(spv::StorageClassFunction, t_.vec4), {regs_, index});
}

Id Translator::merge_masked(Id old_value, Id value, uint32_t mask) {
  mask &= 0xF;
  if (mask == 0xF) return value;
  if (mask == 0) return old_value;
  uint32_t c[4];
  for (uint32_t i = 0; i < 4; ++i) c[i] = (mask >> i) & 1 ? 4 + i : i;
  return b_.op(spv::OpVectorShuffle, t_.vec4, {old_value, value, c[0], c[1], c[2], c[3]});
}

void Translator::store_reg_masked(uint32_t index, Id value, uint32_t mask) {
  const Id ptr = reg_ptr(const_u(index));
  if ((mask & 0xF) != 0xF) value = merge_masked(b_.op(spv::OpLoad, t_.vec4, {ptr}), value, mask);
  b_.op_void(spv::OpStore, {ptr, value});
}

void Translator::kill_if(Id condition) {
  const Id k = b_.op(spv::OpLoad, t_.bool_, {killed_});
  b_.op_void(spv::OpStore, {killed_, b_.op(spv::OpLogicalOr, t_.bool_, {k, condition})});
}

// --- control flow: straight-line exec clauses (Task 15.5 replaces this) --------------------------------------------

Result<void> Translator::emit_control_flow() {
  for (uint32_t i = 0; i < program_.cf.size(); ++i) {
    const CfInstr& c = program_.cf[i];
    switch (c.op) {
      case CfOp::Nop:
      case CfOp::Alloc:
      case CfOp::MarkVsFetchDone:
        break;
      case CfOp::Exec:
      case CfOp::ExecEnd:
        LR_TRY(emit_exec_clause(i, c));
        if (c.op == CfOp::ExecEnd) return {};
        break;
      default:
        return unsupported(i, "cf " + std::string(cf_op_name(c.op)), cf_op_name(c.op));
    }
  }
  return {};
}

Result<void> Translator::emit_exec_clause(uint32_t, const CfInstr& c) {
  for (uint32_t k = 0; k < c.count; ++k) {
    const uint32_t slot = c.address + k;
    if (slot >= program_.slots.size()) return internal("exec clause slot past the decoded slots");
    const auto& s = program_.slots[slot];
    if (const auto* a = std::get_if<AluInstr>(&s))
      LR_TRY(emit_alu(slot, *a));
    else
      LR_TRY(emit_fetch(slot, std::get<FetchInstr>(s)));
  }
  return {};
}

// --- ALU: MAX (mov), ADD, MUL; no scalar unit yet (Tasks 15.6/15.7) ------------------------------------------------

Result<Id> Translator::load_source(uint32_t slot, const AluInstr& a, uint32_t i) {
  const AluSource& s = a.src[i];
  Id v = 0;
  if (s.is_temp) {
    if (a.is_src_temp_relative[i]) return unsupported(slot, "alu", "an aL-relative temporary source");
    if (s.reg >= reg_count_) return internal("source register past the register array");
    v = load_reg(s.reg);
  } else {
    if (s.const_relative) return unsupported(slot, "alu", "relative constant addressing");
    v = load_float_constant(const_u(s.reg));
  }
  uint32_t c[4];
  bool identity = true;
  for (uint32_t k = 0; k < 4; ++k) {
    c[k] = ((s.swizzle >> (2 * k)) + k) & 3;
    identity = identity && c[k] == k;
  }
  if (!identity) v = b_.op(spv::OpVectorShuffle, t_.vec4, {v, v, c[0], c[1], c[2], c[3]});
  if (s.abs) v = glsl(t_.vec4, GLSLstd450FAbs, {v});
  if (s.negate) v = b_.op(spv::OpFNegate, t_.vec4, {v});
  return v;
}

Result<Id> Translator::emit_vector_op(uint32_t slot, const AluInstr& a) {
  Id src[3] = {};
  const uint32_t n = vector_op_source_count(a.vector_op);
  const auto need = [&]() -> Result<void> {
    for (uint32_t i = 0; i < n; ++i) {
      LR_TRY_ASSIGN(v, load_source(slot, a, i));
      src[i] = v;
    }
    return {};
  };
  Id r = 0;
  switch (a.vector_op) {
    case VectorOp::Add:
      LR_TRY(need());
      r = b_.op(spv::OpFAdd, t_.vec4, {src[0], src[1]});
      break;
    case VectorOp::Mul:
      // TODO(15.6): the Xenos 0 x anything = +0 rule.
      LR_TRY(need());
      r = b_.op(spv::OpFMul, t_.vec4, {src[0], src[1]});
      break;
    case VectorOp::Max:  // a >= b ? a : b; with src1 == src2 it is the mov idiom
      LR_TRY(need());
      r = b_.op(spv::OpSelect, t_.vec4,
                {b_.op(spv::OpFOrdGreaterThanEqual, t_.bvec4, {src[0], src[1]}), src[0], src[1]});
      break;
    default:
      return unsupported(slot, "alu", "vector op " + std::string(vector_op_name(a.vector_op)));
  }
  if (a.vector_saturate) r = glsl(t_.vec4, GLSLstd450NClamp, {r, const_vec4(0, 0, 0, 0), const_vec4(1, 1, 1, 1)});
  return r;
}

Result<void> Translator::emit_alu(uint32_t slot, const AluInstr& a) {
  if (a.predicated) return unsupported(slot, "alu", "predicated execution");
  const uint32_t vector_mask = a.export_data ? a.export_vector_mask : a.vector_write_mask;
  const uint32_t scalar_mask = a.export_data ? a.export_scalar_mask : a.scalar_write_mask;
  if (scalar_mask != 0 || scalar_op_has_side_effect(a.scalar_op))
    return unsupported(slot, "alu", "scalar op " + std::string(scalar_op_name(a.scalar_op)));
  Id vector_result = 0;
  if (vector_mask != 0 || vector_op_has_side_effect(a.vector_op)) {
    LR_TRY_ASSIGN(v, emit_vector_op(slot, a));
    vector_result = v;
  }
  if (a.export_data) return emit_export(slot, a, vector_result, 0);
  if (a.vector_write_mask != 0) {
    if (a.vector_dest_rel) return unsupported(slot, "alu", "an aL-relative destination");
    if (a.vector_dest >= reg_count_) return internal("destination register past the register array");
    store_reg_masked(a.vector_dest, vector_result, a.vector_write_mask);
  }
  return {};
}

// --- fetch (Tasks 15.8/15.9) --------------------------------------------------------------------------------------

Result<void> Translator::emit_fetch(uint32_t slot, const FetchInstr& f) {
  return unsupported(slot, "fetch", fetch_op_name(f.op));
}

// --- the module ---------------------------------------------------------------------------------------------------

Result<TranslatedShader> Translator::run() {
  if (auto slot = first_memexport(program_)) return unsupported(*slot, "alu", "memexport (eA/eM)");
  if (program_.uses_memexport) return unsupported(0, "alu", "memexport (eA/eM)");

  temps_ = referenced_temps(program_);
  reg_count_ = std::max<uint32_t>(program_.temp_count, 1);
  if (temps_.relative) reg_count_ = kMaxTemps;

  t_.void_ = b_.type_void();
  t_.bool_ = b_.type_bool();
  t_.f32 = b_.type_float(32);
  t_.u32 = b_.type_int(32, false);
  t_.i32 = b_.type_int(32, true);
  t_.vec2 = b_.type_vector(t_.f32, 2);
  t_.vec4 = b_.type_vector(t_.f32, 4);
  t_.bvec4 = b_.type_vector(t_.bool_, 4);
  t_.uvec2 = b_.type_vector(t_.u32, 2);

  declare_contract();
  declare_stage_interface();
  reg_array_type_ = b_.type_array(t_.vec4, const_u(reg_count_));
  regs_ = b_.variable(spv::StorageClassFunction, b_.type_pointer(spv::StorageClassFunction, reg_array_type_), "r");

  b_.begin_main();
  load_contract_pointers();
  {
    const std::vector<Id> zeros(reg_count_, const_vec4(0, 0, 0, 0));
    b_.op_void(spv::OpStore, {regs_, b_.constant_composite(reg_array_type_, zeros)});
  }
  emit_stage_prologue();
  LR_TRY(emit_control_flow());
  emit_stage_epilogue();
  b_.end_main();

  b_.entry_point("main", interface_);
  if (!is_vs()) {
    b_.execution_mode(spv::ExecutionModeOriginUpperLeft);
    if (depth_out_) b_.execution_mode(spv::ExecutionModeDepthReplacing);
  }
  LR_TRY_ASSIGN(spirv, b_.try_finish());

  info_.vfetch_slots = program_.vfetch_slots;
  info_.tfetch_slots = program_.tfetch_slots;
  for_each_exec_slot(program_, [&](uint32_t, const std::variant<AluInstr, FetchInstr>& s) {
    const auto* f = std::get_if<FetchInstr>(&s);
    if (f && f->op != FetchOp::VertexFetch && f->const_index < 32)
      info_.tfetch_dims[f->const_index] = static_cast<uint8_t>(f->dimension);
  });
  if (!is_vs()) {
    info_.colors_written = program_.colors_written;
    info_.writes_depth = program_.writes_depth;
    info_.uses_kill = program_.uses_kill;
  } else {
    info_.drops_point_size = program_.writes_point_size;
  }
  return TranslatedShader{std::move(spirv), info_};
}

}  // namespace detail

Result<TranslatedShader> translate(const Program& program, const TranslateOptions& options) {
  return detail::Translator(program, options).run();
}

}  // namespace lr::gpu::shader
