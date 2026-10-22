// SPDX-License-Identifier: MIT
// Xenos control flow -> structured SPIR-V (Task 15.5). Semantics per Xenia's ucode.h and shader translators (reference
// only, nothing copied); where Xenia differs from the hardware's documented behaviour the difference is noted.
//
// Two forms:
//   - straight line, when the program has no jumps, calls or loops (Program::has_jumps_calls_or_loops is false): the
//     exec clauses in CF order, each conditional one inside a selection construct. A conditional exec_end sets the
//     `ended` flag, which guards every later clause; an unconditional one ends the program.
//   - the pc switch otherwise: main is one structured loop around an OpSwitch on a function-local `pc`, one case per
//     CF index that is index 0, a branch target, or follows a branch. A case runs the CF instructions from its index
//     up to the next case (falling through by setting pc to it) or up to a branch, which sets pc. exec_end, return at
//     call depth 0 and a pc past the program set pc to kPcEnd, which leaves the loop. Its merge block holds the stage
//     epilogue, so exports and the single final OpKill come after it.
//
// State (Function variables, set in main's entry block):
//   - p0: the predicate, false at the start, kept across clauses (only ALU PRED_SET* writes it);
//   - the loop stacks, depth 4, as vectors that shift: x is the innermost loop. loop_counts holds the iterations left,
//     al_stack holds aL (x is the current aL; yzw the aL of the enclosing loops, restored when a loop is left). A
//     push past depth 4 drops the outermost entry, a pop past the bottom reads 0, so any guest nesting stays in bounds;
//   - the call stack, depth 4, the same way, plus the depth (0..4);
//   - pc and the count of taken backward branches (kCfBackwardBranchCap).
//
// Loops (ADR 0005 M5). The loop constant (shared block, kSharedLoops) holds the count in bits 0..7, the start of aL
// in bits 8..15 and the signed step of aL in bits 16..23. loop_start with count 0 skips to its target (nothing is
// pushed); otherwise it pushes the count and aL (the start, or with is_repeat the current aL). loop_end decrements the
// count and adds the step to aL, then leaves the loop when the count is 0 or, with is_predicated_break, when
// p0 == pred_break_condition; leaving pops both stacks, which restores the enclosing aL. A cond_jmp from inside a loop
// body to outside it pops once per loop it leaves (bodies are matched statically: a loop_end closes the innermost open
// loop_start), so aL is restored on every exit. Xenia leaves the stacks as they are on such a jump; Xenos semantics,
// as the ADR pins them, restore aL on any exit.
//
// Every guest value stays in bounds by construction: CF targets are checked by decode(), pc values that are not a
// case (a target one past the end) take the default case and end the program, and the stacks never index.
#include <spirv/unified1/GLSL.std.450.h>

#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "runtime/gpu/shader/disasm.h"
#include "runtime/gpu/shader/translator_internal.h"

namespace lr::gpu::shader::detail {

namespace {

constexpr uint32_t kPcEnd = 0xFFFFFFFFu;
constexpr uint32_t kStackDepth = 4;

bool is_cf_noop(CfOp op) { return op == CfOp::Nop || op == CfOp::Alloc || op == CfOp::MarkVsFetchDone; }

bool is_conditional_exec(CfOp op) { return is_exec(op) && op != CfOp::Exec && op != CfOp::ExecEnd; }

class CfEmitter {
 public:
  explicit CfEmitter(Translator& t) : t_(t), b_(t.b_), ty_(t.t_), cf_(t.program_.cf) {}

  void declare_state();
  Result<void> straight_line();
  Result<void> pc_switch();

