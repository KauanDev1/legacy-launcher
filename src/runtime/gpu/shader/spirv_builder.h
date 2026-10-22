// SPDX-License-Identifier: MIT
// A minimal SPIR-V module builder for the plan B translator (ADR 0005 M8): our own code over the SPIRV-Headers enums,
// no glslang and no compiler. It writes a SPIR-V 1.5 module (the version Vulkan 1.2 guarantees) with one entry point
// whose function is `main`, the sections in the order the specification requires. CPU-only, no Vulkan API types.
//
// Ids and words. Every value is an `Id`. `op`, `op_void` and `ext_inst` take their operands as raw words, so literal
// operands (OpCompositeExtract indices, OpVectorShuffle components, memory-access and image-operand masks, ...) go in
// the same list as ids.
//
// Deduplication. Scalar, vector, pointer, function, image, sampler and sampled-image types and every non-spec constant
// are deduplicated: asking twice gives the same id (SPIR-V forbids two identical non-aggregate types). Structs and
// arrays are not: each call makes a new type, because their layout decorations (Block, Offset, ArrayStride) are per
// type and two identical declarations may need different ones. Spec constants are never merged (each has its SpecId).
//
// Interface. From SPIR-V 1.4 on, the entry point's interface lists every global variable the entry point uses
// (Input, Output, PushConstant, Private, UniformConstant, StorageBuffer, ...), not just Input/Output. The builder
// records a global as used when it is a pointer operand of an instruction in `main` (operand 0 of any instruction,
// both operands of OpCopyMemory*, every operand of an OpExtInst); that covers every use without variable pointers.
// `check_interface()` reports a used global missing from `entry_point`'s list, and `finish()` asserts it.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spirv/unified1/spirv.hpp>

#include "common/result.h"

namespace lr::gpu::shader {

using Id = uint32_t;

class SpirvBuilder {
 public:
  // Adds Capability Shader. The memory model defaults to Logical GLSL450.
  explicit SpirvBuilder(spv::ExecutionModel model);

  void capability(spv::Capability cap);  // duplicates are ignored
  void extension(std::string_view name);
  void memory_model(spv::AddressingModel addressing, spv::MemoryModel memory);

  // --- types (see "Deduplication" above) ---
  Id type_void();
  Id type_bool();
  Id type_int(uint32_t width, bool is_signed);
  Id type_float(uint32_t width);
  Id type_vector(Id component, uint32_t n);
  Id type_array(Id element, Id length_const);  // never deduplicated
  Id type_runtime_array(Id element);           // never deduplicated
  Id type_struct(std::span<const Id> members);  // never deduplicated
  Id type_pointer(spv::StorageClass storage, Id pointee);
  Id type_function(Id ret);  // no parameters
  // `sampled`: 1 = used with a sampler, 2 = storage image (0 = unknown, not valid for Vulkan).
  Id type_image(Id sampled_type, spv::Dim dim, bool depth, bool arrayed, bool multisampled, uint32_t sampled,
                spv::ImageFormat format = spv::ImageFormatUnknown);
  Id type_sampler();
  Id type_sampled_image(Id image_type);

  // --- constants ---
  Id constant_u32(uint32_t value);
  Id constant_i32(int32_t value);
  Id constant_f32(float value);  // keyed by bit pattern: +0 and -0 are distinct
  Id constant_bool(bool value);
  Id constant_composite(Id type, std::span<const Id> constituents);
  // OpSpecConstant of type uint, decorated SpecId spec_id. Never deduplicated.
  Id spec_constant_u32(uint32_t spec_id, uint32_t default_value);

  // A global variable, or (storage == Function) a variable of `main`, emitted at the start of its first block.
  // `pointer_type` is a type_pointer() of the same storage class. `debug_name` adds an OpName.
  Id variable(spv::StorageClass storage, Id pointer_type, std::string_view debug_name = {});

