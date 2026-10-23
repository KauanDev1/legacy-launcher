// SPDX-License-Identifier: MIT
// The translator's driver: module setup, the runtime contract (contract.h) and the register array (aL-relative temps
// included). Control flow is translator_cf.cpp, the ALU translator_alu.cpp. Xenos semantics per Xenia's shader
// translators (reference only, nothing copied).
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
      if (scalar_op_operand_count(a->scalar_op) > 0) {  // every scalar op but RETAIN_PREV runs (it sets ps)
        if (scalar_op_is_const(a->scalar_op))
          note(a->scalar_const_temp, false);
        else if (a->src[2].is_temp)
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

void Translator::scan_alu_state() {
  for_each_exec_slot(program_, [&](uint32_t, const std::variant<AluInstr, FetchInstr>& s) {
    const auto* a = std::get_if<AluInstr>(&s);
    if (!a) return;
    const bool vector_runs = vector_unit_runs(*a);
    const bool scalar_runs = scalar_unit_runs(*a);
    if ((vector_runs && a->vector_op == VectorOp::MaxA) ||
        (scalar_runs && (a->scalar_op == ScalarOp::MovA || a->scalar_op == ScalarOp::MovAFloor)))
      uses_a0_ = true;
    if (scalar_runs && scalar_op_reads_ps(a->scalar_op)) uses_ps_ = true;
    // Only the sources the running units read: src1..n for the vector op, src3 for a scalar op with operands.
    const auto note_constant = [&](uint32_t i) {
      if (!a->src[i].const_relative) return;
      if (a->a0_rel)
        uses_a0_ = true;
      else
        uses_al_constants_ = true;
    };
    if (vector_runs)
      for (uint32_t i = 0; i < vector_op_source_count(a->vector_op); ++i) note_constant(i);
    if (scalar_runs && scalar_op_operand_count(a->scalar_op) > 0) note_constant(2);
  });
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

void Translator::record_internal(std::string_view what) {
  if (!internal_error_) internal_error_ = internal(what);
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
  // Not a member offset: a translator bug. Recorded (run() returns it) and replaced by a placeholder so emission
  // can go on; the module is discarded.
  record_internal("load_shared: " + std::to_string(contract_offset) + " is not a shared-block member offset");
  return const_u(0);
}

Id Translator::load_shared_u32(uint32_t contract_offset, uint32_t index) {
  for (uint32_t m = 0; m < shared_members_.size(); ++m) {
    if (shared_members_[m].first != contract_offset) continue;
    const uint32_t length = contract_offset == kSharedBools ? 8 : 32;
    if (index >= length) break;
    const Id ptr = b_.op(spv::OpAccessChain, b_.type_pointer(spv::StorageClassPhysicalStorageBuffer, t_.u32),
                         {shared_, const_u(m), const_u(index)});
    return b_.op(spv::OpLoad, t_.u32, {ptr, spv::MemoryAccessAlignedMask, 4});
  }
  record_internal("load_shared_u32: " + std::to_string(contract_offset) + "[" + std::to_string(index) +
                  "] is not a shared-block array element");
  return const_u(0);
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
  store_reg_masked_at(const_u(index), value, mask);
}

void Translator::store_reg_masked_at(Id index, Id value, uint32_t mask) {
  const Id ptr = reg_ptr(index);
  if ((mask & 0xF) != 0xF) value = merge_masked(b_.op(spv::OpLoad, t_.vec4, {ptr}), value, mask);
  b_.op_void(spv::OpStore, {ptr, value});
}

Id Translator::temp_index(uint32_t reg, bool relative) {
  if (!relative) return const_u(reg);
  // r[aL + reg]: the register array has kMaxTemps entries whenever a temporary is relative, and the index is
  // clamped into it (planner's call; the hardware register file size is not keyed).
  const Id sum = b_.op(spv::OpIAdd, t_.i32, {current_al(), b_.constant_i32(static_cast<int32_t>(reg))});
  const Id clamped =
      glsl(t_.i32, GLSLstd450SClamp, {sum, b_.constant_i32(0), b_.constant_i32(static_cast<int32_t>(kMaxTemps - 1))});
  return b_.op(spv::OpBitcast, t_.u32, {clamped});
}

void Translator::kill_if(Id condition) {
  const Id k = b_.op(spv::OpLoad, t_.bool_, {killed_});
  b_.op_void(spv::OpStore, {killed_, b_.op(spv::OpLogicalOr, t_.bool_, {k, condition})});
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
  scan_alu_state();
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
  t_.uvec4 = b_.type_vector(t_.u32, 4);
  t_.ivec4 = b_.type_vector(t_.i32, 4);

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
  declare_alu_state();
  emit_stage_prologue();
  LR_TRY(emit_control_flow());
  emit_stage_epilogue();
  b_.end_main();
  if (internal_error_) return *internal_error_;

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
