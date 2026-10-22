// SPDX-License-Identifier: MIT
// PM4 packet semantics per Xenia command_processor.cc and xenos.h (reference only): header layouts, opcode
// numbers, payload layouts and the endian modes. The code below is our own.
#include "runtime/gpu/pm4.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <thread>
#include <utility>

namespace lr::gpu {

Backend::~Backend() = default;

namespace pm4 {

const char* opcode_name(uint32_t opcode) {
  switch (opcode) {
#define LR_PM4_NAME(op) \
  case op:              \
    return #op;
    LR_PM4_NAME(NOP) LR_PM4_NAME(REG_RMW) LR_PM4_NAME(DRAW_INDX) LR_PM4_NAME(VIZ_QUERY) LR_PM4_NAME(WAIT_FOR_IDLE)
    LR_PM4_NAME(IM_LOAD) LR_PM4_NAME(IM_LOAD_IMMEDIATE) LR_PM4_NAME(SET_CONSTANT) LR_PM4_NAME(LOAD_ALU_CONSTANT)
    LR_PM4_NAME(DRAW_INDX_2) LR_PM4_NAME(INDIRECT_BUFFER_PFD) LR_PM4_NAME(INVALIDATE_STATE)
    LR_PM4_NAME(WAIT_REG_MEM) LR_PM4_NAME(MEM_WRITE) LR_PM4_NAME(REG_TO_MEM) LR_PM4_NAME(INDIRECT_BUFFER)
    LR_PM4_NAME(COND_WRITE) LR_PM4_NAME(EVENT_WRITE) LR_PM4_NAME(ME_INIT) LR_PM4_NAME(SET_BIN_BASE_OFFSET)
    LR_PM4_NAME(SET_BIN_MASK) LR_PM4_NAME(SET_BIN_SELECT) LR_PM4_NAME(INTERRUPT) LR_PM4_NAME(SET_CONSTANT2)
    LR_PM4_NAME(SET_SHADER_CONSTANTS) LR_PM4_NAME(EVENT_WRITE_SHD) LR_PM4_NAME(EVENT_WRITE_EXT)
    LR_PM4_NAME(EVENT_WRITE_ZPD) LR_PM4_NAME(CONTEXT_UPDATE) LR_PM4_NAME(SET_BIN_MASK_LO)
    LR_PM4_NAME(SET_BIN_MASK_HI) LR_PM4_NAME(SET_BIN_SELECT_LO) LR_PM4_NAME(SET_BIN_SELECT_HI)
    LR_PM4_NAME(LR_SWAP)
#undef LR_PM4_NAME
    default:
      return nullptr;
  }
}

uint32_t gpu_swap(uint32_t value, uint32_t endian) {
  switch (endian & 3) {
    case kEndian8in16:
      return ((value << 8) & 0xFF00FF00u) | ((value >> 8) & 0x00FF00FFu);
    case kEndian8in32:
      return __builtin_bswap32(value);
    case kEndian16in32:
      return (value << 16) | (value >> 16);
    default:
      return value;
  }
}

}  // namespace pm4

namespace {

std::string hex(uint32_t v, int digits = 8) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%0*X", digits, v);
  return b;
}

Error malformed(std::string message) { return Error{ErrorCode::InvalidFormat, std::move(message), {}}; }

std::string op_label(uint32_t opcode) {
  const char* n = pm4::opcode_name(opcode);
  return n != nullptr ? std::string(n) : "opcode " + hex(opcode, 2);
}

// VGT_DRAW_INITIATOR fields.
constexpr uint32_t kSourceDma = 0, kSourceImmediate = 1, kSourceAutoIndex = 2;
// RB_MODECONTROL.edram_mode (bits 0-2): colour+depth and depth-only draw, copy turns the draw into a resolve,
// anything else (0 = ignore) draws nothing.
constexpr uint32_t kEdramModeColorDepth = 4, kEdramModeDepthOnly = 5, kEdramModeCopy = 6;
// SET_CONSTANT / LOAD_ALU_CONSTANT type field -> register base.
constexpr uint32_t kConstantBases[] = {kShaderConstantFloatBase, kShaderConstantFetchBase, kShaderConstantBoolBase,
                                       kShaderConstantLoopBase, 0x2000 /* context registers */};
// VGT_EVENT_INITIATOR events used by VIZ_QUERY.
constexpr uint32_t kEventVizQueryStart = 7, kEventVizQueryEnd = 8;
// EVENT_WRITE_EXT screen extents: no real hi-z data, so report the whole surface (max dimension 8192 / 8 tiles).
constexpr uint16_t kExtentMaxXY = 8192 >> 3;

}  // namespace