 private:
  Id load(Id var, Id type) { return b_.op(spv::OpLoad, type, {var}); }
  void store(Id var, Id value) { b_.op_void(spv::OpStore, {var, value}); }
  Id select(Id type, Id cond, Id a, Id b) { return b_.op(spv::OpSelect, type, {cond, a, b}); }
  Id u(uint32_t v) { return t_.const_u(v); }
  Id lnot(Id v) { return b_.op(spv::OpLogicalNot, ty_.bool_, {v}); }
  Id land(Id a, Id b) { return b_.op(spv::OpLogicalAnd, ty_.bool_, {a, b}); }
  // A Function variable of `type` set to `init` (in the entry block).
  Id state_var(Id type, std::string_view name, Id init) {
    const Id v = b_.variable(spv::StorageClassFunction, b_.type_pointer(spv::StorageClassFunction, type), name);
    store(v, init);
    return v;
  }

  Id exec_condition(const CfInstr& c);    // 0 = unconditional
  Id branch_condition(const CfInstr& c);  // cond_jmp / cond_call; 0 = unconditional
  Id push(Id stack, Id type, Id component_type, Id value);
  Id pop(Id stack, Id type);
  void set_pc(Id value) { store(t_.pc_, value); }
  // taken: the condition under which the backward branch is followed (0 = always).
  void count_backward(Id taken);
  uint32_t loops_left(uint32_t from, uint32_t to) const;

  Result<void> run_clause(uint32_t index, const CfInstr& c, Id cond);
  Result<void> emit_case(uint32_t start);
  void loop_start(uint32_t i, const CfInstr& c);
  void loop_end(uint32_t i, const CfInstr& c);
  void jump(uint32_t i, const CfInstr& c);
  void call(uint32_t i, const CfInstr& c);
  void ret(uint32_t i);

