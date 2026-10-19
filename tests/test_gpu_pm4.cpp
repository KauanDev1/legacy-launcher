// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <thread>
#include <vector>

#include "runtime/gpu/null_backend.h"
#include "runtime/gpu/pm4.h"
#include "runtime/gpu/registers.h"
#include "support/pm4_builder.h"

using namespace lr::gpu;
using lr::testing::Pm4Builder;

namespace {

// 1 MiB of "guest memory"; physical address P maps to guest address P + kPhysToGuest when P < kPhysSize, so a
// processor that forgets to translate reads the wrong place.
constexpr uint32_t kMemSize = 0x100000;
constexpr uint32_t kPhysToGuest = 0x40000;
constexpr uint32_t kPhysSize = kMemSize - kPhysToGuest;

constexpr uint32_t kRing = 0x1000;  // guest address of the top-level buffer

struct Fixture {
  std::vector<uint8_t> mem = std::vector<uint8_t>(kMemSize, 0);
  RegisterFile regs;
  NullBackend backend;
  std::vector<uint32_t> interrupts;
  CommandProcessor cp{MemoryAccess{mem.data(),
                                   [](uint32_t phys) -> std::optional<uint32_t> {
                                     if (phys >= kPhysSize) return std::nullopt;
                                     return phys + kPhysToGuest;
                                   },
                                   kMemSize},
                      regs, backend, [this](uint32_t source) { interrupts.push_back(source); }};

  uint8_t* guest(uint32_t addr) { return mem.data() + addr; }
  uint8_t* phys(uint32_t addr) { return mem.data() + addr + kPhysToGuest; }
  uint32_t be32_at(const uint8_t* p) const {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return __builtin_bswap32(v);
  }

  lr::Result<uint32_t> run(const Pm4Builder& b) {
    b.store(guest(kRing));
    return cp.execute(kRing, b.size());
  }
  lr::Result<uint32_t> run_phys(const Pm4Builder& b, uint32_t phys_addr) {
    b.store(phys(phys_addr));
    return cp.execute(phys_addr + kPhysToGuest, b.size());
  }
};

}  // namespace

TEST_CASE("type-0 packet writes consecutive registers", "[unit][gpu]") {
  Fixture f;
  Pm4Builder b;
  b.type0(RB_SURFACE_INFO, {0x11, 0x22, 0x33}).type2();
  auto r = f.run(b);
  REQUIRE(r.ok());
  CHECK(r.value() == b.size());
  CHECK(f.regs.read(RB_SURFACE_INFO) == 0x11);
  CHECK(f.regs.read(RB_COLOR_INFO) == 0x22);
  CHECK(f.regs.read(RB_DEPTH_INFO) == 0x33);
  CHECK(f.regs.read(RB_COLOR1_INFO) == 0);
}

TEST_CASE("one-reg type-0 packet", "[unit][gpu]") {
  Fixture f;
  Pm4Builder b;
  b.type0(SCRATCH_REG2, {1, 2, 3}, /*one_reg=*/true);
  REQUIRE(f.run(b).ok());
  CHECK(f.regs.read(SCRATCH_REG2) == 3);
  CHECK(f.regs.read(SCRATCH_REG3) == 0);
  CHECK(f.regs.read(SCRATCH_REG4) == 0);
}

TEST_CASE("indirect buffer is executed", "[unit][gpu]") {
  Fixture f;
  constexpr uint32_t kIb = 0x8000;  // physical
  Pm4Builder ib;
  ib.type0(SCRATCH_REG0, {0xCAFE});
  ib.store(f.phys(kIb));

  for (uint32_t op : {pm4::INDIRECT_BUFFER, pm4::INDIRECT_BUFFER_PFD}) {
    f.regs.write(SCRATCH_REG0, 0);
    Pm4Builder b;
    b.type3(op, {kIb, ib.size()}).type0(SCRATCH_REG1, {op});
    auto r = f.run(b);
    REQUIRE(r.ok());
    CHECK(r.value() == b.size());
    CHECK(f.regs.read(SCRATCH_REG0) == 0xCAFE);
    CHECK(f.regs.read(SCRATCH_REG1) == op);
  }
  // A zero-length indirect buffer is a no-op.
  Pm4Builder empty;
  empty.type3(pm4::INDIRECT_BUFFER, {kIb, 0});
  CHECK(f.run(empty).ok());
}