CommandProcessor::CommandProcessor(MemoryAccess mem, RegisterFile& regs, Backend& backend,
                                   std::function<void(uint32_t source, uint32_t cpu)> raise_interrupt)
    : mem_(std::move(mem)), regs_(regs), backend_(backend), raise_interrupt_(std::move(raise_interrupt)) {}

bool CommandProcessor::in_bounds(uint32_t guest, uint64_t bytes) const {
  return mem_.base != nullptr && uint64_t{guest} <= mem_.size && bytes <= mem_.size - guest;
}

uint32_t CommandProcessor::read_be(uint32_t guest) const {
  uint32_t v;
  std::memcpy(&v, mem_.base + guest, sizeof v);
  return __builtin_bswap32(v);
}

Result<uint32_t> CommandProcessor::translate(uint32_t phys, uint64_t bytes, const char* what) const {
  const uint32_t masked = phys & pm4::kPhysicalMask;
  std::optional<uint32_t> guest = mem_.to_guest ? mem_.to_guest(masked) : std::nullopt;
  if (!guest) return malformed(std::string(what) + ": physical address " + hex(phys) + " maps to no guest memory");
  if (!in_bounds(*guest, bytes)) {
    return malformed(std::string(what) + ": " + std::to_string(bytes) + " bytes at physical " + hex(phys) +
                     " run past the end of guest memory");
  }
  if (bytes == 4 && (*guest & 3) != 0) {
    return malformed(std::string(what) + ": physical " + hex(phys) + " translates to unaligned guest " +
                     hex(*guest));
  }
  return *guest;
}

Result<uint32_t> CommandProcessor::load_mem(uint32_t phys_and_endian, const char* what) const {
  LR_TRY_ASSIGN(guest, translate(phys_and_endian & ~3u, 4, what));
  // Atomic: another thread (the CPU side) may be writing the value we poll; a plain load could be hoisted.
  const uint32_t raw = std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(mem_.base + guest))
                           .load(std::memory_order_acquire);
  // `raw` holds the bytes as the little-endian GPU sees them; the endian mode says how to reorder them.
  return pm4::gpu_swap(raw, phys_and_endian);
}

Result<void> CommandProcessor::store_mem(uint32_t phys_and_endian, uint32_t value, const char* what) {
  LR_TRY_ASSIGN(guest, translate(phys_and_endian & ~3u, 4, what));
  // The GPU stores little-endian after applying the endian mode: kEndian8in32 (what D3D uses for fences) lands
  // big-endian, as the guest CPU reads it. Release, so a CPU thread that sees the fence sees earlier writes.
  std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(mem_.base + guest))
      .store(pm4::gpu_swap(value, phys_and_endian), std::memory_order_release);
  return {};
}

Result<void> CommandProcessor::write_register(uint32_t index, uint32_t value) {
  regs_.write(index, value);
  // Scratch write-back: with bit n of SCRATCH_UMSK set, SCRATCH_REGn is mirrored to SCRATCH_ADDR + 4n (big-endian),
  // which is how the guest reads GPU progress without touching MMIO.
  if (index >= SCRATCH_REG0 && index <= SCRATCH_REG7) {
    const uint32_t n = index - SCRATCH_REG0;
    if (((regs_.read(SCRATCH_UMSK) >> n) & 1) != 0) {
      const uint32_t address = (regs_.read(SCRATCH_ADDR) & ~3u) + 4 * n;
      return store_mem(address | pm4::kEndian8in32, value, "scratch register write-back (SCRATCH_ADDR)");
    }
  }
  return {};
}

