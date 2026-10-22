// SPDX-License-Identifier: MIT
#include "runtime/gpu/shader/disasm.h"

#include <array>
#include <cstddef>
#include <string>
#include <variant>

namespace lr::gpu::shader {

std::string_view cf_op_name(CfOp op) {
  static constexpr std::array<std::string_view, 16> kNames = {
      "nop",      "exec",   "exec_end",  "cond_exec",  "cond_exec_end",        "cond_exec_pred",
      "cond_exec_pred_end", "loop_start", "loop_end", "cond_call", "return", "cond_jmp",
      "alloc",    "cond_exec_pred_clean", "cond_exec_pred_clean_end", "mark_vs_fetch_done"};
  const auto i = static_cast<size_t>(op);
  return i < kNames.size() ? kNames[i] : std::string_view{};
}

std::string_view vector_op_name(VectorOp op) {
  static constexpr std::array<std::string_view, 30> kNames = {
      "add",          "mul",          "max",          "min",          "seq",     "sgt",     "sge",
      "sne",          "frc",          "trunc",        "floor",        "mad",     "cndeq",   "cndge",
      "cndgt",        "dp4",          "dp3",          "dp2add",       "cube",    "max4",    "setp_eq_push",
      "setp_ne_push", "setp_gt_push", "setp_ge_push", "kill_eq",      "kill_gt", "kill_ge", "kill_ne",
      "dst",          "maxa"};
  const auto i = static_cast<size_t>(op);
  return i < kNames.size() ? kNames[i] : std::string_view{};
}

std::string_view scalar_op_name(ScalarOp op) {
  static constexpr std::array<std::string_view, 51> kNames = {
      "adds",      "adds_prev",   "muls",        "muls_prev",   "muls_prev2",  "maxs",        "mins",
      "seqs",      "sgts",        "sges",        "snes",        "frcs",        "truncs",      "floors",
      "exp",       "logc",        "log",         "rcpc",        "rcpf",        "rcp",         "rsqc",
      "rsqf",      "rsq",         "maxas",       "maxasf",      "subs",        "subs_prev",   "setp_eq",
      "setp_ne",   "setp_gt",     "setp_ge",     "setp_inv",    "setp_pop",    "setp_clr",    "setp_rstr",
      "kills_eq",  "kills_gt",    "kills_ge",    "kills_ne",    "kills_one",   "sqrt",        "",
      "mul_const_0", "mul_const_1", "add_const_0", "add_const_1", "sub_const_0", "sub_const_1", "sin",
      "cos",       "retain_prev"};
  const auto i = static_cast<size_t>(op);
  return i < kNames.size() ? kNames[i] : std::string_view{};
}

std::string_view fetch_op_name(FetchOp op) {
  switch (op) {
    case FetchOp::VertexFetch: return "vfetch";
    case FetchOp::TextureFetch: return "tfetch";
    case FetchOp::GetTextureBorderColorFrac: return "get_border_color_frac";
    case FetchOp::GetTextureComputedLod: return "get_comp_tex_lod";
    case FetchOp::GetTextureGradients: return "get_gradients";
    case FetchOp::GetTextureWeights: return "get_weights";
    case FetchOp::SetTextureLod: return "set_tex_lod";
    case FetchOp::SetTextureGradientsHorz: return "set_gradients_h";
    case FetchOp::SetTextureGradientsVert: return "set_gradients_v";
  }
  return {};
}

namespace {

constexpr char kComp[] = "xyzw";

std::string temp_name(uint32_t reg, bool relative) {
  return relative ? "r[aL+" + std::to_string(reg) + "]" : "r" + std::to_string(reg);
}

std::string mask_suffix(uint32_t mask) {
  std::string s = ".";
  for (uint32_t i = 0; i < 4; ++i) s += ((mask >> i) & 1) != 0 ? kComp[i] : '_';
  return s;
}

std::string export_name(ShaderStage stage, uint32_t reg) {
  if (reg == kExportAddress) return "eA";
  if (reg > kExportAddress && reg < kExportData0 + kExportDataCount) return "eM" + std::to_string(reg - kExportData0);
  if (stage == ShaderStage::Vertex) {
    if (reg == kExportVsPosition) return "oPos";
    if (reg == kExportVsPointSize) return "oPts";
    return "o" + std::to_string(reg);
  }
  if (reg == kExportPsDepth) return "oDepth";
  return "oC" + std::to_string(reg);
}

std::string const_name(const AluInstr& a, const AluSource& s) {
  if (!s.const_relative) return "c" + std::to_string(s.reg);
  return "c[" + std::to_string(s.reg) + (a.a0_rel ? "+a0]" : "+aL]");
}

// A full vector operand: the swizzle is printed unless it is the identity.
std::string vector_src(const AluInstr& a, uint32_t i) {
  const AluSource& s = a.src[i];
  std::string base = s.is_temp ? temp_name(s.reg, a.is_src_temp_relative[i]) : const_name(a, s);
  if (s.swizzle != 0) {
    base += '.';
    for (uint32_t c = 0; c < 4; ++c) base += kComp[(((s.swizzle >> (2 * c)) & 3) + c) & 3];
  }
  if (s.abs) base = "|" + base + "|";
  return s.negate ? "-" + base : base;
}

// A scalar operand: component a is the w field of src3's swizzle, b the x field (both component-relative).
std::string scalar_component(const AluInstr& a, bool is_b, bool as_const_temp) {
  const AluSource& s = a.src[2];
  const uint32_t comp = is_b ? (s.swizzle & 3) : (((s.swizzle >> 6) & 3) + 3) & 3;
  std::string base;
  if (as_const_temp)
    base = "r" + std::to_string(a.scalar_const_temp);
  else
    base = s.is_temp ? temp_name(s.reg, a.is_src_temp_relative[2]) : const_name(a, s);
  base += '.';
  base += kComp[comp];
  if (s.abs && !as_const_temp) base = "|" + base + "|";
  return s.negate ? "-" + base : base;
}

std::string predicate_prefix(bool predicated, bool condition) {
  if (!predicated) return "";
  return condition ? "(p0) " : "(!p0) ";
}

std::string alu_text(ShaderStage stage, const AluInstr& a) {
  std::string out = predicate_prefix(a.predicated, a.pred_condition);
  out += vector_op_name(a.vector_op);
  if (a.vector_saturate) out += "_sat";
  out += ' ';
  out += a.export_data ? export_name(stage, a.vector_dest) : temp_name(a.vector_dest, a.vector_dest_rel);
  out += mask_suffix(a.vector_write_mask);
  for (uint32_t i = 0; i < vector_op_source_count(a.vector_op); ++i) out += ", " + vector_src(a, i);

  if (a.scalar_op != ScalarOp::RetainPrev || a.scalar_write_mask != 0) {
    out += " + ";
    out += scalar_op_name(a.scalar_op);
    if (a.scalar_saturate) out += "_sat";
    out += ' ';
    out += a.export_data ? export_name(stage, a.vector_dest) : temp_name(a.scalar_dest, a.scalar_dest_rel);
    out += mask_suffix(a.scalar_write_mask);
    const uint32_t operands = scalar_op_operand_count(a.scalar_op);
    if (scalar_op_is_const(a.scalar_op)) {
      out += ", " + scalar_component(a, false, false) + ", " + scalar_component(a, true, true);
    } else {
      if (operands >= 1) out += ", " + scalar_component(a, false, false);
      if (operands >= 2) out += ", " + scalar_component(a, true, false);
    }
  }
  return out;
}

std::string dst_swizzle_text(uint32_t swizzle) {
  static constexpr char kSel[] = "xyzw01?_";
  std::string s = ".";
  for (uint32_t i = 0; i < 4; ++i) s += kSel[(swizzle >> (3 * i)) & 7];
  return s;
}

std::string fetch_text(const FetchInstr& f) {
  std::string out = predicate_prefix(f.predicated, f.pred_condition);
  out += fetch_op_name(f.op);
  if (f.op == FetchOp::VertexFetch) {
    if (f.mini) out += "_mini";
    out += ' ' + temp_name(f.dst_reg, f.dst_rel) + dst_swizzle_text(f.dst_swizzle);
    out += ", " + temp_name(f.src_reg, f.src_rel) + "." + kComp[f.index_component & 3];
    out += ", vf" + std::to_string(f.slot);
    out += ", format=" + std::to_string(f.format) + ", offset=" + std::to_string(f.offset) +
           ", stride=" + std::to_string(f.stride);
    out += f.is_signed ? ", signed" : ", unsigned";
    if (!f.normalized) out += ", int";
    if (f.exp_adjust != 0) out += ", exp_adjust=" + std::to_string(f.exp_adjust);
    if (f.rounded) out += ", rounded";
    return out;
  }
  static constexpr const char* kDims[] = {"1d", "2d", "3d", "cube"};
  out += ' ';
  out += fetch_writes_dest(f) ? temp_name(f.dst_reg, f.dst_rel) + dst_swizzle_text(f.dst_swizzle) : "-";
  out += ", " + temp_name(f.src_reg, f.src_rel) + ".";
  for (uint32_t i = 0; i < 3; ++i) out += kComp[(f.src_swizzle >> (2 * i)) & 3];
  out += ", tf" + std::to_string(f.const_index);
  out += ", ";
  out += kDims[f.dimension & 3];
  if (f.unnormalized) out += ", unnormalized";
  if (f.use_register_lod) out += ", lod=register";
  if (!f.use_computed_lod) out += ", no_computed_lod";
  if (f.use_register_gradients) out += ", gradients=register";
  if (f.lod_bias != 0.0f) out += ", lod_bias=" + std::to_string(f.lod_bias);
  if (f.offset_x != 0.0f || f.offset_y != 0.0f || f.offset_z != 0.0f)
    out += ", offset=(" + std::to_string(f.offset_x) + "," + std::to_string(f.offset_y) + "," +
           std::to_string(f.offset_z) + ")";
  out += ", filter=" + std::to_string(f.mag_filter) + "/" + std::to_string(f.min_filter) + "/" +
         std::to_string(f.mip_filter) + "/" + std::to_string(f.aniso_filter);
  return out;
}

std::string cf_text(const CfInstr& c) {
  std::string out(cf_op_name(c.op));
  out += ' ';
  const auto cond = [&]() -> std::string {
    if (c.use_predicate) return c.condition ? "p0" : "!p0";
    return std::string(c.condition ? "" : "!") + "b" + std::to_string(c.bool_index);
  };
  switch (c.op) {
    case CfOp::Exec:
    case CfOp::ExecEnd:
    case CfOp::CondExec:
    case CfOp::CondExecEnd:
    case CfOp::CondPredExec:
    case CfOp::CondPredExecEnd:
    case CfOp::CondExecPredClean:
    case CfOp::CondExecPredCleanEnd:
      if (c.op != CfOp::Exec && c.op != CfOp::ExecEnd) out += cond() + " ";
      out += "addr=" + std::to_string(c.address) + " count=" + std::to_string(c.count);
      if (c.count > 0) {
        out += " seq=";
        for (uint32_t k = 0; k < c.count; ++k) out += (k < 6 && ((c.sequence >> (2 * k)) & 1) != 0) ? 'F' : 'A';
      }
      break;
    case CfOp::LoopStart:
      out += "loop" + std::to_string(c.loop_id) + " skip=" + std::to_string(c.target);
      if (c.is_repeat) out += " repeat";
      break;
    case CfOp::LoopEnd:
      out += "loop" + std::to_string(c.loop_id) + " body=" + std::to_string(c.target);
      if (c.is_predicated_break) out += c.pred_break_condition ? " break_if p0" : " break_if !p0";
      break;
    case CfOp::CondCall:
    case CfOp::CondJmp:
      out += (c.is_unconditional ? std::string("always") : cond()) + " target=" + std::to_string(c.target);
      break;
    case CfOp::Alloc: {
      static constexpr const char* kTypes[] = {"none", "position", "interpolators", "memory"};
      out += std::string(kTypes[c.alloc_type & 3]) + " size=" + std::to_string(c.count);
      break;
    }
    case CfOp::Nop:
    case CfOp::Return:
    case CfOp::MarkVsFetchDone:
      break;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

template <size_t N>
std::string bit_list(const std::bitset<N>& bits) {
  std::string s;
  for (size_t i = 0; i < N; ++i)
    if (bits.test(i)) s += (s.empty() ? "" : " ") + std::to_string(i);
  return s.empty() ? "-" : s;
}

}  // namespace

std::string disassemble(const Program& p) {
  std::string out;
  out += p.stage == ShaderStage::Vertex ? "; vertex shader" : "; pixel shader";
  out += ": " + std::to_string(p.cf.size()) + " cf, " + std::to_string(p.slots.size()) + " slots, " +
         std::to_string(p.temp_count) + " temps" + (p.uses_relative_temps ? " (aL-relative)" : "") + "\n";
  out += "; bools: " + bit_list(p.bools_used) + "; loops: " + bit_list(p.loops_used) + "\n";
  out += "; vfetch: " + bit_list(p.vfetch_slots) + "; tfetch: " + bit_list(p.tfetch_slots) + "\n";
  out += "; exports:";
  if (p.writes_position) out += " position";
  if (p.writes_point_size) out += " point_size";
  if (p.writes_depth) out += " depth";
  if (p.interpolators_written != 0) out += " interpolators=" + std::to_string(p.interpolators_written);
  if (p.colors_written != 0) out += " colors=" + std::to_string(p.colors_written);
  if (p.uses_memexport) out += " memexport=" + std::to_string(p.memexport_written);
  if (p.uses_kill) out += " kill";
  out += "\n";

  for (size_t i = 0; i < p.cf.size(); ++i) {
    const CfInstr& c = p.cf[i];
    out += std::to_string(i) + ": " + cf_text(c) + "\n";
    if (!is_exec(c.op)) continue;
    for (uint32_t k = 0; k < c.count; ++k) {
      const size_t slot = size_t{c.address} + k;
      if (slot >= p.slots.size()) break;  // never for a decoded Program
      out += "    " + std::to_string(slot) + "  ";
      if (const auto* a = std::get_if<AluInstr>(&p.slots[slot]))
        out += alu_text(p.stage, *a);
      else
        out += fetch_text(std::get<FetchInstr>(p.slots[slot]));
      out += "\n";
    }
  }
  return out;
}

}  // namespace lr::gpu::shader