  Translator& t_;
  SpirvBuilder& b_;
  const Translator::Types& ty_;
  const std::vector<CfInstr>& cf_;
  std::vector<bool> case_start_;
  std::vector<std::pair<uint32_t, uint32_t>> loop_bodies_;  // (loop_start, loop_end): the body is (first, second]
};

void CfEmitter::declare_state() {
  t_.p0_ = state_var(ty_.bool_, "p0", b_.constant_bool(false));
  const bool branches = t_.program_.has_jumps_calls_or_loops;
  if (branches || t_.temps_.relative) {
    const Id zero = b_.constant_i32(0);
    const Id zeros[] = {zero, zero, zero, zero};
    t_.al_stack_ = state_var(ty_.ivec4, "aL", b_.constant_composite(ty_.ivec4, zeros));
  }
  if (branches) {
    const Id zeros[] = {u(0), u(0), u(0), u(0)};
    const Id zero4 = b_.constant_composite(ty_.uvec4, zeros);
    t_.loop_counts_ = state_var(ty_.uvec4, "loop_count", zero4);
    t_.call_stack_ = state_var(ty_.uvec4, "call_stack", zero4);
    t_.call_depth_ = state_var(ty_.u32, "call_depth", u(0));
    t_.pc_ = state_var(ty_.u32, "pc", u(0));
    t_.backward_ = state_var(ty_.u32, "backward_branches", u(0));
    return;
  }
  for (const CfInstr& c : cf_) {
    if (is_exec_end(c.op) && is_conditional_exec(c.op)) {
      t_.ended_ = state_var(ty_.bool_, "ended", b_.constant_bool(false));
      return;
    }
  }
}

Id CfEmitter::exec_condition(const CfInstr& c) {
  switch (c.op) {
    case CfOp::CondExec:
    case CfOp::CondExecEnd:
    case CfOp::CondExecPredClean:  // a bool constant too; "clean" only tells the sequencer p0 is not written inside
    case CfOp::CondExecPredCleanEnd:
      return t_.bool_matches(c.bool_index, c.condition);
    case CfOp::CondPredExec:
    case CfOp::CondPredExecEnd:
      return t_.pred_matches(c.condition);
    default:
      return 0;
  }
}

Id CfEmitter::branch_condition(const CfInstr& c) {
  if (c.is_unconditional) return 0;
  return c.use_predicate ? t_.pred_matches(c.condition) : t_.bool_matches(c.bool_index, c.condition);
}

Id CfEmitter::push(Id stack, Id type, Id component_type, Id value) {
  return b_.op(spv::OpCompositeConstruct, type,
               {value, t_.extract(stack, 0, component_type), t_.extract(stack, 1, component_type),
                t_.extract(stack, 2, component_type)});
}

Id CfEmitter::pop(Id stack, Id type) {
  const Id zero = type == ty_.ivec4 ? b_.constant_i32(0) : u(0);
  const Id zeros[] = {zero, zero, zero, zero};
  return b_.op(spv::OpVectorShuffle, type, {stack, b_.constant_composite(type, zeros), 1, 2, 3, 4});
}

void CfEmitter::count_backward(Id taken) {
  const Id n = load(t_.backward_, ty_.u32);
  const Id step = taken ? select(ty_.u32, taken, u(1), u(0)) : u(1);
  store(t_.backward_, b_.op(spv::OpIAdd, ty_.u32, {n, step}));
}

uint32_t CfEmitter::loops_left(uint32_t from, uint32_t to) const {
  uint32_t n = 0;
  for (const auto& [first, last] : loop_bodies_) {
    const bool from_inside = first < from && from <= last;
    const bool to_inside = first < to && to <= last;
    if (from_inside && !to_inside) ++n;
  }
  return n;
}

Result<void> CfEmitter::run_clause(uint32_t index, const CfInstr& c, Id cond) {
  if (!cond) return t_.emit_exec_clause(index, c);
  return t_.emit_if(cond, [&] { return t_.emit_exec_clause(index, c); });
}

// --- straight line ------------------------------------------------------------------------------------------------

Result<void> CfEmitter::straight_line() {
  for (uint32_t i = 0; i < cf_.size(); ++i) {
    const CfInstr& c = cf_[i];
    if (is_cf_noop(c.op)) continue;
    if (!is_exec(c.op))
      return Translator::internal("straight-line control flow met cf " + std::string(cf_op_name(c.op)));
    const Id cond = exec_condition(c);
    Id run = cond;
    if (t_.ended_) {
      const Id live = lnot(load(t_.ended_, ty_.bool_));
      run = run ? land(run, live) : live;
    }
    LR_TRY(run_clause(i, c, run));
    if (!is_exec_end(c.op)) continue;
    if (!cond) return {};  // exec_end: the program ends here
    if (!t_.ended_) return Translator::internal("conditional exec_end without the ended flag");
    store(t_.ended_, b_.op(spv::OpLogicalOr, ty_.bool_, {load(t_.ended_, ty_.bool_), run}));
  }
  return {};
}

// --- the pc switch ------------------------------------------------------------------------------------------------

void CfEmitter::loop_start(uint32_t i, const CfInstr& c) {
  const Id lc = t_.load_shared_u32(kSharedLoops, c.loop_id);
  const Id count = b_.op(spv::OpBitwiseAnd, ty_.u32, {lc, u(0xFF)});
  const Id enter = b_.op(spv::OpINotEqual, ty_.bool_, {count, u(0)});
  const Id counts = load(t_.loop_counts_, ty_.uvec4);
  store(t_.loop_counts_, select(ty_.uvec4, enter, push(counts, ty_.uvec4, ty_.u32, count), counts));
  const Id al = load(t_.al_stack_, ty_.ivec4);
  const Id first = c.is_repeat
                       ? t_.extract(al, 0, ty_.i32)
                       : b_.op(spv::OpBitcast, ty_.i32,
                               {b_.op(spv::OpBitwiseAnd, ty_.u32,
                                      {b_.op(spv::OpShiftRightLogical, ty_.u32, {lc, u(8)}), u(0xFF)})});
  store(t_.al_stack_, select(ty_.ivec4, enter, push(al, ty_.ivec4, ty_.i32, first), al));
  set_pc(select(ty_.u32, enter, u(i + 1), u(c.target)));
  if (c.target <= i) count_backward(lnot(enter));
}

void CfEmitter::loop_end(uint32_t i, const CfInstr& c) {
  const Id lc = t_.load_shared_u32(kSharedLoops, c.loop_id);
  // The signed step in bits 16..23: shift it to the top, then arithmetic shift down.
  const Id step = b_.op(spv::OpShiftRightArithmetic, ty_.i32,
                        {b_.op(spv::OpBitcast, ty_.i32, {b_.op(spv::OpShiftLeftLogical, ty_.u32, {lc, u(8)})}), u(24)});
  const Id counts = load(t_.loop_counts_, ty_.uvec4);
  const Id al = load(t_.al_stack_, ty_.ivec4);
  const Id left = b_.op(spv::OpISub, ty_.u32, {t_.extract(counts, 0, ty_.u32), u(1)});
  const Id next_al = b_.op(spv::OpIAdd, ty_.i32, {t_.extract(al, 0, ty_.i32), step});
  Id leave = b_.op(spv::OpIEqual, ty_.bool_, {left, u(0)});
  if (c.is_predicated_break)
    leave = b_.op(spv::OpLogicalOr, ty_.bool_, {leave, t_.pred_matches(c.pred_break_condition)});
  const Id stay_counts = b_.op(spv::OpCompositeInsert, ty_.uvec4, {left, counts, 0});
  const Id stay_al = b_.op(spv::OpCompositeInsert, ty_.ivec4, {next_al, al, 0});
  store(t_.loop_counts_, select(ty_.uvec4, leave, pop(counts, ty_.uvec4), stay_counts));
  store(t_.al_stack_, select(ty_.ivec4, leave, pop(al, ty_.ivec4), stay_al));
  set_pc(select(ty_.u32, leave, u(i + 1), u(c.target)));
  if (c.target <= i) count_backward(lnot(leave));
}

void CfEmitter::jump(uint32_t i, const CfInstr& c) {
  const Id cond = branch_condition(c);
  if (const uint32_t n = loops_left(i, c.target); n > 0) {
    // Leaving loop bodies: pop their counts and aL (at most the stack depth matters).
    Id counts = load(t_.loop_counts_, ty_.uvec4);
    Id al = load(t_.al_stack_, ty_.ivec4);
    Id popped_counts = counts, popped_al = al;
    for (uint32_t k = 0; k < n && k < kStackDepth; ++k) {
      popped_counts = pop(popped_counts, ty_.uvec4);
      popped_al = pop(popped_al, ty_.ivec4);
    }
    store(t_.loop_counts_, cond ? select(ty_.uvec4, cond, popped_counts, counts) : popped_counts);
    store(t_.al_stack_, cond ? select(ty_.ivec4, cond, popped_al, al) : popped_al);
  }
  set_pc(cond ? select(ty_.u32, cond, u(c.target), u(i + 1)) : u(c.target));
  if (c.target <= i) count_backward(cond);
}

void CfEmitter::call(uint32_t i, const CfInstr& c) {
  const Id cond = branch_condition(c);
  const Id stack = load(t_.call_stack_, ty_.uvec4);
  const Id depth = load(t_.call_depth_, ty_.u32);
  const Id pushed = push(stack, ty_.uvec4, ty_.u32, u(i + 1));
  const Id deeper = t_.glsl(ty_.u32, GLSLstd450UMin, {b_.op(spv::OpIAdd, ty_.u32, {depth, u(1)}), u(kStackDepth)});
  store(t_.call_stack_, cond ? select(ty_.uvec4, cond, pushed, stack) : pushed);
  store(t_.call_depth_, cond ? select(ty_.u32, cond, deeper, depth) : deeper);
  set_pc(cond ? select(ty_.u32, cond, u(c.target), u(i + 1)) : u(c.target));
  if (c.target <= i) count_backward(cond);
}

void CfEmitter::ret(uint32_t i) {
  // At depth 0 a return ends the program (the brief's rule; Xenia's ucode.h calls it a no-op outside a function).
  const Id depth = load(t_.call_depth_, ty_.u32);
  const Id in_call = b_.op(spv::OpINotEqual, ty_.bool_, {depth, u(0)});
  const Id stack = load(t_.call_stack_, ty_.uvec4);
  const Id address = t_.extract(stack, 0, ty_.u32);
  store(t_.call_stack_, select(ty_.uvec4, in_call, pop(stack, ty_.uvec4), stack));
  store(t_.call_depth_, select(ty_.u32, in_call, b_.op(spv::OpISub, ty_.u32, {depth, u(1)}), depth));
  set_pc(select(ty_.u32, in_call, address, u(kPcEnd)));
  count_backward(land(in_call, b_.op(spv::OpULessThanEqual, ty_.bool_, {address, u(i)})));
}

Result<void> CfEmitter::emit_case(uint32_t start) {
  const auto n = static_cast<uint32_t>(cf_.size());
  for (uint32_t i = start;; ++i) {
    if (i >= n) {
      set_pc(u(kPcEnd));
      return {};
    }
    if (i != start && case_start_[i]) {
      set_pc(u(i));  // fall through to the next case
      return {};
    }
    const CfInstr& c = cf_[i];
    if (is_cf_noop(c.op)) continue;
    if (is_exec(c.op)) {
      const Id cond = exec_condition(c);
      LR_TRY(run_clause(i, c, cond));
      if (!is_exec_end(c.op)) continue;
      set_pc(cond ? select(ty_.u32, cond, u(kPcEnd), u(i + 1)) : u(kPcEnd));
      return {};
    }
    switch (c.op) {
      case CfOp::LoopStart:
        loop_start(i, c);
        return {};
      case CfOp::LoopEnd:
        loop_end(i, c);
        return {};
      case CfOp::CondJmp:
        jump(i, c);
        return {};
      case CfOp::CondCall:
        call(i, c);
        return {};
      case CfOp::Return:
        ret(i);
        return {};
      default:
        return Translator::internal("pc switch met cf " + std::string(cf_op_name(c.op)));
    }
  }
}

Result<void> CfEmitter::pc_switch() {
  const auto n = static_cast<uint32_t>(cf_.size());
  if (n == 0) return Translator::internal("a program without control flow");

  // Cases: index 0, every branch target, every index after a branch.
  case_start_.assign(n, false);
  case_start_[0] = true;
  const auto mark = [&](uint32_t index) {
    if (index < n) case_start_[index] = true;
  };
  std::vector<uint32_t> open_loops;
  for (uint32_t i = 0; i < n; ++i) {
    const CfInstr& c = cf_[i];
    switch (c.op) {
      case CfOp::LoopStart:
        open_loops.push_back(i);
        mark(c.target);
        mark(i + 1);
        break;
      case CfOp::LoopEnd:
        if (!open_loops.empty()) {
          loop_bodies_.emplace_back(open_loops.back(), i);
          open_loops.pop_back();
        }
        mark(c.target);
        mark(i + 1);
        break;
      case CfOp::CondJmp:
      case CfOp::CondCall:
        mark(c.target);
        mark(i + 1);
        break;
      case CfOp::Return:
        mark(i + 1);
        break;
      default:
        if (is_exec_end(c.op) && is_conditional_exec(c.op)) mark(i + 1);
        break;
    }
  }
  for (uint32_t first : open_loops) loop_bodies_.emplace_back(first, n);  // never closed: the rest of the program

  std::vector<std::pair<uint32_t, Id>> cases;
  for (uint32_t i = 0; i < n; ++i)
    if (case_start_[i]) cases.emplace_back(i, b_.new_block());
  const Id header = b_.new_block();
  const Id body = b_.new_block();
  const Id default_case = b_.new_block();
  const Id switch_merge = b_.new_block();
  const Id continue_block = b_.new_block();
  const Id merge = b_.new_block();

  b_.branch(header);
  b_.set_block(header);
  b_.loop_merge(merge, continue_block);
  b_.branch(body);

  b_.set_block(body);
  const Id selector = load(t_.pc_, ty_.u32);  // before the merge instruction, which must precede the switch
  b_.selection_merge(switch_merge);
  b_.switch_(selector, default_case, cases);
  for (const auto& [index, label] : cases) {
    b_.set_block(label);
    LR_TRY(emit_case(index));
    b_.branch(switch_merge);
  }
  b_.set_block(default_case);  // kPcEnd, or a target one past the last instruction
  set_pc(u(kPcEnd));
  b_.branch(switch_merge);

  b_.set_block(switch_merge);
  b_.branch(continue_block);

  b_.set_block(continue_block);
  const Id running = land(b_.op(spv::OpINotEqual, ty_.bool_, {load(t_.pc_, ty_.u32), u(kPcEnd)}),
                          b_.op(spv::OpULessThan, ty_.bool_, {load(t_.backward_, ty_.u32), u(kCfBackwardBranchCap)}));
  b_.branch_conditional(running, header, merge);

  b_.set_block(merge);
  return {};
}

}  // namespace

// --- Translator ---------------------------------------------------------------------------------------------------

Id Translator::pred_matches(bool condition) {
  if (!p0_) {
    record_internal("p0 read before the control-flow state exists");
    return b_.constant_bool(false);
  }
  const Id p = b_.op(spv::OpLoad, t_.bool_, {p0_});
  return condition ? p : b_.op(spv::OpLogicalNot, t_.bool_, {p});
}

Id Translator::bool_matches(uint32_t bool_index, bool condition) {
  // Bool b is bit b % 32 of word b / 32 (kSharedBools).
  const Id word = load_shared_u32(kSharedBools, (bool_index / 32) % 8);
  const Id bit = b_.op(spv::OpBitwiseAnd, t_.u32,
                       {b_.op(spv::OpShiftRightLogical, t_.u32, {word, const_u(bool_index % 32)}), const_u(1)});
  return b_.op(condition ? spv::OpINotEqual : spv::OpIEqual, t_.bool_, {bit, const_u(0)});
}

Id Translator::current_al() {
  if (!al_stack_) {
    record_internal("aL read without the loop state");
    return b_.constant_i32(0);
  }
  return extract(b_.op(spv::OpLoad, t_.ivec4, {al_stack_}), 0, t_.i32);
}

Result<std::pair<Id, Id>> Translator::emit_scalar_pred_set(uint32_t slot, const AluInstr& a) {
  // PRED_SET{EQ,NE,GT,GE}s compare operand a (src3's W after the swizzle) with 0: p0 = the comparison, and the
  // result is 0.0 when it holds, 1.0 otherwise. NaN: only "not equal" holds.
  LR_TRY_ASSIGN(src, load_source(slot, a, 2));
  const Id x = extract(src, 3);
  spv::Op compare = spv::OpFOrdEqual;
  switch (a.scalar_op) {
    case ScalarOp::PredSetEq: compare = spv::OpFOrdEqual; break;
    case ScalarOp::PredSetNe: compare = spv::OpFUnordNotEqual; break;
    case ScalarOp::PredSetGt: compare = spv::OpFOrdGreaterThan; break;
    case ScalarOp::PredSetGe: compare = spv::OpFOrdGreaterThanEqual; break;
    default: return unsupported(slot, "alu", "scalar op " + std::string(scalar_op_name(a.scalar_op)));
  }
  if (!p0_) return internal("predicate set before the control-flow state exists");
  const Id p = b_.op(compare, t_.bool_, {x, const_f(0.0f)});
  const Id result = b_.op(spv::OpSelect, t_.f32, {p, const_f(0.0f), const_f(1.0f)});
  return std::pair<Id, Id>{result, p};
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

Result<void> Translator::emit_control_flow() {
  CfEmitter cf(*this);
  cf.declare_state();
  return program_.has_jumps_calls_or_loops ? cf.pc_switch() : cf.straight_line();
}

}  // namespace lr::gpu::shader::detail