TEST_CASE("nested indirect depth limit", "[unit][gpu]") {
  // Chain: ring -> IB1 -> IB2 -> ... -> IBn; the last one writes SCRATCH_REG7.
  auto build_chain = [](Fixture& f, uint32_t levels) {
    const uint32_t base = 0x10000;
    for (uint32_t level = 1; level <= levels; ++level) {
      Pm4Builder ib;
      if (level == levels) {
        ib.type0(SCRATCH_REG7, {level}).type2();  // 3 dwords, like the IB packets
      } else {
        ib.type3(pm4::INDIRECT_BUFFER, {base + level * 0x100, 3});
      }
      ib.store(f.phys(base + (level - 1) * 0x100));
    }
    Pm4Builder ring;
    ring.type3(pm4::INDIRECT_BUFFER, {base, 3});
    return f.run(ring);
  };

  {
    Fixture f;
    auto r = build_chain(f, pm4::kMaxIndirectDepth);
    REQUIRE(r.ok());
    CHECK(f.regs.read(SCRATCH_REG7) == pm4::kMaxIndirectDepth);
  }
  {
    Fixture f;
    auto r = build_chain(f, pm4::kMaxIndirectDepth + 1);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == lr::ErrorCode::InvalidFormat);
    CHECK_THAT(r.error().message, Catch::Matchers::ContainsSubstring("INDIRECT_BUFFER"));
    CHECK(f.regs.read(SCRATCH_REG7) == 0);
  }
}

TEST_CASE("MEM_WRITE fence lands big-endian", "[unit][gpu]") {
  Fixture f;
  constexpr uint32_t kFence = 0x2000;  // physical
  Pm4Builder b;
  // Endian mode 2 (8in32) in the low address bits: the dwords land big-endian, in packet order.
  b.type3(pm4::MEM_WRITE, {kFence | pm4::kEndian8in32, 0x12345678, 0x9ABCDEF0});
  // Endian mode 0: the GPU's native little-endian order.
  b.type3(pm4::MEM_WRITE, {(kFence + 0x10) | pm4::kEndianNone, 0x12345678});
  REQUIRE(f.run(b).ok());
  const uint8_t* p = f.phys(kFence);
  CHECK(p[0] == 0x12);
  CHECK(p[1] == 0x34);
  CHECK(p[2] == 0x56);
  CHECK(p[3] == 0x78);
  CHECK(f.be32_at(p + 4) == 0x9ABCDEF0);
  CHECK(f.be32_at(f.phys(kFence + 0x10)) == 0x78563412);
}

TEST_CASE("EVENT_WRITE_SHD writes the swap counter", "[unit][gpu]") {
  Fixture f;
  constexpr uint32_t kAddr = 0x3000;  // physical
  Pm4Builder b;
  b.type3(pm4::LR_SWAP, {0x1F000000, 1280, 720});
  b.type3(pm4::LR_SWAP, {0x1F100000, 1280, 720});
  // Bit 31 of the initiator: store the swap counter, not the packet's value.
  b.type3(pm4::EVENT_WRITE_SHD, {0x80000000u | 0x07, kAddr | pm4::kEndian8in32, 0xDEAD});
  // Bit 31 clear: store the value.
  b.type3(pm4::EVENT_WRITE_SHD, {0x07, (kAddr + 4) | pm4::kEndian8in32, 0xBEEF});
  REQUIRE(f.run(b).ok());
  CHECK(f.cp.swap_counter() == 2);
  CHECK(f.backend.swap_calls == 2);
  REQUIRE(f.backend.last_swap.has_value());
  CHECK(f.backend.last_swap->frontbuffer_phys == 0x1F100000);
  CHECK(f.backend.last_swap->width == 1280);
  CHECK(f.backend.last_swap->height == 720);
  CHECK(f.be32_at(f.phys(kAddr)) == 2);
  CHECK(f.be32_at(f.phys(kAddr + 4)) == 0xBEEF);
  CHECK(f.regs.read(VGT_EVENT_INITIATOR) == 0x07);
}

