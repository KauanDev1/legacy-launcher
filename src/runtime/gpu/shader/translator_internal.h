// SPDX-License-Identifier: MIT
// Internal to the translator (translator*.cpp): the Translator class every part of the translation adds to. Not
// staged API; nothing outside src/runtime/gpu/shader/translator*.cpp includes it.
//
// How a module is built (Translator::run, translator.cpp):
//   1. declare: types, the contract globals (push constants, buffer structs), the stage interface
//      (translator_export.cpp), the register array;
//   2. main's entry block: the buffer pointers (load_contract_pointers), the register array and export variables set
//      to zero, the stage prologue (VS r0.x = vertex index; PS r0..r15 from the interpolators, param_gen);
//   3. emit_control_flow(): the CF program, which runs exec clauses through emit_alu()/emit_fetch();
//   4. the stage epilogue (translator_export.cpp): exports to outputs, position transform, alpha test, final kill.
// Values that must dominate every later block (buffer pointers) are made in step 2, so the CF of Task 15.5 can put
// the clauses in any blocks.
//
// Where later tasks plug in: emit_control_flow/emit_exec_clause (15.5, translator_cf.cpp), emit_vector_op and the
// scalar unit in emit_alu (15.6/15.7, translator_alu.cpp), emit_fetch (15.8/15.9, translator_fetch.cpp). They report
// what they cannot translate with unsupported().
#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/result.h"
#include "runtime/gpu/shader/contract.h"
#include "runtime/gpu/shader/ir.h"
#include "runtime/gpu/shader/spirv_builder.h"
#include "runtime/gpu/shader/translator.h"

namespace lr::gpu::shader::detail {

// Temporaries an instruction reads or writes, as the decoder counts them (only the sources the issued ops read).
struct TempUse {
  std::bitset<kMaxTemps> absolute;
  bool relative = false;
};
TempUse referenced_temps(const Program& program);

class Translator {
 public:
  Translator(const Program& program, const TranslateOptions& options);
  Result<TranslatedShader> run();

  // --- errors ---
  // "instruction <index> (<kind>): <what> is not supported"
  static Error unsupported(uint32_t index, std::string_view kind, std::string_view what);
  static Error internal(std::string_view what);
  // For helpers that return an Id: keeps the first internal error, which run() returns once emission ends.
  void record_internal(std::string_view what);

  bool is_vs() const { return program_.stage == ShaderStage::Vertex; }

  // --- types, constants, globals (translator.cpp) ---
  struct Types {
    Id void_ = 0, bool_ = 0, f32 = 0, u32 = 0, i32 = 0, vec2 = 0, vec4 = 0, bvec4 = 0, uvec2 = 0;
  };
  Id const_f(float v) { return b_.constant_f32(v); }
  Id const_u(uint32_t v) { return b_.constant_u32(v); }
  Id const_vec4(float x, float y, float z, float w);
  // A global variable, added to the entry point's interface.
  Id global_var(spv::StorageClass storage, Id type, std::string_view name);
  Id glsl(Id result_type, uint32_t glsl_std_450_op, std::initializer_list<Id> operands) {
    return b_.ext_inst(result_type, glsl_std_450_op, operands);
  }
  Id extract(Id composite, uint32_t component, Id type = 0);

  // --- the contract (translator.cpp) ---
  void declare_contract();
  void load_contract_pointers();             // main's entry block
  Id load_float_constant(Id index);          // this stage's c<index> (index: a u32 value)
  Id load_shared(uint32_t contract_offset);  // a scalar or float4 member of the shared block, by its offset

  // --- registers (translator.cpp) ---
  Id reg_ptr(Id index);  // index: a u32 value < reg_count_
  Id load_reg(uint32_t index) { return b_.op(spv::OpLoad, t_.vec4, {reg_ptr(const_u(index))}); }
  // `value` where bit i of `mask` is set, `old_value` elsewhere.
  Id merge_masked(Id old_value, Id value, uint32_t mask);
  void store_reg_masked(uint32_t index, Id value, uint32_t mask);

  // --- kill (PS) ---
  // ORs `condition` into the killed flag; main ends with OpKill when it is set (never a mid-shader OpKill, so later
  // fetches keep their derivatives).
  void kill_if(Id condition);

  // --- control flow (translator.cpp; Task 15.5 replaces it in translator_cf.cpp) ---
  Result<void> emit_control_flow();
  Result<void> emit_exec_clause(uint32_t cf_index, const CfInstr& c);

  // --- ALU (translator.cpp; Tasks 15.6/15.7 move it to translator_alu.cpp) ---
  Result<void> emit_alu(uint32_t slot, const AluInstr& a);
  Result<Id> emit_vector_op(uint32_t slot, const AluInstr& a);
  Result<Id> load_source(uint32_t slot, const AluInstr& a, uint32_t i);

  // --- fetch (Tasks 15.8/15.9) ---
  Result<void> emit_fetch(uint32_t slot, const FetchInstr& f);

  // --- stage interface and exports (translator_export.cpp) ---
  void declare_stage_interface();
  void emit_stage_prologue();  // after the registers are zeroed
  // An ALU export: `vector_result` / `scalar_result` are 0 when the instruction does not issue them.
  Result<void> emit_export(uint32_t slot, const AluInstr& a, Id vector_result, Id scalar_result);
  void emit_stage_epilogue();

  // --- state ---
  const Program& program_;
  const TranslateOptions& options_;
  SpirvBuilder b_;
  Types t_;
  ShaderInfo info_;
  std::vector<Id> interface_;
  TempUse temps_;

  uint32_t reg_count_ = 1;  // max(temp_count, 1)
  Id regs_ = 0;             // Function vec4[reg_count_]
  Id reg_array_type_ = 0;

  // contract
  Id push_block_ = 0;      // PushConstant variable
  Id consts_ptr_type_ = 0;  // PhysicalStorageBuffer pointer to { vec4[256] }
  Id shared_ptr_type_ = 0;  // PhysicalStorageBuffer pointer to the shared block struct
  Id consts_ = 0;           // this stage's float constants (pointer value, entry block)
  Id shared_ = 0;           // the shared block (pointer value, entry block)
  std::vector<std::pair<uint32_t, Id>> shared_members_;  // (contract offset, member type) in member order

  // exports: Function vec4 variables, by export register (0 = the register is never exported)
  std::array<Id, 64> export_vars_{};
  // stage interface
  std::array<Id, kInterpolatorLocations> interpolators_{};  // VS outputs / PS inputs (0 = not declared)
  std::array<Id, kExportColorCount> color_outputs_{};
  Id position_out_ = 0, point_size_out_ = 0, vertex_index_in_ = 0;
  Id depth_out_ = 0, frag_coord_in_ = 0, front_facing_in_ = 0, point_coord_in_ = 0;
  Id killed_ = 0;      // PS: Function bool, 0 when nothing can kill
  Id spec_alpha_ = 0;  // PS: spec constant 0
  std::optional<Error> internal_error_;  // see record_internal()
};

}  // namespace lr::gpu::shader::detail
