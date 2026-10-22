// SPDX-License-Identifier: MIT
#include "runtime/gpu/shader/spirv_builder.h"

#include <bit>
#include <cassert>

#include "common/log.h"

namespace lr::gpu::shader {
namespace {

constexpr uint32_t kMagic = 0x07230203;
constexpr uint32_t kVersion15 = 0x00010500;
constexpr uint32_t kGenerator = 0;  // no registered generator id

uint32_t first_word(spv::Op opcode, size_t word_count) {
  assert(word_count <= 0xFFFF);
  return static_cast<uint32_t>(word_count) << 16 | static_cast<uint32_t>(opcode);
}

// A literal string: UTF-8 bytes, NUL-terminated, padded with NULs to a whole word, packed little-end first.
void append_string(std::vector<uint32_t>& out, std::string_view s) {
  const size_t words = s.size() / 4 + 1;
  const size_t base = out.size();
  out.resize(base + words, 0);
  for (size_t i = 0; i < s.size(); ++i)
    out[base + i / 4] |= static_cast<uint32_t>(static_cast<unsigned char>(s[i])) << (8 * (i % 4));
}

void append_inst(std::vector<uint32_t>& out, spv::Op opcode, std::span<const uint32_t> words) {
  out.push_back(first_word(opcode, words.size() + 1));
  out.insert(out.end(), words.begin(), words.end());
}

}  // namespace

SpirvBuilder::SpirvBuilder(spv::ExecutionModel model) : model_(model) { capability(spv::CapabilityShader); }

void SpirvBuilder::capability(spv::Capability cap) {
  if (!capability_set_.insert(static_cast<uint32_t>(cap)).second) return;
  const uint32_t w = cap;
  append_inst(capabilities_, spv::OpCapability, std::span(&w, 1));
}

void SpirvBuilder::extension(std::string_view name) {
  std::vector<uint32_t> words;
  append_string(words, name);
  append_inst(extensions_, spv::OpExtension, words);
}

void SpirvBuilder::memory_model(spv::AddressingModel addressing, spv::MemoryModel memory) {
  addressing_ = addressing;
  memory_ = memory;
}

void SpirvBuilder::emit_global(spv::Op opcode, std::initializer_list<uint32_t> words) {
  append_inst(globals_, opcode, std::span(words.begin(), words.size()));
}

Id SpirvBuilder::dedup_type(std::vector<uint32_t> key) {
  if (auto it = type_ids_.find(key); it != type_ids_.end()) return it->second;
  const Id id = next_id();
  std::vector<uint32_t> words;
  words.push_back(id);
  words.insert(words.end(), key.begin() + 1, key.end());
  append_inst(globals_, static_cast<spv::Op>(key[0]), words);
  type_ids_.emplace(std::move(key), id);
  return id;
}

Id SpirvBuilder::dedup_constant(std::vector<uint32_t> key) {
  if (auto it = constant_ids_.find(key); it != constant_ids_.end()) return it->second;
  const Id id = next_id();
  std::vector<uint32_t> words;
  words.push_back(key[1]);  // result type
  words.push_back(id);
  words.insert(words.end(), key.begin() + 2, key.end());
  append_inst(globals_, static_cast<spv::Op>(key[0]), words);
  constant_ids_.emplace(std::move(key), id);
  return id;
}

// --- types ---

Id SpirvBuilder::type_void() { return dedup_type({spv::OpTypeVoid}); }
Id SpirvBuilder::type_bool() { return dedup_type({spv::OpTypeBool}); }
Id SpirvBuilder::type_int(uint32_t width, bool is_signed) {
  return dedup_type({spv::OpTypeInt, width, is_signed ? 1u : 0u});
}
Id SpirvBuilder::type_float(uint32_t width) { return dedup_type({spv::OpTypeFloat, width}); }
Id SpirvBuilder::type_vector(Id component, uint32_t n) { return dedup_type({spv::OpTypeVector, component, n}); }

Id SpirvBuilder::type_array(Id element, Id length_const) {
  const Id id = next_id();
  emit_global(spv::OpTypeArray, {id, element, length_const});
  return id;
}

Id SpirvBuilder::type_runtime_array(Id element) {
  const Id id = next_id();
  emit_global(spv::OpTypeRuntimeArray, {id, element});
  return id;
}

Id SpirvBuilder::type_struct(std::span<const Id> members) {
  const Id id = next_id();
  std::vector<uint32_t> words{id};
  words.insert(words.end(), members.begin(), members.end());
  append_inst(globals_, spv::OpTypeStruct, words);
  return id;
}

Id SpirvBuilder::type_pointer(spv::StorageClass storage, Id pointee) {
  return dedup_type({spv::OpTypePointer, static_cast<uint32_t>(storage), pointee});
}

Id SpirvBuilder::type_function(Id ret) { return dedup_type({spv::OpTypeFunction, ret}); }

Id SpirvBuilder::type_image(Id sampled_type, spv::Dim dim, bool depth, bool arrayed, bool multisampled,
                            uint32_t sampled, spv::ImageFormat format) {
  return dedup_type({spv::OpTypeImage, sampled_type, static_cast<uint32_t>(dim), depth ? 1u : 0u, arrayed ? 1u : 0u,
                     multisampled ? 1u : 0u, sampled, static_cast<uint32_t>(format)});
}

Id SpirvBuilder::type_sampler() { return dedup_type({spv::OpTypeSampler}); }
Id SpirvBuilder::type_sampled_image(Id image_type) { return dedup_type({spv::OpTypeSampledImage, image_type}); }

// --- constants ---

Id SpirvBuilder::constant_u32(uint32_t value) { return dedup_constant({spv::OpConstant, type_int(32, false), value}); }
Id SpirvBuilder::constant_i32(int32_t value) {
  return dedup_constant({spv::OpConstant, type_int(32, true), static_cast<uint32_t>(value)});
}
Id SpirvBuilder::constant_f32(float value) {
  return dedup_constant({spv::OpConstant, type_float(32), std::bit_cast<uint32_t>(value)});
}
Id SpirvBuilder::constant_bool(bool value) {
  const spv::Op opcode = value ? spv::OpConstantTrue : spv::OpConstantFalse;
  return dedup_constant({static_cast<uint32_t>(opcode), type_bool()});
}

Id SpirvBuilder::constant_composite(Id type, std::span<const Id> constituents) {
  std::vector<uint32_t> key{spv::OpConstantComposite, type};
  key.insert(key.end(), constituents.begin(), constituents.end());
  return dedup_constant(std::move(key));
}

Id SpirvBuilder::spec_constant_u32(uint32_t spec_id, uint32_t default_value) {
  const Id type = type_int(32, false);
  const Id id = next_id();
  emit_global(spv::OpSpecConstant, {type, id, default_value});
  decorate(id, spv::DecorationSpecId, {spec_id});
  return id;
}

// --- variables and annotations ---

Id SpirvBuilder::variable(spv::StorageClass storage, Id pointer_type, std::string_view debug_name) {
  const Id id = next_id();
  const uint32_t words[] = {pointer_type, id, static_cast<uint32_t>(storage)};
  if (storage == spv::StorageClassFunction) {
    append_inst(function_vars_, spv::OpVariable, words);
  } else {
    append_inst(globals_, spv::OpVariable, words);
    global_vars_.insert(id);
  }
  if (!debug_name.empty()) {
    std::vector<uint32_t> name{id};
    append_string(name, debug_name);
    append_inst(debug_, spv::OpName, name);
    names_.emplace(id, std::string(debug_name));
  }
  return id;
}

void SpirvBuilder::decorate(Id target, spv::Decoration decoration, std::initializer_list<uint32_t> literals) {
  std::vector<uint32_t> words{target, static_cast<uint32_t>(decoration)};
  words.insert(words.end(), literals.begin(), literals.end());
  append_inst(annotations_, spv::OpDecorate, words);
}

void SpirvBuilder::member_decorate(Id struct_type, uint32_t member, spv::Decoration decoration,
                                   std::initializer_list<uint32_t> literals) {
  std::vector<uint32_t> words{struct_type, member, static_cast<uint32_t>(decoration)};
  words.insert(words.end(), literals.begin(), literals.end());
  append_inst(annotations_, spv::OpMemberDecorate, words);
}

// --- instructions ---

void SpirvBuilder::note_use(Id id) {
  if (global_vars_.count(id)) used_globals_.insert(id);
}

bool SpirvBuilder::is_terminator(spv::Op opcode) const {
  switch (opcode) {
    case spv::OpBranch:
    case spv::OpBranchConditional:
    case spv::OpSwitch:
    case spv::OpReturn:
    case spv::OpReturnValue:
    case spv::OpKill:
    case spv::OpUnreachable:
    case spv::OpTerminateInvocation:
      return true;
    default:
      return false;
  }
}

void SpirvBuilder::emit_body(spv::Op opcode, std::span<const uint32_t> words) {
  assert(main_begun_ && !main_ended_ && "instruction outside main");
  assert(in_block_ && "instruction after a block terminator; call set_block first");
  append_inst(body_, opcode, words);
  if (is_terminator(opcode)) in_block_ = false;
}

Id SpirvBuilder::op(spv::Op opcode, Id result_type, std::initializer_list<Id> operands) {
  const Id id = next_id();
  std::vector<uint32_t> words{result_type, id};
  words.insert(words.end(), operands.begin(), operands.end());
  if (operands.size() > 0) note_use(*operands.begin());
  emit_body(opcode, words);
  return id;
}

void SpirvBuilder::op_void(spv::Op opcode, std::initializer_list<Id> operands) {
  const auto* it = operands.begin();
  if (operands.size() > 0) note_use(it[0]);
  if ((opcode == spv::OpCopyMemory || opcode == spv::OpCopyMemorySized) && operands.size() > 1) note_use(it[1]);
  emit_body(opcode, std::span(operands.begin(), operands.size()));
}

Id SpirvBuilder::ext_inst(Id result_type, uint32_t glsl_std_450_op, std::initializer_list<Id> operands) {
  if (glsl_std_450_ == 0) {
    glsl_std_450_ = next_id();
    std::vector<uint32_t> words{glsl_std_450_};
    append_string(words, "GLSL.std.450");
    append_inst(ext_imports_, spv::OpExtInstImport, words);
  }
  const Id id = next_id();
  std::vector<uint32_t> words{result_type, id, glsl_std_450_, glsl_std_450_op};
  words.insert(words.end(), operands.begin(), operands.end());
  for (Id o : operands) note_use(o);
  emit_body(spv::OpExtInst, words);
  return id;
}

// --- control flow ---

Id SpirvBuilder::new_block() { return next_id(); }

void SpirvBuilder::set_block(Id label) {
  assert(main_begun_ && !main_ended_ && "block outside main");
  assert(!in_block_ && "the previous block is not terminated");
  append_inst(body_, spv::OpLabel, std::span(&label, 1));
  in_block_ = true;
}

void SpirvBuilder::branch(Id target) { emit_body(spv::OpBranch, std::span(&target, 1)); }

void SpirvBuilder::branch_conditional(Id cond, Id true_label, Id false_label) {
  const uint32_t words[] = {cond, true_label, false_label};
  emit_body(spv::OpBranchConditional, words);
}

void SpirvBuilder::selection_merge(Id merge) {
  const uint32_t words[] = {merge, spv::SelectionControlMaskNone};
  emit_body(spv::OpSelectionMerge, words);
}

void SpirvBuilder::loop_merge(Id merge, Id continue_target) {
  const uint32_t words[] = {merge, continue_target, spv::LoopControlMaskNone};
  emit_body(spv::OpLoopMerge, words);
}

void SpirvBuilder::switch_(Id selector, Id default_block, std::span<const std::pair<uint32_t, Id>> cases) {
  std::vector<uint32_t> words{selector, default_block};
  for (const auto& [literal, label] : cases) {
    words.push_back(literal);
    words.push_back(label);
  }
  emit_body(spv::OpSwitch, words);
}

// --- entry point ---

void SpirvBuilder::begin_main() {
  assert(!main_begun_ && "begin_main called twice");
  type_function(type_void());  // declared now so finish() allocates no ids
  main_ = next_id();
  main_label_ = next_id();
  main_begun_ = true;
  in_block_ = true;
}

void SpirvBuilder::end_main() {
  assert(main_begun_ && !main_ended_);
  if (in_block_) op_void(spv::OpReturn, {});
  main_ended_ = true;
}

void SpirvBuilder::entry_point(std::string_view name, std::span<const Id> interface) {
  has_entry_point_ = true;
  entry_name_ = name;
  interface_.assign(interface.begin(), interface.end());
}

void SpirvBuilder::execution_mode(spv::ExecutionMode mode, std::initializer_list<uint32_t> literals) {
  modes_.emplace_back(mode, std::vector<uint32_t>(literals));
}

Result<void> SpirvBuilder::check_interface() const {
  const std::set<Id> listed(interface_.begin(), interface_.end());
  for (Id g : used_globals_) {
    if (listed.count(g)) continue;
    auto it = names_.find(g);
    const std::string what = it != names_.end() ? "'" + it->second + "' (%" + std::to_string(g) + ")"
                                                : "%" + std::to_string(g);
    return Error{ErrorCode::InvalidFormat,
                 "SPIR-V entry point interface does not list the global variable " + what + " that main uses",
                 "from SPIR-V 1.4 on, entry_point() must list every global variable the shader uses"};
  }
  return {};
}

std::vector<uint32_t> SpirvBuilder::finish() {
  const auto check = check_interface();
  assert(check.ok() && "entry point interface misses a used global (check_interface())");
  if (!check.ok()) {
    log::error(check.error().message);
    return {};
  }
  return assemble();
}

Result<std::vector<uint32_t>> SpirvBuilder::try_finish() {
  if (!main_begun_ || !main_ended_ || !has_entry_point_)
    return Error{ErrorCode::Internal, "SPIR-V module finished without main or an entry point", ""};
  LR_TRY(check_interface());
  return assemble();
}

std::vector<uint32_t> SpirvBuilder::assemble() {
  assert(main_begun_ && main_ended_ && "finish() needs begin_main() and end_main()");
  assert(has_entry_point_ && "finish() needs entry_point()");
  // Every id is allocated by now (finish emits no new ones).
  const Id fn_type = type_function(type_void());

  std::vector<uint32_t> m{kMagic, kVersion15, kGenerator, bound_, 0};
  m.insert(m.end(), capabilities_.begin(), capabilities_.end());
  m.insert(m.end(), extensions_.begin(), extensions_.end());
  m.insert(m.end(), ext_imports_.begin(), ext_imports_.end());
  {
    const uint32_t w[] = {static_cast<uint32_t>(addressing_), static_cast<uint32_t>(memory_)};
    append_inst(m, spv::OpMemoryModel, w);
  }
  {
    std::vector<uint32_t> w{static_cast<uint32_t>(model_), main_};
    append_string(w, entry_name_);
    w.insert(w.end(), interface_.begin(), interface_.end());
    append_inst(m, spv::OpEntryPoint, w);
  }
  for (const auto& [mode, literals] : modes_) {
    std::vector<uint32_t> w{main_, static_cast<uint32_t>(mode)};
    w.insert(w.end(), literals.begin(), literals.end());
    append_inst(m, spv::OpExecutionMode, w);
  }
  {
    std::vector<uint32_t> w{main_};
    append_string(w, "main");
    append_inst(m, spv::OpName, w);
  }
  m.insert(m.end(), debug_.begin(), debug_.end());
  m.insert(m.end(), annotations_.begin(), annotations_.end());
  m.insert(m.end(), globals_.begin(), globals_.end());
  {
    const uint32_t w[] = {type_void(), main_, spv::FunctionControlMaskNone, fn_type};
    append_inst(m, spv::OpFunction, w);
  }
  append_inst(m, spv::OpLabel, std::span(&main_label_, 1));
  m.insert(m.end(), function_vars_.begin(), function_vars_.end());
  m.insert(m.end(), body_.begin(), body_.end());
  append_inst(m, spv::OpFunctionEnd, {});
  return m;
}

}  // namespace lr::gpu::shader