Result<bool> CommandProcessor::compare(uint32_t wait_info, uint32_t poll, uint32_t ref, uint32_t mask) {
  uint32_t value;
  if ((wait_info & 0x10) != 0) {
    LR_TRY_ASSIGN(v, load_mem(poll, "WAIT_REG_MEM/COND_WRITE poll address"));
    value = v;
  } else {
    if (poll == COHER_STATUS_HOST) {
      // Our guest memory is always coherent with the GPU: a requested cache sync (bit 31) completes at once.
      regs_.write(COHER_STATUS_HOST, regs_.read(COHER_STATUS_HOST) & ~0x80000000u);
    }
    value = regs_.read(poll);
  }
  value &= mask;
  switch (wait_info & 7) {
    case 0: return false;  // never
    case 1: return value < ref;
    case 2: return value <= ref;
    case 3: return value == ref;
    case 4: return value != ref;
    case 5: return value >= ref;
    case 6: return value > ref;
    default: return true;  // always
  }
}

Result<uint32_t> CommandProcessor::execute(uint32_t guest_addr, uint32_t dwords) {
  if ((guest_addr & 3) != 0) return malformed("PM4 buffer at " + hex(guest_addr) + " is not 4-byte aligned");
  if (!in_bounds(guest_addr, uint64_t{dwords} * 4)) {
    return malformed("PM4 buffer at " + hex(guest_addr) + " (" + std::to_string(dwords) +
                     " dwords) lies outside guest memory");
  }
  return execute_buffer(guest_addr, dwords, 0);
}

Result<uint32_t> CommandProcessor::execute_buffer(uint32_t guest_addr, uint32_t dwords, uint32_t depth) {
  // The caller checked [guest_addr, guest_addr + 4 * dwords) against guest memory.
  uint32_t pos = 0;
  // A packet whose payload runs past the buffer: the ring buffer leaves it for the next call (the rest arrives
  // after the wrap); inside an indirect buffer it is malformed.
  auto incomplete = [&](uint32_t packet_pos, uint32_t needed, const std::string& label) -> Result<uint32_t> {
    if (depth == 0) return packet_pos;
    return malformed("PM4 " + label + " packet at " + hex(guest_addr + packet_pos * 4) + " has " +
                     std::to_string(needed) + " dwords but the indirect buffer has " +
                     std::to_string(dwords - packet_pos - 1) + " left");
  };
  while (pos < dwords) {
    const uint32_t packet_pos = pos;
    const uint32_t packet_addr = guest_addr + pos * 4;
    const uint32_t header = read_be(packet_addr);
    ++pos;
    const uint32_t left = dwords - pos;
    const uint32_t payload = packet_addr + 4;
    switch (header >> 30) {
      case pm4::kType0: {
        if (header == 0) break;  // an all-zero dword is a one-dword no-op (padding)
        const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
        if (count > left) return incomplete(packet_pos, count, "type-0");
        if (observer_) observer_(pm4::kType0, 0, count + 1);
        const uint32_t base = header & 0x7FFF;
        const bool one_reg = (header & 0x8000) != 0;
        for (uint32_t i = 0; i < count; ++i) {
          LR_TRY(write_register(one_reg ? base : base + i, read_be(payload + 4 * i)));
        }
        pos += count;
        break;
      }
      case pm4::kType1: {
        if (left < 2) return incomplete(packet_pos, 2, "type-1");
        if (observer_) observer_(pm4::kType1, 0, 3);
        LR_TRY(write_register(header & 0x7FF, read_be(payload)));
        LR_TRY(write_register((header >> 11) & 0x7FF, read_be(payload + 4)));
        pos += 2;
        break;
      }
      case pm4::kType2:
        if (observer_) observer_(pm4::kType2, 0, 1);
        break;
      default: {
        const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
        const uint32_t opcode = (header >> 8) & 0x7F;
        if (count > left) return incomplete(packet_pos, count, op_label(opcode));
        ++counts_[opcode];
        if (observer_) observer_(pm4::kType3, opcode, count + 1);
        // Predicated packets run only for the selected bins.
        if ((header & 1) == 0 || (bin_select_ & bin_mask_) != 0) {
          Result<void> r = execute_type3(opcode, payload, count, depth);
          if (!r) {
            if (r.error().code != ErrorCode::InvalidFormat) return r.error();
            return malformed(r.error().message + " (packet at " + hex(packet_addr) + ")");
          }
        }
        pos += count;
        break;
      }
    }
  }
  return dwords;
}

