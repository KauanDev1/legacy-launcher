// SPDX-License-Identifier: MIT
// PM4 command processor: decodes the packet stream the guest writes into its ring buffer and indirect buffers,
// applies register writes to the RegisterFile, writes fences into guest memory and drives a Backend. Packet
// semantics per Xenia command_processor.cc / xenos.h (reference only; no code taken from it).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>

#include "common/result.h"
#include "runtime/gpu/backend.h"
#include "runtime/gpu/registers.h"

namespace lr::gpu {

// How the command processor reaches guest memory. Guest memory is big-endian; base points at guest address 0.
struct MemoryAccess {
  uint8_t* base;
  // Maps a physical address taken from a packet to a guest address; nullopt when it maps to no guest memory.
  std::function<std::optional<uint32_t>(uint32_t phys)> to_guest;
  // Bytes of guest memory reachable from base (the runtime maps the whole 4 GiB space, GuestMemory::size()).
  // Every access is checked against it: [guest, guest + n) must lie in [0, size). Interface addition (Task 8).
  uint64_t size = 0;
};

namespace pm4 {

inline constexpr uint32_t kType0 = 0, kType1 = 1, kType2 = 2, kType3 = 3;

// Type-3 opcodes (header bits 8-14). Numbers per Xenia xenos.h (reference only; they are hardware facts).
enum Opcode : uint32_t {
  NOP = 0x10,
  REG_RMW = 0x21,
  DRAW_INDX = 0x22,
  VIZ_QUERY = 0x23,
  WAIT_FOR_IDLE = 0x26,
  IM_LOAD = 0x27,
  IM_LOAD_IMMEDIATE = 0x2B,
  SET_CONSTANT = 0x2D,
  LOAD_ALU_CONSTANT = 0x2F,
  DRAW_INDX_2 = 0x36,
  INDIRECT_BUFFER_PFD = 0x37,
  INVALIDATE_STATE = 0x3B,
  WAIT_REG_MEM = 0x3C,
  MEM_WRITE = 0x3D,
  REG_TO_MEM = 0x3E,
  INDIRECT_BUFFER = 0x3F,
  COND_WRITE = 0x45,
  EVENT_WRITE = 0x46,
  ME_INIT = 0x48,
  SET_BIN_BASE_OFFSET = 0x4B,
  SET_BIN_MASK = 0x50,
  SET_BIN_SELECT = 0x51,
  INTERRUPT = 0x54,
  SET_CONSTANT2 = 0x55,
  SET_SHADER_CONSTANTS = 0x56,
  EVENT_WRITE_SHD = 0x58,
  EVENT_WRITE_EXT = 0x5A,
  EVENT_WRITE_ZPD = 0x5B,
  CONTEXT_UPDATE = 0x5E,
  SET_BIN_MASK_LO = 0x60,
  SET_BIN_MASK_HI = 0x61,
  SET_BIN_SELECT_LO = 0x62,
  SET_BIN_SELECT_HI = 0x63,
  // Not a hardware opcode: our HLE VdSwap emits it (payload: frontbuffer physical address, width, height). Calls
  // Backend::swap and advances the swap counter EVENT_WRITE_SHD reports.
  LR_SWAP = 0x64,
};

// Name of a known opcode ("DRAW_INDX"), nullptr otherwise.
const char* opcode_name(uint32_t opcode);

// Endian modes in the low 2 bits of fence/poll addresses (xenos Endian).
inline constexpr uint32_t kEndianNone = 0, kEndian8in16 = 1, kEndian8in32 = 2, kEndian16in32 = 3;
uint32_t gpu_swap(uint32_t value, uint32_t endian);

inline constexpr uint32_t kMaxIndirectDepth = 4;  // nested INDIRECT_BUFFER levels below the ring buffer

}  // namespace pm4

// Thread-safety: execute() and opcode_counts() belong to the GPU thread (or run after it stopped);
// request_stop() may be called from any thread.
class CommandProcessor {
 public:
  CommandProcessor(MemoryAccess mem, RegisterFile& regs, Backend& backend,
                   std::function<void(uint32_t source)> raise_interrupt);

  // Executes the `dwords` packet dwords at guest address `guest_addr` (4-byte aligned). Returns the dwords
  // consumed (all of them) or the first fatal error: unknown type-3 opcode ("unknown PM4 opcode 0x.."), a packet
  // running past the end of the buffer or too short for its opcode, an address outside guest memory, indirect
  // buffers nested deeper than pm4::kMaxIndirectDepth (all InvalidFormat), or Cancelled when request_stop()
  // ended a WAIT_REG_MEM. A packet split across the end of the buffer is malformed: the caller hands over whole
  // packets (a ring-buffer wrap is the caller's business).
  Result<uint32_t> execute(uint32_t guest_addr, uint32_t dwords);

  // Type-3 opcode -> packets seen (including a final unknown one), for --trace-gpu and fatal reports.
  std::map<uint32_t, uint64_t> opcode_counts() const { return counts_; }

  // Escape hatch for shutdown: a WAIT_REG_MEM that is spinning (or the next one) returns Cancelled. Sticky.
  void request_stop() { stop_.store(true, std::memory_order_release); }

  // Frames swapped through LR_SWAP so far (the value EVENT_WRITE_SHD writes when initiator bit 31 is set).
  uint32_t swap_counter() const { return swap_counter_; }

 private:
  Result<void> execute_buffer(uint32_t guest_addr, uint32_t dwords, uint32_t depth);
  Result<void> execute_type3(uint32_t opcode, uint32_t payload_addr, uint32_t count, uint32_t depth);

  bool in_bounds(uint32_t guest, uint64_t bytes) const;
  uint32_t read_be(uint32_t guest) const;  // caller checked bounds
  Result<uint32_t> translate(uint32_t phys, uint64_t bytes, const char* what) const;
  Result<uint32_t> load_mem(uint32_t phys_and_endian, const char* what) const;
  Result<void> store_mem(uint32_t phys_and_endian, uint32_t value, const char* what);
  Result<bool> compare(uint32_t wait_info, uint32_t poll, uint32_t ref, uint32_t mask);
  void write_register(uint32_t index, uint32_t value);

  MemoryAccess mem_;
  RegisterFile& regs_;
  Backend& backend_;
  std::function<void(uint32_t)> raise_interrupt_;
  std::map<uint32_t, uint64_t> counts_;
  std::atomic<bool> stop_{false};
  uint32_t swap_counter_ = 0;
  uint64_t bin_mask_ = ~uint64_t{0};
  uint64_t bin_select_ = ~uint64_t{0};
};

}  // namespace lr::gpu