TEST_CASE("IM_LOAD reports the shader to the backend", "[unit][gpu]") {
  Fixture f;
  constexpr uint32_t kShader = 0x4000;  // physical, 4-aligned; low 2 bits carry the stage
  Pm4Builder b;
  b.type3(pm4::IM_LOAD, {kShader | 1u /*pixel*/, 0x30});
  REQUIRE(f.run(b).ok());
  CHECK(f.backend.load_shader_calls == 1);
  REQUIRE(f.backend.last_shader.has_value());
  CHECK(f.backend.last_shader->stage == ShaderStage::Pixel);
  CHECK(f.backend.last_shader->guest_addr == kShader + kPhysToGuest);
  CHECK(f.backend.last_shader->dwords == 0x30);

  // IM_LOAD_IMMEDIATE: the microcode follows inline; the backend sees its guest address in the buffer.
  Pm4Builder imm;
  imm.type3(pm4::IM_LOAD_IMMEDIATE, {0 /*vertex*/, 3, 0xA, 0xB, 0xC});
  REQUIRE(f.run(imm).ok());
  CHECK(f.backend.load_shader_calls == 2);
  CHECK(f.backend.last_shader->stage == ShaderStage::Vertex);
  CHECK(f.backend.last_shader->guest_addr == kRing + 3 * 4);
  CHECK(f.backend.last_shader->dwords == 3);

  // The inline microcode must fit in the packet.
  Pm4Builder short_imm;
  short_imm.type3(pm4::IM_LOAD_IMMEDIATE, {0, 8, 0xA});
  auto r = f.run(short_imm);
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == lr::ErrorCode::InvalidFormat);
}

TEST_CASE("DRAW_INDX reports a draw", "[unit][gpu]") {
  Fixture f;
  // prim_type 4 (triangle list), source_select 0 (DMA), index_size 1 (32-bit), 6 indices.
  const uint32_t initiator = 4u | (0u << 6) | (1u << 11) | (6u << 16);
  Pm4Builder b;
  b.type3(pm4::DRAW_INDX, {0 /*viz*/, initiator, 0x5002 /*base, unaligned*/, 6 /*num_words*/});
  REQUIRE(f.run(b).ok());
  CHECK(f.backend.draw_calls == 1);
  REQUIRE(f.backend.last_draw.has_value());
  CHECK(f.backend.last_draw->prim_type == 4);
  CHECK(f.backend.last_draw->index_count == 6);
  CHECK(f.backend.last_draw->indexed);
  CHECK(f.backend.last_draw->index_size == 4);
  CHECK(f.backend.last_draw->index_base_phys == 0x5000);
  CHECK(f.backend.last_draw->vgt_draw_initiator == initiator);
  CHECK(f.regs.read(VGT_DRAW_INITIATOR) == initiator);

  // DRAW_INDX_2 with auto-index (source_select 2): no index buffer.
  const uint32_t auto_init = 5u | (2u << 6) | (3u << 16);
  Pm4Builder b2;
  b2.type3(pm4::DRAW_INDX_2, {auto_init});
  REQUIRE(f.run(b2).ok());
  CHECK(f.backend.draw_calls == 2);
  CHECK(f.backend.last_draw->prim_type == 5);
  CHECK(f.backend.last_draw->index_count == 3);
  CHECK_FALSE(f.backend.last_draw->indexed);
  CHECK(f.backend.last_draw->index_size == 0);

  // With RB_MODECONTROL.edram_mode = copy (6) the "draw" is an EDRAM resolve.
  f.regs.write(RB_MODECONTROL, 6);
  REQUIRE(f.run(b2).ok());
  CHECK(f.backend.draw_calls == 2);
  CHECK(f.backend.resolve_calls == 1);
}