Result<void> CommandProcessor::execute_type3(uint32_t opcode, uint32_t payload, uint32_t count, uint32_t depth) {
  // payload .. payload + 4 * count is inside the (bounds-checked) buffer.
  auto p = [&](uint32_t i) { return read_be(payload + 4 * i); };
  auto need = [&](uint32_t n) -> Result<void> {
    if (count >= n) return {};
    return malformed("PM4 " + op_label(opcode) + " packet has " + std::to_string(count) + " dwords, needs " +
                     std::to_string(n));
  };
  auto set_event = [&](uint32_t initiator) { return write_register(VGT_EVENT_INITIATOR, initiator & 0x3F); };
  auto write_consecutive = [&](uint32_t base, uint32_t first) -> Result<void> {
    for (uint32_t i = first; i < count; ++i) LR_TRY(write_register(base + (i - first), p(i)));
    return {};
  };
  auto constant_base = [&](uint32_t offset_type) -> Result<uint32_t> {
    const uint32_t type = (offset_type >> 16) & 0xFF;
    if (type >= std::size(kConstantBases)) {
      return malformed("PM4 " + op_label(opcode) + " has unknown constant type " + std::to_string(type));
    }
    return kConstantBases[type] + (offset_type & 0x7FF);
  };
  auto shader_stage = [&](uint32_t type) -> Result<ShaderStage> {
    if (type == 0) return ShaderStage::Vertex;
    if (type == 1) return ShaderStage::Pixel;
    return malformed("PM4 " + op_label(opcode) + " has unknown shader type " + std::to_string(type));
  };
  // DRAW_INDX / DRAW_INDX_2: the initiator is payload dword `at`, the DMA index buffer (if any) follows it.
  auto draw = [&](uint32_t at) -> Result<void> {
    const uint32_t initiator = p(at);
    LR_TRY(write_register(VGT_DRAW_INITIATOR, initiator));
    DrawInfo info{initiator & 0x3F, initiator >> 16, 0, 0, false, initiator};
    const uint32_t source = (initiator >> 6) & 3;
    if (source == kSourceDma) {
      LR_TRY(need(at + 3));
      const uint32_t dma_base = p(at + 1), dma_size = p(at + 2);
      info.indexed = true;
      info.index_size = ((initiator >> 11) & 1) != 0 ? 4 : 2;
      info.index_base_phys = dma_base & ~(info.index_size - 1);
      info.index_endian = dma_size >> 30;
      info.index_words = dma_size & 0xFFFFFF;
      LR_TRY(write_register(VGT_DMA_BASE, dma_base));
      LR_TRY(write_register(VGT_DMA_SIZE, dma_size));
    } else if (source != kSourceAutoIndex) {
      return malformed("PM4 " + op_label(opcode) + " uses unsupported index source " + std::to_string(source) +
                       (source == kSourceImmediate ? " (immediate)" : ""));
    }
    const uint32_t edram_mode = regs_.read(RB_MODECONTROL) & 7;
    if (edram_mode == kEdramModeCopy) {
      backend_.resolve(regs_);
    } else if (edram_mode == kEdramModeColorDepth || edram_mode == kEdramModeDepthOnly) {
      backend_.draw(regs_, info);
    }  // other modes (0 = ignore): nothing reaches the render targets
    return {};
  };

  switch (opcode) {
    case pm4::ME_INIT:  // microengine init words: nothing to emulate
    case pm4::NOP:
    case pm4::INVALIDATE_STATE:
    case pm4::CONTEXT_UPDATE:
    case pm4::SET_BIN_BASE_OFFSET:
      return {};

    case pm4::WAIT_FOR_IDLE:
      backend_.wait_idle();
      return {};

    case pm4::INDIRECT_BUFFER:
    case pm4::INDIRECT_BUFFER_PFD: {
      LR_TRY(need(2));
      if (depth >= pm4::kMaxIndirectDepth) {
        return malformed("INDIRECT_BUFFER nested deeper than " + std::to_string(pm4::kMaxIndirectDepth) + " levels");
      }
      const uint32_t phys = p(0);
      const uint32_t length = p(1) & 0xFFFFF;  // dwords
      if (length == 0) return {};
      if ((phys & 3) != 0) return malformed("INDIRECT_BUFFER address " + hex(phys) + " is not 4-byte aligned");
      LR_TRY_ASSIGN(guest, translate(phys, uint64_t{length} * 4, "INDIRECT_BUFFER"));
      LR_TRY(execute_buffer(guest, length, depth + 1));
      return {};
    }

    case pm4::WAIT_REG_MEM: {
      LR_TRY(need(5));
      const uint32_t wait_info = p(0), poll = p(1), ref = p(2), mask = p(3);
      // Spins (yielding, then sleeping briefly) until the condition holds. Escape: request_stop() -> Cancelled,
      // so the GPU thread can always be shut down.
      for (uint32_t spins = 0;; ++spins) {
        LR_TRY_ASSIGN(ok, compare(wait_info, poll, ref, mask));
        if (ok) return {};
        if (stop_.load(std::memory_order_acquire)) {
          return Error{ErrorCode::Cancelled, "PM4 WAIT_REG_MEM interrupted by a stop request", {}};
        }
        if (spins < 64) {
          std::this_thread::yield();
        } else {
          std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
      }
    }

    case pm4::REG_RMW: {
      LR_TRY(need(3));
      const uint32_t info = p(0);
      const uint32_t and_value = (info & 0x80000000u) != 0 ? regs_.read(p(1) & 0x1FFF) : p(1);
      const uint32_t or_value = (info & 0x40000000u) != 0 ? regs_.read(p(2) & 0x1FFF) : p(2);
      const uint32_t target = info & 0x1FFF;
      return write_register(target, (regs_.read(target) & and_value) | or_value);
    }

    case pm4::COND_WRITE: {
      LR_TRY(need(6));
      const uint32_t wait_info = p(0);
      LR_TRY_ASSIGN(ok, compare(wait_info, p(1), p(2), p(3)));
      if (!ok) return {};
      if ((wait_info & 0x100) != 0) return store_mem(p(4), p(5), "COND_WRITE address");
      return write_register(p(4), p(5));
    }

    case pm4::EVENT_WRITE:
    case pm4::EVENT_WRITE_ZPD:
      // ZPD (occlusion sample counts) is not emulated beyond the event itself.
      LR_TRY(need(1));
      return set_event(p(0));

    case pm4::EVENT_WRITE_SHD: {
      LR_TRY(need(3));
      const uint32_t initiator = p(0);
      LR_TRY(set_event(initiator));
      // Bit 31: store the swap counter (frames presented so far) instead of the packet's value.
      const uint32_t data = (initiator & 0x80000000u) != 0 ? swap_counter_ : p(2);
      return store_mem(p(1), data, "EVENT_WRITE_SHD address");
    }

    case pm4::EVENT_WRITE_EXT: {
      LR_TRY(need(2));
      LR_TRY(set_event(p(0)));
      const uint32_t address = p(1);
      LR_TRY_ASSIGN(guest, translate(address & ~3u, 12, "EVENT_WRITE_EXT address"));
      // Screen extents (min x, max x, min y, max y, min z, max z) as 16-bit values.
      const uint16_t extents[6] = {0, kExtentMaxXY, 0, kExtentMaxXY, 0, 1};
      for (size_t i = 0; i < 6; ++i) {
        uint16_t v = extents[i];
        if ((address & 3) == pm4::kEndian8in16) v = static_cast<uint16_t>((v << 8) | (v >> 8));
        std::memcpy(mem_.base + guest + 2 * i, &v, sizeof v);
      }
      return {};
    }

    case pm4::MEM_WRITE: {
      LR_TRY(need(1));
      uint32_t address = p(0);
      for (uint32_t i = 1; i < count; ++i, address += 4) LR_TRY(store_mem(address, p(i), "MEM_WRITE address"));
      return {};
    }

    case pm4::REG_TO_MEM: {
      LR_TRY(need(2));
      return store_mem(p(1), regs_.read(p(0)), "REG_TO_MEM address");
    }

    case pm4::INTERRUPT: {
      LR_TRY(need(1));
      // One command interrupt (source 1) per CPU bit set in the mask, delivered on that CPU.
      const uint32_t cpu_mask = p(0);
      for (uint32_t cpu = 0; cpu < 6; ++cpu) {
        if ((cpu_mask & (1u << cpu)) != 0 && raise_interrupt_) raise_interrupt_(1, cpu);
      }
      return {};
    }

    case pm4::SET_CONSTANT: {
      LR_TRY(need(1));
      LR_TRY_ASSIGN(base, constant_base(p(0)));
      return write_consecutive(base, 1);
    }

    case pm4::SET_CONSTANT2:
    case pm4::SET_SHADER_CONSTANTS:
      LR_TRY(need(1));
      return write_consecutive(p(0) & 0xFFFF, 1);

    case pm4::LOAD_ALU_CONSTANT: {
      LR_TRY(need(3));
      const uint32_t phys = p(0) & 0x3FFFFFFF;
      LR_TRY_ASSIGN(base, constant_base(p(1)));
      const uint32_t size = p(2) & 0xFFF;  // dwords
      if (size == 0) return {};
      LR_TRY_ASSIGN(guest, translate(phys, uint64_t{size} * 4, "LOAD_ALU_CONSTANT"));
      for (uint32_t i = 0; i < size; ++i) LR_TRY(write_register(base + i, read_be(guest + 4 * i)));
      return {};
    }

    case pm4::IM_LOAD: {
      LR_TRY(need(2));
      LR_TRY_ASSIGN(stage, shader_stage(p(0) & 3));
      const uint32_t phys = p(0) & ~3u;
      const uint32_t size = p(1) & 0xFFFF;  // dwords; bits 16-31 (start) are always 0 in practice
      LR_TRY_ASSIGN(guest, translate(phys, uint64_t{size} * 4, "IM_LOAD"));
      backend_.load_shader(stage, guest, size);
      return {};
    }

    case pm4::IM_LOAD_IMMEDIATE: {
      LR_TRY(need(2));
      LR_TRY_ASSIGN(stage, shader_stage(p(0)));
      const uint32_t size = p(1) & 0xFFFF;
      LR_TRY(need(2 + size));
      backend_.load_shader(stage, payload + 8, size);
      return {};
    }

    case pm4::DRAW_INDX:
      LR_TRY(need(2));  // dword 0 is the viz-query token
      return draw(1);

    case pm4::DRAW_INDX_2:
      LR_TRY(need(1));
      return draw(0);

    case pm4::VIZ_QUERY: {
      LR_TRY(need(1));
      const uint32_t id = p(0) & 0x3F;
      if ((p(0) & 0x100) == 0) return write_register(VGT_EVENT_INITIATOR, kEventVizQueryStart);
      LR_TRY(write_register(VGT_EVENT_INITIATOR, kEventVizQueryEnd));
      // No occlusion data: every query reports "visible".
      const uint32_t reg = id < 32 ? PA_SC_VIZ_QUERY_STATUS_0 : PA_SC_VIZ_QUERY_STATUS_1;
      return write_register(reg, regs_.read(reg) | (1u << (id % 32)));
    }

    case pm4::SET_BIN_MASK:
      LR_TRY(need(2));
      bin_mask_ = (uint64_t{p(0)} << 32) | p(1);
      return {};
    case pm4::SET_BIN_SELECT:
      LR_TRY(need(2));
      bin_select_ = (uint64_t{p(0)} << 32) | p(1);
      return {};
    case pm4::SET_BIN_MASK_LO:
      LR_TRY(need(1));
      bin_mask_ = (bin_mask_ & ~uint64_t{0xFFFFFFFF}) | p(0);
      return {};
    case pm4::SET_BIN_MASK_HI:
      LR_TRY(need(1));
      bin_mask_ = (bin_mask_ & 0xFFFFFFFFu) | (uint64_t{p(0)} << 32);
      return {};
    case pm4::SET_BIN_SELECT_LO:
      LR_TRY(need(1));
      bin_select_ = (bin_select_ & ~uint64_t{0xFFFFFFFF}) | p(0);
      return {};
    case pm4::SET_BIN_SELECT_HI:
      LR_TRY(need(1));
      bin_select_ = (bin_select_ & 0xFFFFFFFFu) | (uint64_t{p(0)} << 32);
      return {};

    case pm4::LR_SWAP:
      LR_TRY(need(4));
      if (p(0) != pm4::kSwapMagic) return malformed("LR_SWAP packet without the 'LRSW' signature: " + hex(p(0)));
      backend_.swap(p(1), p(2), p(3));
      ++swap_counter_;
      return {};

    default:
      return malformed("unknown PM4 opcode " + hex(opcode, 2));
  }
}

}  // namespace lr::gpu
