// SPDX-License-Identifier: MIT
// The Xenos microcode decoder and disassembler (runtime/gpu/shader/ir.h, disasm.h). Every shader here is assembled
// by hand with tests/support/xenos_shader_builder.h.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "runtime/gpu/shader/disasm.h"
#include "runtime/gpu/shader/ir.h"
#include "support/xenos_shader_builder.h"

namespace x = lr::testing::xenos;
namespace sh = lr::gpu::shader;
using lr::gpu::ShaderStage;

namespace {

sh::Program decode_ok(ShaderStage stage, const std::vector<uint32_t>& code) {
  auto r = sh::decode(stage, code);
  if (!r) FAIL("decode failed: " << r.error().message);
  return std::move(r.value());
}

std::string decode_error(ShaderStage stage, const std::vector<uint32_t>& code) {
  auto r = sh::decode(stage, code);
  REQUIRE_FALSE(r);
  CHECK(r.error().code == lr::ErrorCode::InvalidFormat);
  return r.error().message;
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

const sh::AluInstr& alu_at(const sh::Program& p, uint32_t slot) {
  REQUIRE(slot < p.slots.size());
  REQUIRE(std::holds_alternative<sh::AluInstr>(p.slots[slot]));
  return std::get<sh::AluInstr>(p.slots[slot]);
}

const sh::FetchInstr& fetch_at(const sh::Program& p, uint32_t slot) {
  REQUIRE(slot < p.slots.size());
  REQUIRE(std::holds_alternative<sh::FetchInstr>(p.slots[slot]));
  return std::get<sh::FetchInstr>(p.slots[slot]);
}

// A plain ALU that writes r0 from r0 (no export).
x::Alu move_alu() {
  x::Alu a;
  a.vector_op = x::kVectorMax;
  a.vector_dest = 0;
  return a;
}

}  // namespace

TEST_CASE("cf pair unpacking matches the builder", "[unit][gpu]") {
  std::vector<uint32_t> code;
  x::emit(code, x::cf_pair(x::cf_exec(false, 4, 1, 0), x::cf_cond_exec(false, 4, 1, 0, 7, true)));
  x::emit(code, x::cf_pair(x::cf_loop_start(3, 4, true), x::cf_loop_end(3, 3)));
  x::emit(code, x::cf_pair(x::cf_cond_jmp(6, 9, false), x::cf_call(7)));
  x::emit(code, x::cf_pair(x::cf_alloc(x::kAllocPosition, 2), x::cf_cond_exec_pred(true, 4, 1, 0, true)));
  x::emit(code, x::alu(move_alu()));

  const sh::Program p = decode_ok(ShaderStage::Vertex, code);
  REQUIRE(p.cf.size() == 8);

  CHECK(p.cf[0].op == sh::CfOp::Exec);
  CHECK(p.cf[0].address == 4);
  CHECK(p.cf[0].count == 1);

  CHECK(p.cf[1].op == sh::CfOp::CondExec);
  CHECK(p.cf[1].address == 4);
  CHECK(p.cf[1].bool_index == 7);
  CHECK(p.cf[1].condition);
  CHECK_FALSE(p.cf[1].use_predicate);

  CHECK(p.cf[2].op == sh::CfOp::LoopStart);
  CHECK(p.cf[2].loop_id == 3);
  CHECK(p.cf[2].target == 4);
  CHECK(p.cf[2].is_repeat);

  CHECK(p.cf[3].op == sh::CfOp::LoopEnd);
  CHECK(p.cf[3].loop_id == 3);
  CHECK(p.cf[3].target == 3);
  CHECK_FALSE(p.cf[3].is_predicated_break);

  CHECK(p.cf[4].op == sh::CfOp::CondJmp);
  CHECK(p.cf[4].target == 6);
  CHECK(p.cf[4].bool_index == 9);
  CHECK_FALSE(p.cf[4].condition);
  CHECK_FALSE(p.cf[4].is_unconditional);
  CHECK_FALSE(p.cf[4].use_predicate);

  CHECK(p.cf[5].op == sh::CfOp::CondCall);
  CHECK(p.cf[5].target == 7);
  CHECK(p.cf[5].is_unconditional);

  CHECK(p.cf[6].op == sh::CfOp::Alloc);
  CHECK(p.cf[6].alloc_type == x::kAllocPosition);
  CHECK(p.cf[6].count == 2);

  CHECK(p.cf[7].op == sh::CfOp::CondPredExecEnd);
  CHECK(p.cf[7].use_predicate);
  CHECK(p.cf[7].condition);
  CHECK(p.cf[7].address == 4);

  CHECK(p.bools_used.test(7));
  CHECK(p.bools_used.test(9));
  CHECK(p.bools_used.count() == 2);
  CHECK(p.loops_used.test(3));
  CHECK(p.has_jumps_calls_or_loops);
}

TEST_CASE("exec clause decodes its alu and fetch slots", "[unit][gpu]") {
  std::vector<uint32_t> code;
  // slot 1: ALU, slot 2: fetch, slot 3: fetch (sequence bit 0 of each 2-bit field).
  x::emit(code, x::cf_pair(x::cf_exec(true, 1, 3, (1u << 2) | (1u << 4)), x::cf_nop()));

  x::Alu a;
  a.vector_op = x::kVectorMulAdd;
  a.vector_dest = 2;
  a.vector_mask = 0x5;
  a.vector_saturate = true;
  a.scalar_op = x::kScalarRecip;
  a.scalar_dest = 3;
  a.scalar_mask = 0x8;
  a.src1 = {1, true, 0x1B, true};
  a.src1.abs = true;
  a.src2 = {5, false};
  a.src3 = {200, false};
  a.abs_constants = true;
  a.const1_relative = true;
  a.address_register_relative = true;
  a.predicated = true;
  a.pred_condition = true;
  x::emit(code, x::alu(a));

  x::VFetch v;
  v.dst = 4;
  v.src = 0;
  v.src_component = 2;
  v.slot = 5;
  v.offset_dwords = 4;
  v.stride_dwords = 8;
  v.format = 57;
  v.is_signed = true;
  v.normalized = false;
  v.exp_adjust = -3;
  v.rounded = true;
  x::emit(code, x::vfetch(v));

  x::TFetch t;
  t.dst = 6;
  t.src = 7;
  t.const_index = 9;
  t.dimension = x::kTextureDim3D;
  t.use_computed_lod = false;
  t.use_register_lod = true;
  t.lod_bias = -8;
  t.offset_x = 1;
  t.offset_y = -2;
  t.offset_z = 3;
  t.mag_filter = 1;
  t.min_filter = 0;
  t.aniso_filter = 2;
  t.arbitrary_filter = 5;
  t.unnormalized = true;
  t.predicated = true;
  t.pred_condition = false;
  x::emit(code, x::tfetch(t));

  const sh::Program p = decode_ok(ShaderStage::Pixel, code);
  REQUIRE(p.cf.size() == 2);
  CHECK(p.cf[0].op == sh::CfOp::ExecEnd);
  CHECK(p.cf[0].count == 3);
  CHECK(p.cf[1].op == sh::CfOp::Nop);
  REQUIRE(p.slots.size() == 4);

  const sh::AluInstr& alu = alu_at(p, 1);
  CHECK(alu.vector_op == sh::VectorOp::MulAdd);
  CHECK(alu.scalar_op == sh::ScalarOp::Recip);
  CHECK(alu.vector_dest == 2);
  CHECK(alu.vector_write_mask == 0x5);
  CHECK(alu.vector_saturate);
  CHECK_FALSE(alu.scalar_saturate);
  CHECK(alu.scalar_dest == 3);
  CHECK(alu.scalar_write_mask == 0x8);
  CHECK(alu.predicated);
  CHECK(alu.pred_condition);
  CHECK(alu.a0_rel);
  CHECK(alu.src[0].is_temp);
  CHECK(alu.src[0].reg == 1);
  CHECK(alu.src[0].swizzle == 0x1B);
  CHECK(alu.src[0].negate);
  CHECK(alu.src[0].abs);
  CHECK_FALSE(alu.src[1].is_temp);
  CHECK(alu.src[1].reg == 5);
  CHECK(alu.src[1].abs);  // abs_constants
  CHECK_FALSE(alu.src[1].const_relative);  // first constant operand: const0_rel (clear)
  CHECK(alu.src[2].reg == 200);
  CHECK(alu.src[2].const_relative);  // second constant operand: const1_rel

  const sh::FetchInstr& vf = fetch_at(p, 2);
  CHECK(vf.op == sh::FetchOp::VertexFetch);
  CHECK(vf.dst_reg == 4);
  CHECK(vf.src_reg == 0);
  CHECK(vf.index_component == 2);
  CHECK(vf.slot == 5);
  CHECK(vf.offset == 4);
  CHECK(vf.stride == 8);
  CHECK(vf.format == 57);
  CHECK(vf.is_signed);
  CHECK_FALSE(vf.normalized);
  CHECK(vf.exp_adjust == -3);
  CHECK(vf.rounded);
  CHECK_FALSE(vf.mini);
  CHECK(vf.dst_swizzle == x::kSwizzleXyzw);

  const sh::FetchInstr& tf = fetch_at(p, 3);
  CHECK(tf.op == sh::FetchOp::TextureFetch);
  CHECK(tf.dst_reg == 6);
  CHECK(tf.src_reg == 7);
  CHECK(tf.const_index == 9);
  CHECK(tf.dimension == x::kTextureDim3D);
  CHECK_FALSE(tf.use_computed_lod);
  CHECK(tf.use_register_lod);
  CHECK(tf.lod_bias == -0.5f);
  CHECK(tf.offset_x == 0.5f);
  CHECK(tf.offset_y == -1.0f);
  CHECK(tf.offset_z == 1.5f);
  CHECK(tf.mag_filter == 1);
  CHECK(tf.min_filter == 0);
  CHECK(tf.aniso_filter == 2);
  CHECK(tf.arbitrary_filter == 5);
  CHECK(tf.unnormalized);
  CHECK(tf.predicated);
  CHECK_FALSE(tf.pred_condition);
  CHECK(tf.src_swizzle == (0u | (1u << 2) | (2u << 4)));

  CHECK(p.temp_count == 8);  // r7 is the highest register used
}

TEST_CASE("exec past the end of the microcode is an error", "[unit][gpu]") {
  std::vector<uint32_t> code;
  x::emit(code, x::cf_pair(x::cf_exec(true, 1, 3, 0), x::cf_nop()));
  x::emit(code, x::alu(move_alu()));  // only slot 1 exists; the clause wants slots 1..3
  const std::string msg = decode_error(ShaderStage::Vertex, code);
  CHECK(contains(msg, "instruction 0"));
  CHECK(contains(msg, "past the end"));

  SECTION("an address far outside is also rejected") {
    std::vector<uint32_t> far;
    x::emit(far, x::cf_pair(x::cf_exec(true, 0xFFF, 6, 0), x::cf_nop()));
    CHECK(contains(decode_error(ShaderStage::Vertex, far), "past the end"));
  }
  SECTION("an exec that points into the control flow is rejected") {
    std::vector<uint32_t> overlap;
    x::emit(overlap, x::cf_pair(x::cf_exec(false, 2, 1, 0), x::cf_exec(true, 0, 1, 0)));
    x::emit(overlap, x::alu(move_alu()));
    x::emit(overlap, x::alu(move_alu()));
    const std::string m = decode_error(ShaderStage::Vertex, overlap);
    CHECK(contains(m, "instruction 1"));
    CHECK(contains(m, "control flow"));
  }
  SECTION("a microcode shorter than one control-flow pair is rejected") {
    CHECK(contains(decode_error(ShaderStage::Vertex, {0, 0}), "instruction 0"));
  }
  SECTION("random microcode decodes or fails, never reads outside it (ASan)") {
    uint32_t state = 0x12345678u;
    const auto next = [&] {
      state = state * 1664525u + 1013904223u;
      return state;
    };
    constexpr uint32_t kRounds = 2000;
    constexpr uint32_t kFetchOps[] = {0, 1, 16, 17, 18, 19, 24, 25, 26};
    uint32_t decoded = 0;
    uint32_t shaped = 0;
    uint32_t shaped_decoded = 0;
    for (uint32_t round = 0; round < kRounds; ++round) {
      std::vector<uint32_t> random;
      if (round & 1) {
        // Shaped: one exec_end over 1..6 random slots whose opcodes are valid and that export nothing, so decoding
        // reaches every ALU and fetch field instead of stopping at an early check.
        ++shaped;
        const uint32_t n = 1 + next() % 6;
        const uint32_t sequence = next() & 0xFFF;
        x::emit(random, x::cf_pair(x::cf_exec(true, 1, n, sequence), x::cf_nop()));
        for (uint32_t k = 0; k < n; ++k) {
          std::array<uint32_t, 3> w = {next(), next(), next()};
          if ((sequence >> (2 * k)) & 1) {
            w[0] = (w[0] & ~0x1Fu) | kFetchOps[next() % 9];
            if ((w[0] & 0x1F) == 0) {
              w[0] = (w[0] & ~(3u << 25)) | ((next() % 3) << 25);  // a vertex fetch constant select 0..2
              w[1] &= ~(1u << 30);                                // a full fetch, never mini
            }
          } else {
            uint32_t scalar = next() % 51;
            if (scalar == 41) scalar = 40;
            w[0] = (w[0] & ~(0x3Fu << 26) & ~(1u << 15)) | (scalar << 26);  // valid scalar op, no export
            w[2] = (w[2] & ~(0x1Fu << 24)) | ((next() % 30) << 24);         // valid vector op
          }
          x::emit(random, w);
        }
      } else {
        random.resize(next() % 64);
        for (uint32_t& w : random) w = next();
        // Small exec addresses so some clauses land inside the buffer.
        if (random.size() >= 3 && (round & 2)) random[0] &= ~0xFF0u;
      }
      const auto stage = (round & 4) ? ShaderStage::Pixel : ShaderStage::Vertex;
      auto r = sh::decode(stage, random);
      if (r) {
        ++decoded;
        if (round & 1) ++shaped_decoded;
        CHECK(r.value().temp_count <= sh::kMaxTemps);
        CHECK_FALSE(sh::disassemble(r.value()).empty());
      } else {
        CHECK(contains(r.error().message, "instruction "));
        if (round & 1) FAIL_CHECK("a shaped program failed: " << r.error().message);
      }
    }
    // Every shaped program is valid, so at least half of all rounds decode fully.
    CHECK(shaped_decoded == shaped);
    CHECK(decoded >= kRounds / 2);
  }
}

TEST_CASE("jump target outside the program is an error", "[unit][gpu]") {
  std::vector<uint32_t> code;
  x::emit(code, x::cf_pair(x::cf_jmp(9), x::cf_exec(true, 1, 1, 0)));
  x::emit(code, x::alu(move_alu()));
  const std::string msg = decode_error(ShaderStage::Vertex, code);
  CHECK(contains(msg, "instruction 0"));
  CHECK(contains(msg, "target 9"));

  SECTION("calls and loop ends are checked too") {
    std::vector<uint32_t> call;
    x::emit(call, x::cf_pair(x::cf_exec(true, 1, 1, 0), x::cf_call(2)));
    x::emit(call, x::alu(move_alu()));
    CHECK(contains(decode_error(ShaderStage::Vertex, call), "instruction 1"));

    std::vector<uint32_t> loop;
    x::emit(loop, x::cf_pair(x::cf_loop_end(0, 0x1FFF), x::cf_exec(true, 1, 1, 0)));
    x::emit(loop, x::alu(move_alu()));
    CHECK(contains(decode_error(ShaderStage::Vertex, loop), "instruction 0"));
  }
}

TEST_CASE("unknown vector opcode is an error naming the slot", "[unit][gpu]") {
  std::vector<uint32_t> code;
  x::emit(code, x::cf_pair(x::cf_exec(true, 1, 2, 0), x::cf_nop()));
  x::emit(code, x::alu(move_alu()));
  x::Alu bad = move_alu();
  bad.vector_op = 30;
  x::emit(code, x::alu(bad));
  const std::string msg = decode_error(ShaderStage::Pixel, code);
  CHECK(contains(msg, "instruction 2"));
  CHECK(contains(msg, "vector opcode 30"));

  SECTION("unknown scalar and fetch opcodes too") {
    std::vector<uint32_t> s;
    x::emit(s, x::cf_pair(x::cf_exec(true, 1, 1, 0), x::cf_nop()));
    x::Alu sc = move_alu();
    sc.scalar_op = 41;
    x::emit(s, x::alu(sc));
    CHECK(contains(decode_error(ShaderStage::Pixel, s), "scalar opcode 41"));

    std::vector<uint32_t> f;
    x::emit(f, x::cf_pair(x::cf_exec(true, 1, 1, 1), x::cf_nop()));
    x::TFetch t;
    t.opcode = 5;
    x::emit(f, x::tfetch(t));
    const std::string m = decode_error(ShaderStage::Pixel, f);
    CHECK(contains(m, "instruction 1"));
    CHECK(contains(m, "fetch opcode 5"));
  }
}

TEST_CASE("empty microcode is an error", "[unit][gpu]") {
  const std::string msg = decode_error(ShaderStage::Vertex, {});
  CHECK(contains(msg, "empty"));
}


TEST_CASE("program summary lists used constants and fetch slots", "[unit][gpu]") {
  std::vector<uint32_t> code;
  // cf 0..5; slots 3..8.
  x::emit(code, x::cf_pair(x::cf_cond_exec(false, 3, 2, (1u << 0) | (1u << 2), 17, true), x::cf_loop_start(4, 4)));
  x::emit(code, x::cf_pair(x::cf_cond_exec_pred_clean(false, 5, 1, 0, 200, false), x::cf_loop_end(4, 2)));
  x::emit(code, x::cf_pair(x::cf_exec(true, 6, 3, 1u << 4), x::cf_nop()));
  x::emit(code, x::vfetch(1, 0, 5, 0, 4, x::kFormat32_32_32_32Float));   // slot 3
  x::emit(code, x::vfetch(2, 0, 95, 0, 4, x::kFormat32_32_32_32Float));  // slot 4
  x::Alu out = move_alu();
  out.export_data = true;
  out.vector_dest = 3;  // interpolator 3
  out.src1 = {2, true};
  out.src2 = {2, true};
  x::emit(code, x::alu(out));  // slot 5
  x::Alu pos = out;
  pos.vector_dest = x::kExportVsPosition;
  x::emit(code, x::alu(pos));  // slot 6
  x::Alu psize = out;
  psize.vector_dest = x::kExportVsPointSize;
  psize.vector_mask = 0;
  psize.scalar_op = x::kScalarMax;
  psize.scalar_mask = 1;
  x::emit(code, x::alu(psize));  // slot 7
  x::emit(code, x::tfetch(5, 1, 31, x::kTextureDim2D));  // slot 8

  const sh::Program p = decode_ok(ShaderStage::Vertex, code);
  CHECK(p.bools_used.test(17));
  CHECK(p.bools_used.test(200));
  CHECK(p.bools_used.count() == 2);
  CHECK(p.loops_used.test(4));
  CHECK(p.loops_used.count() == 1);
  CHECK(p.vfetch_slots.test(5));
  CHECK(p.vfetch_slots.test(95));
  CHECK(p.vfetch_slots.count() == 2);
  CHECK(p.tfetch_slots.test(31));
  CHECK(p.tfetch_slots.count() == 1);
  CHECK(p.interpolators_written == (1u << 3));
  CHECK(p.writes_position);
  CHECK(p.writes_point_size);
  CHECK_FALSE(p.writes_depth);
  CHECK_FALSE(p.uses_memexport);
  CHECK_FALSE(p.uses_kill);
  CHECK_FALSE(p.uses_relative_temps);
  CHECK(p.has_jumps_calls_or_loops);
  CHECK(p.temp_count == 6);  // r5 (the tfetch destination) is the highest

  SECTION("pixel shader colors, depth and kill") {
    std::vector<uint32_t> ps;
    x::emit(ps, x::cf_pair(x::cf_exec(true, 1, 3, 0), x::cf_nop()));
    x::Alu c1 = out;
    c1.vector_dest = x::kExportPsColor1;
    x::emit(ps, x::alu(c1));
    x::Alu depth = out;
    depth.vector_dest = x::kExportPsDepth;
    x::emit(ps, x::alu(depth));
    x::Alu kill = move_alu();
    kill.vector_op = x::kVectorKillEq;
    kill.vector_mask = 0;
    x::emit(ps, x::alu(kill));
    const sh::Program q = decode_ok(ShaderStage::Pixel, ps);
    CHECK(q.colors_written == 0x2);
    CHECK(q.writes_depth);
    CHECK(q.uses_kill);
    CHECK_FALSE(q.has_jumps_calls_or_loops);
    CHECK(q.interpolators_written == 0);
  }
  SECTION("a vertex fetch constant past 95 is an error") {
    std::vector<uint32_t> bad;
    x::emit(bad, x::cf_pair(x::cf_exec(true, 1, 1, 1), x::cf_nop()));
    auto words = x::vfetch(1, 0, 0, 0, 4, x::kFormat32_32_32_32Float);
    words[0] |= (31u << 20) | (3u << 25);  // const_index 31, select 3: slot 96
    x::emit(bad, words);
    const std::string m = decode_error(ShaderStage::Vertex, bad);
    CHECK(contains(m, "instruction 1"));
    CHECK(contains(m, "96"));
  }
}

TEST_CASE("memexport destination is recorded", "[unit][gpu]") {
  const ShaderStage stage = GENERATE(ShaderStage::Vertex, ShaderStage::Pixel);
  std::vector<uint32_t> code;
  x::emit(code, x::cf_pair(x::cf_alloc(x::kAllocMemory, 1), x::cf_exec(true, 1, 3, 0)));
  x::Alu ea = move_alu();
  ea.export_data = true;
  ea.vector_dest = x::kExportAddress;
  x::emit(code, x::alu(ea));
  x::Alu em0 = ea;
  em0.vector_dest = x::kExportData0;
  x::emit(code, x::alu(em0));
  x::Alu em4 = ea;
  em4.vector_dest = x::kExportData0 + 4;  // 37
  em4.vector_mask = 0;
  em4.scalar_op = x::kScalarMax;
  em4.scalar_mask = 0x1;
  x::emit(code, x::alu(em4));

  const sh::Program p = decode_ok(stage, code);
  CHECK(p.uses_memexport);
  CHECK(p.memexport_written == (1u << 0 | 1u << 1 | 1u << 5));
  CHECK(p.cf[0].alloc_type == x::kAllocMemory);
  CHECK(p.interpolators_written == 0);
  CHECK(p.colors_written == 0);
  CHECK_FALSE(p.writes_position);

  SECTION("an export register the stage does not have is an error") {
    std::vector<uint32_t> bad;
    x::emit(bad, x::cf_pair(x::cf_exec(true, 1, 1, 0), x::cf_nop()));
    x::Alu e = ea;
    e.vector_dest = 40;
    x::emit(bad, x::alu(e));
    const std::string m = decode_error(stage, bad);
    CHECK(contains(m, "instruction 1"));
    CHECK(contains(m, "export register 40"));
  }
}

TEST_CASE("export constant writes follow the vector and scalar masks", "[unit][gpu]") {
  std::vector<uint32_t> code;
  x::emit(code, x::cf_pair(x::cf_exec(true, 1, 4, 0), x::cf_nop()));
  x::Alu zero = move_alu();  // o3 = 0000: no mask, scalar_dest_rel
  zero.export_data = true;
  zero.vector_dest = 3;
  zero.vector_mask = 0;
  zero.scalar_mask = 0;
  zero.scalar_dest_relative = true;
  x::emit(code, x::alu(zero));  // slot 1
  x::Alu pos = move_alu();      // oPos.xyz1: w is in both masks
  pos.export_data = true;
  pos.vector_dest = x::kExportVsPosition;
  pos.vector_mask = 0xF;
  pos.scalar_op = x::kScalarMax;
  pos.scalar_mask = 0x8;
  x::emit(code, x::alu(pos));  // slot 2
  x::Alu split = pos;          // o1: xy from the vector op, zw from the scalar op
  split.vector_dest = 1;
  split.vector_mask = 0x3;
  split.scalar_mask = 0xC;
  x::emit(code, x::alu(split));  // slot 3
  x::Alu none = move_alu();      // o5 with no mask and no scalar_dest_rel: writes nothing
  none.export_data = true;
  none.vector_dest = 5;
  none.vector_mask = 0;
  x::emit(code, x::alu(none));  // slot 4

  const sh::Program p = decode_ok(ShaderStage::Vertex, code);
  const sh::AluInstr& z = alu_at(p, 1);
  CHECK(z.export_const0_mask == 0xF);
  CHECK(z.export_const1_mask == 0);
  CHECK(z.export_vector_mask == 0);
  CHECK(z.export_scalar_mask == 0);
  const sh::AluInstr& o = alu_at(p, 2);
  CHECK(o.export_vector_mask == 0x7);
  CHECK(o.export_scalar_mask == 0);
  CHECK(o.export_const1_mask == 0x8);
  CHECK(o.export_const0_mask == 0);
  const sh::AluInstr& s = alu_at(p, 3);
  CHECK(s.export_vector_mask == 0x3);
  CHECK(s.export_scalar_mask == 0xC);
  CHECK(s.export_const0_mask == 0);
  CHECK(s.export_const1_mask == 0);
  CHECK(alu_at(p, 4).export_const0_mask == 0);

  CHECK(p.interpolators_written == ((1u << 3) | (1u << 1)));  // o3 counts, o5 does not
  CHECK(p.writes_position);
  CHECK_FALSE(p.uses_relative_temps);  // scalar_dest_rel on an export is not a relative temporary

  const std::string text = sh::disassemble(p);
  INFO(text);
  CHECK(contains(text, "o3.0000"));
  CHECK(contains(text, "oPos.xyz1"));
  CHECK(contains(text, "o1.xy__"));
  CHECK(contains(text, "maxs o1.__zw"));

  SECTION("non-export instructions have no export masks") {
    const sh::AluInstr plain = std::get<sh::AluInstr>(  // a copy: the Program is a temporary
        decode_ok(ShaderStage::Vertex, [] {
          std::vector<uint32_t> c;
          x::emit(c, x::cf_pair(x::cf_exec(true, 1, 1, 0), x::cf_nop()));
          x::emit(c, x::alu(move_alu()));
          return c;
        }()).slots[1]);
    CHECK((plain.export_vector_mask | plain.export_scalar_mask | plain.export_const0_mask |
           plain.export_const1_mask) == 0);
  }
}

TEST_CASE("aL-relative source and destination temps are decoded", "[unit][gpu]") {
  std::vector<uint32_t> code;
  x::emit(code, x::cf_pair(x::cf_exec(true, 1, 2, 1u << 2), x::cf_nop()));
  x::Alu a = move_alu();
  a.vector_op = x::kVectorAdd;
  a.vector_dest = 3;
  a.vector_dest_relative = true;
  a.src1 = {1, true};
  a.src1.relative = true;
  a.src2 = {2, true};
  a.scalar_op = x::kScalarAdd;
  a.scalar_dest = 4;
  a.scalar_dest_relative = true;
  a.scalar_mask = 0x2;
  a.src3 = {5, true};
  a.src3.relative = true;
  x::emit(code, x::alu(a));
  x::TFetch t;
  t.dst = 6;
  t.dst_relative = true;
  t.src = 7;
  t.src_relative = true;
  x::emit(code, x::tfetch(t));

  const sh::Program p = decode_ok(ShaderStage::Pixel, code);
  const sh::AluInstr& alu = alu_at(p, 1);
  CHECK(alu.vector_dest == 3);
  CHECK(alu.vector_dest_rel);
  CHECK(alu.scalar_dest == 4);
  CHECK(alu.scalar_dest_rel);
  CHECK(alu.is_src_temp_relative[0]);
  CHECK_FALSE(alu.is_src_temp_relative[1]);
  CHECK(alu.is_src_temp_relative[2]);
  CHECK(alu.src[0].reg == 1);  // the index bits only, without the relative flag
  CHECK(alu.src[2].reg == 5);
  const sh::FetchInstr& tf = fetch_at(p, 2);
  CHECK(tf.src_rel);
  CHECK(tf.dst_rel);
  CHECK(tf.src_reg == 7);
  CHECK(tf.dst_reg == 6);
  CHECK(p.uses_relative_temps);
  CHECK(p.temp_count == sh::kMaxTemps);

  SECTION("a relative export destination is an error") {
    std::vector<uint32_t> bad;
    x::emit(bad, x::cf_pair(x::cf_exec(true, 1, 1, 0), x::cf_nop()));
    x::Alu e = move_alu();
    e.export_data = true;
    e.vector_dest = x::kExportVsPosition;
    e.vector_dest_relative = true;
    x::emit(bad, x::alu(e));
    CHECK(contains(decode_error(ShaderStage::Vertex, bad), "instruction 1"));
  }
}

TEST_CASE("negative vfetch offset is sign-extended", "[unit][gpu]") {
  std::vector<uint32_t> code;
  x::emit(code, x::cf_pair(x::cf_exec(true, 1, 3, 0x15), x::cf_nop()));
  x::VFetch full;
  full.dst = 1;
  full.src = 0;
  full.src_component = 1;
  full.slot = 7;
  full.stride_dwords = 3;
  full.offset_dwords = -1;
  x::emit(code, x::vfetch(full));
  x::emit(code, x::vfetch_mini(2, -4194304, 6));
  x::VFetch max = full;
  max.offset_dwords = 4194303;
  x::emit(code, x::vfetch(max));

  const sh::Program p = decode_ok(ShaderStage::Vertex, code);
  CHECK(fetch_at(p, 1).offset == -1);
  const sh::FetchInstr& mini = fetch_at(p, 2);
  CHECK(mini.mini);
  CHECK(mini.offset == -4194304);
  CHECK(mini.format == 6);
  CHECK(mini.dst_reg == 2);
  CHECK(mini.slot == 7);  // inherited from the full fetch
  CHECK(mini.stride == 3);
  CHECK(mini.src_reg == 0);
  CHECK(mini.index_component == 1);
  CHECK(fetch_at(p, 3).offset == 4194303);
  CHECK(p.vfetch_slots.count() == 1);

  SECTION("a mini fetch without a full fetch before it is an error") {
    std::vector<uint32_t> bad;
    x::emit(bad, x::cf_pair(x::cf_exec(true, 1, 1, 1), x::cf_nop()));
    x::emit(bad, x::vfetch_mini(2, 0, 6));
    const std::string m = decode_error(ShaderStage::Vertex, bad);
    CHECK(contains(m, "instruction 1"));
    CHECK(contains(m, "mini"));
  }
}

TEST_CASE("loop_end predicated break is decoded", "[unit][gpu]") {
  const bool condition = GENERATE(false, true);
  std::vector<uint32_t> code;
  x::emit(code, x::cf_pair(x::cf_loop_start(2, 2), x::cf_loop_end(2, 1, true, condition)));
  x::emit(code, x::cf_pair(x::cf_exec(true, 2, 1, 0), x::cf_nop()));
  x::emit(code, x::alu(move_alu()));

  const sh::Program p = decode_ok(ShaderStage::Vertex, code);
  REQUIRE(p.cf.size() == 4);
  CHECK(p.cf[1].op == sh::CfOp::LoopEnd);
  CHECK(p.cf[1].loop_id == 2);
  CHECK(p.cf[1].target == 1);
  CHECK(p.cf[1].is_predicated_break);
  CHECK(p.cf[1].pred_break_condition == condition);
  CHECK(p.loops_used.test(2));
}

TEST_CASE("disassembly names every opcode of a mixed shader", "[unit][gpu]") {
  // cf 0..15 (8 pairs); slots 8..14.
  std::vector<uint32_t> code;
  x::emit(code, x::cf_pair(x::cf_exec(false, 8, 2, 1u << 2), x::cf_cond_exec(false, 10, 1, 0, 1, true)));
  x::emit(code, x::cf_pair(x::cf_cond_exec_pred(false, 11, 2, 0x5, true), x::cf_loop_start(0, 5)));
  x::emit(code, x::cf_pair(x::cf_loop_end(0, 4), x::cf_cond_jmp(7, 2, true)));
  x::emit(code, x::cf_pair(x::cf_cond_call(14, 3, false), x::cf_alloc(x::kAllocInterpolators, 1)));
  x::emit(code, x::cf_pair(x::cf_cond_exec_pred_clean(false, 13, 1, 0, 4, true), x::cf_mark_vs_fetch_done()));
  x::emit(code, x::cf_pair(x::cf_nop(), x::cf_cond_exec(true, 14, 1, 0, 5, false)));
  x::emit(code, x::cf_pair(x::cf_cond_exec_pred(true, 14, 1, 0, false),
                           x::cf_cond_exec_pred_clean(true, 14, 1, 0, 6, true)));
  x::emit(code, x::cf_pair(x::cf_ret(), x::cf_exec(true, 14, 1, 0)));

  x::Alu a8 = move_alu();
  a8.vector_op = x::kVectorAdd;
  a8.scalar_op = x::kScalarMul;
  a8.scalar_mask = 1;
  a8.scalar_dest = 1;
  x::emit(code, x::alu(a8));                                            // slot 8
  x::emit(code, x::vfetch(2, 0, 0, 0, 4, x::kFormat32_32_32_32Float));  // slot 9
  x::Alu a10 = move_alu();
  a10.vector_op = x::kVectorDot4;
  a10.scalar_op = x::kScalarRecip;
  a10.scalar_mask = 2;
  x::emit(code, x::alu(a10));                            // slot 10
  x::emit(code, x::tfetch(3, 1, 0, x::kTextureDim2D));  // slot 11
  x::TFetch lod;
  lod.opcode = x::kFetchSetLod;
  lod.src = 1;
  x::emit(code, x::tfetch(lod));  // slot 12
  x::Alu a13 = move_alu();
  a13.vector_op = x::kVectorKillEq;
  a13.vector_mask = 0;
  x::emit(code, x::alu(a13));  // slot 13
  x::Alu a14 = move_alu();
  a14.vector_op = x::kVectorMul;
  a14.scalar_op = x::kScalarMulConst0;
  a14.scalar_mask = 1;
  a14.src3 = {12, false};
  a14.abs_constants = true;  // the CONST ops apply it to both operands
  x::emit(code, x::alu(a14));  // slot 14

  const sh::Program p = decode_ok(ShaderStage::Vertex, code);
  const std::string text = sh::disassemble(p);
  INFO(text);
  for (const char* name :
       {"nop", " exec ", "exec_end", "cond_exec ", "cond_exec_end", "cond_exec_pred ", "cond_exec_pred_end",
        "loop_start", "loop_end", "cond_call", "return", "cond_jmp", "alloc", "cond_exec_pred_clean ",
        "cond_exec_pred_clean_end", "mark_vs_fetch_done", "add ", "muls ", "vfetch ", "dp4 ", "rcp ", "tfetch ",
        "set_tex_lod", "kill_eq", "mul ", "mul_const_0 r0.x___, |c12.w|, |r0.x|"}) {
    CHECK(contains(text, name));
  }
  // Every opcode has a name, so a later dump never prints a placeholder.
  for (uint32_t op = 0; op < 16; ++op) CHECK_FALSE(sh::cf_op_name(sh::CfOp(op)).empty());
  for (uint32_t op = 0; op < 30; ++op) CHECK_FALSE(sh::vector_op_name(sh::VectorOp(op)).empty());
  for (uint32_t op = 0; op <= 50; ++op) {
    if (op != 41) CHECK_FALSE(sh::scalar_op_name(sh::ScalarOp(op)).empty());
  }
}