TEST_CASE("WAIT_REG_MEM on memory waits until satisfied", "[unit][gpu]") {
  Fixture f;
  constexpr uint32_t kFlag = 0x6000;  // physical
  Pm4Builder b;
  // Memory poll (bit 4), function 3 (equal): (value & 0xFFFF) == 0x1234, value big-endian (8in32).
  b.type3(pm4::WAIT_REG_MEM, {0x10 | 3, kFlag | pm4::kEndian8in32, 0x1234, 0xFFFF, 0x10});
  b.type0(SCRATCH_REG0, {1});
  b.store(f.guest(kRing));

  std::atomic<bool> released{false};
  std::thread writer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    released.store(true);
    std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(f.phys(kFlag)))
        .store(__builtin_bswap32(0xABCD1234u), std::memory_order_release);
  });
  auto r = f.cp.execute(kRing, b.size());
  writer.join();
  REQUIRE(r.ok());
  CHECK(released.load());
  CHECK(f.regs.read(SCRATCH_REG0) == 1);

  // A wait that never holds ends with Cancelled once request_stop() is called.
  Pm4Builder never;
  never.type3(pm4::WAIT_REG_MEM, {0x10 | 3, (kFlag + 4) | pm4::kEndian8in32, 1, 0xFFFFFFFF, 0x10});
  never.store(f.guest(kRing));
  std::thread stopper([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    f.cp.request_stop();
  });
  auto stopped = f.cp.execute(kRing, never.size());
  stopper.join();
  REQUIRE_FALSE(stopped.ok());
  CHECK(stopped.error().code == lr::ErrorCode::Cancelled);
}

TEST_CASE("unknown opcode is an error", "[unit][gpu]") {
  Fixture f;
  Pm4Builder b;
  b.type3(pm4::NOP, {0}).type3(0x7E, {0, 0});
  auto r = f.run(b);
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == lr::ErrorCode::InvalidFormat);
  CHECK_THAT(r.error().message, Catch::Matchers::ContainsSubstring("unknown PM4 opcode 0x7E"));
  const auto counts = f.cp.opcode_counts();
  CHECK(counts.at(pm4::NOP) == 1);
  CHECK(counts.at(0x7E) == 1);
}

TEST_CASE("malformed packets are fatal, not out-of-bounds", "[unit][gpu]") {
  Fixture f;
  auto expect_invalid = [](const lr::Result<uint32_t>& r) {
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == lr::ErrorCode::InvalidFormat);
  };

  SECTION("type-3 count runs past the end of the buffer") {
    Pm4Builder b;
    b.raw(Pm4Builder::type3_header(pm4::NOP, 10)).raw(0);
    expect_invalid(f.run(b));
  }
  SECTION("type-0 count runs past the end of the buffer") {
    Pm4Builder b;
    b.raw((0x20u << 16) | SCRATCH_REG0).raw(1);
    expect_invalid(f.run(b));
  }
  SECTION("count past the end of guest memory") {
    // The packet sits in the last dword of guest memory and claims 16384 payload dwords (count field 0x3FFF).
    Pm4Builder b;
    b.raw(Pm4Builder::type3_header(pm4::NOP, 0x4000));
    b.store(f.guest(kMemSize - 4));
    expect_invalid(f.cp.execute(kMemSize - 4, 0x4001));
  }
  SECTION("buffer outside guest memory") {
    expect_invalid(f.cp.execute(kMemSize, 1));
    expect_invalid(f.cp.execute(0xFFFFFFFC, 4));
    expect_invalid(f.cp.execute(kRing + 2, 1));  // misaligned
  }
  SECTION("indirect buffer outside guest memory") {
    Pm4Builder unmapped;
    unmapped.type3(pm4::INDIRECT_BUFFER, {kPhysSize + 0x1000, 4});
    expect_invalid(f.run(unmapped));
    Pm4Builder past_end;  // mapped start, length runs past the end of guest memory
    past_end.type3(pm4::INDIRECT_BUFFER, {kPhysSize - 8, 0x1000});
    expect_invalid(f.run(past_end));
  }
  SECTION("zero-length and short packets") {
    // A type-3 header with count 0 encodes 16384 payload dwords: past the end.
    Pm4Builder zero;
    zero.raw((3u << 30) | (0x3FFFu << 16) | (pm4::MEM_WRITE << 8));
    expect_invalid(f.run(zero));
    // Packets shorter than their opcode's fixed payload.
    for (uint32_t op : {pm4::WAIT_REG_MEM, pm4::COND_WRITE, pm4::INDIRECT_BUFFER, pm4::DRAW_INDX,
                        pm4::EVENT_WRITE_SHD, pm4::IM_LOAD, pm4::LOAD_ALU_CONSTANT, pm4::REG_RMW}) {
      Pm4Builder b;
      b.type3(op, {0});
      INFO("opcode " << op);
      expect_invalid(f.run(b));
    }
    // DMA draw without its index-buffer dwords.
    Pm4Builder draw;
    draw.type3(pm4::DRAW_INDX_2, {4u | (6u << 16)});
    expect_invalid(f.run(draw));
  }
  SECTION("memory operands outside guest memory") {
    Pm4Builder write;
    write.type3(pm4::MEM_WRITE, {kPhysSize - 4, 1, 2});  // second dword falls off the mapping
    expect_invalid(f.run(write));
    Pm4Builder alu;
    alu.type3(pm4::LOAD_ALU_CONSTANT, {kPhysSize - 4, 0, 8});
    expect_invalid(f.run(alu));
  }
  CHECK(f.backend.draw_calls == 0);
}