  // --- annotations ---
  void decorate(Id target, spv::Decoration decoration, std::initializer_list<uint32_t> literals = {});
  void member_decorate(Id struct_type, uint32_t member, spv::Decoration decoration,
                       std::initializer_list<uint32_t> literals = {});

  // --- instructions in the current block of main ---
  Id op(spv::Op opcode, Id result_type, std::initializer_list<Id> operands);  // value-producing instruction
  void op_void(spv::Op opcode, std::initializer_list<Id> operands);         // OpStore, OpKill, OpReturn, ...
  // OpExtInst from GLSL.std.450 (imported on first use); glsl_std_450_op is a GLSLstd450 enumerant.
  Id ext_inst(Id result_type, uint32_t glsl_std_450_op, std::initializer_list<Id> operands);

  // --- structured control flow ---
  Id new_block();         // allocates a label id; set_block() starts it
  void set_block(Id label);  // emits OpLabel; the previous block must be terminated
  void branch(Id target);
  void branch_conditional(Id cond, Id true_label, Id false_label);
  void selection_merge(Id merge);  // goes right before branch_conditional/switch_
  void loop_merge(Id merge, Id continue_target);  // goes right before the header's branch
  void switch_(Id selector, Id default_block, std::span<const std::pair<uint32_t, Id>> cases);  // 32-bit selector

  // --- the entry point ---
  void begin_main();  // OpFunction void main() and its first block
  void end_main();    // OpReturn if the current block is open, then OpFunctionEnd
  void entry_point(std::string_view name, std::span<const Id> interface);
  void execution_mode(spv::ExecutionMode mode, std::initializer_list<uint32_t> literals = {});

  // Empty when every global main uses is in the entry point's interface; otherwise an InvalidFormat error naming
  // the first missing variable (by its OpName when it has one).
  Result<void> check_interface() const;

  // The module: SPIR-V 1.5 header, then the sections in specification order. Requires begin_main/end_main and
  // entry_point. Asserts check_interface(); in a build without assertions a failing check logs an error and
  // returns an empty vector.
  std::vector<uint32_t> finish();

 private:
  Id next_id() { return bound_++; }
  Id dedup_type(std::vector<uint32_t> key);      // key = {opcode, operands...}; emits into globals_
  Id dedup_constant(std::vector<uint32_t> key);  // key = {opcode, type, operands...}
  void emit_global(spv::Op opcode, std::initializer_list<uint32_t> words);
  void emit_body(spv::Op opcode, std::span<const uint32_t> words);
  void note_use(Id id);
  bool is_terminator(spv::Op opcode) const;

  spv::ExecutionModel model_;
  Id bound_ = 1;

  std::vector<uint32_t> capabilities_;
  std::vector<uint32_t> extensions_;
  std::vector<uint32_t> ext_imports_;
  spv::AddressingModel addressing_ = spv::AddressingModelLogical;
  spv::MemoryModel memory_ = spv::MemoryModelGLSL450;
  std::vector<uint32_t> debug_;
  std::vector<uint32_t> annotations_;
  std::vector<uint32_t> globals_;  // types, constants and global variables, in creation order
  std::vector<uint32_t> function_vars_;
  std::vector<uint32_t> body_;  // main's instructions after the first OpLabel

  std::map<std::vector<uint32_t>, Id> type_ids_;
  std::map<std::vector<uint32_t>, Id> constant_ids_;
  std::set<uint32_t> capability_set_;
  std::set<Id> global_vars_;
  std::set<Id> used_globals_;
  std::map<Id, std::string> names_;

  Id glsl_std_450_ = 0;
  Id main_ = 0;
  Id main_label_ = 0;
  bool main_begun_ = false;
  bool main_ended_ = false;
  bool in_block_ = false;
  bool has_entry_point_ = false;
  std::string entry_name_;
  std::vector<Id> interface_;
  std::vector<std::pair<spv::ExecutionMode, std::vector<uint32_t>>> modes_;
};

}  // namespace lr::gpu::shader