TEST_CASE("constant packets fill the register file", "[unit][gpu]") {
  Fixture f;
  constexpr uint32_t kConsts = 0x7000;  // physical
  const uint8_t src[8] = {0x3F, 0x80, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00};  // 1.0f, 2.0f big-endian
  std::memcpy(f.phys(kConsts), src, sizeof src);
  Pm4Builder b;
  b.type3(pm4::SET_CONSTANT, {(1u << 16) | 6 /*fetch 1, dword 0*/, 0xAA, 0xBB});
  b.type3(pm4::SET_CONSTANT, {(4u << 16) | 0x0208 /*register 0x2208*/, 4});
  b.type3(pm4::SET_CONSTANT2, {SCRATCH_REG5, 0x55});
  b.type3(pm4::SET_SHADER_CONSTANTS, {kShaderConstantLoopBase + 1, 0x77});
  b.type3(pm4::LOAD_ALU_CONSTANT, {kConsts, (0u << 16) | 8 /*vec4 2, X*/, 2});
  b.type3(pm4::REG_RMW, {SCRATCH_REG5, 0xF0, 0x01});  // (0x55 & 0xF0) | 0x01
  b.type3(pm4::COND_WRITE, {3 /*register, equal*/, SCRATCH_REG5, 0x51, 0xFF, SCRATCH_REG6, 0x66});
  REQUIRE(f.run(b).ok());
  CHECK(f.regs.read(kShaderConstantFetchBase + 6) == 0xAA);
  CHECK(f.regs.read(kShaderConstantFetchBase + 7) == 0xBB);
  CHECK(f.regs.read(RB_MODECONTROL) == 4);
  CHECK(f.regs.read(kShaderConstantLoopBase + 1) == 0x77);
  CHECK(f.regs.read_float(kShaderConstantFloatBase + 8) == 1.0f);
  CHECK(f.regs.read_float(kShaderConstantFloatBase + 9) == 2.0f);
  CHECK(f.regs.read(SCRATCH_REG5) == 0x51);
  CHECK(f.regs.read(SCRATCH_REG6) == 0x66);

  Pm4Builder bad_type;
  bad_type.type3(pm4::SET_CONSTANT, {(9u << 16), 1});
  auto r = f.run(bad_type);
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == lr::ErrorCode::InvalidFormat);
}

TEST_CASE("INTERRUPT raises one command interrupt per CPU bit", "[unit][gpu]") {
  Fixture f;
  Pm4Builder b;
  b.type3(pm4::INTERRUPT, {0b101});
  REQUIRE(f.run(b).ok());
  CHECK(f.interrupts == std::vector<uint32_t>{1, 1});
}
